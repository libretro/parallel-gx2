#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "util/helpers/MpmcQueue.h"
#include "util/helpers/EventWake.h"
#include "Cemu/FileCache/FileCache.h"

#include "Cafe/HW/Latte/Renderer/Vulkan/RslcGlsl.h"
#include <gfx/shaders/rslc.h>
#include "util/helpers/helpers.h"
#include "Cafe/HW/Latte/Core/PsoTrace.h"

bool s_isLoadingShadersVk{ false };
class FileCache* s_spirvCache{nullptr};

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

// The compile queue holds a ClaimTicket for each shader (ClaimTicket.h):
// whoever compiles it - a compile thread, PreponeCompilation on the thread
// that needs it now, or the destructor of one never compiled - claims it.
// Every shader's DONE notifies it; each waiter has its own shader to check
static ImmortalEventWake s_shaderCompiledWake;

class _ShaderVkThreadPool
{
public:
	void StartThreads()
	{
		if (m_threadsActive.exchange(true))
			return;
		// create thread pool
		const uint32 threadCount = 2;
		for (uint32 i = 0; i < threadCount; ++i)
			s_threads.emplace_back(&_ShaderVkThreadPool::CompilerThreadFunc, this);
	}

	void StopThreads()
	{
		if (!m_threadsActive.exchange(false))
			return;
		// one empty ticket per thread, each of which leaves at one
		for (uint32 i = 0; i < s_threads.size(); ++i)
			s_compilationQueue.push(nullptr);
		for (auto& it : s_threads)
			it.join();
		s_threads.clear();
	}

	~_ShaderVkThreadPool()
	{
		StopThreads();
	}

	void CompilerThreadFunc()
	{
		SetThreadName("vkShaderComp");
		while (true)
		{
			ShaderCompileTicket* ticket = s_compilationQueue.pop();
			if (!ticket)
			{
				if (!m_threadsActive.load(std::memory_order::relaxed))
					return;
				continue;
			}
			RendererShaderVk* job = ticket->Claim();
			ticket->Release();
			if (!job)
				continue; // compiled synchronously via PreponeCompilation(), or gone
			cemu_assert_debug(job->m_compilationState.load() == RendererShaderVk::COMPILATION_STATE::QUEUED);
			job->m_compilationState.store(RendererShaderVk::COMPILATION_STATE::COMPILING, std::memory_order_release);
			job->CompileInternal(false);
			++g_compiled_shaders_async;
			job->SetCompiled();
		}
	}

	bool HasThreadsRunning() const { return m_threadsActive; }

public:
	std::vector<std::thread> s_threads;
	MpmcQueue<ShaderCompileTicket*> s_compilationQueue;

private:
	std::atomic<bool> m_threadsActive;
}ShaderVkThreadPool;

RendererShaderVk::RendererShaderVk(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslCode)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_glslCode(glslCode)
{
	// start async compilation
	m_compilationState.store(COMPILATION_STATE::QUEUED, std::memory_order_relaxed);
	m_ticket = new ShaderCompileTicket(this);
	ShaderVkThreadPool.s_compilationQueue.push(m_ticket);
	cemu_assert_debug(ShaderVkThreadPool.HasThreadsRunning()); // make sure .StartThreads() was called
}

RendererShaderVk::~RendererShaderVk()
{
	if (m_ticket)
	{
		// Never compiled: the queue lets go of the ticket without touching this.
		// Being compiled: done first, the compile thread uses this until then.
		if (m_ticket->Claim() == nullptr)
			WaitUntilCompiled();
		m_ticket->Release();
		m_ticket = nullptr;
	}
	list_pipelineInfo.ForEachTaken([](PipelineInfo* p) { delete p; });

	VkDevice vkDev = VulkanRenderer::GetInstance()->GetLogicalDevice();
	vkDestroyShaderModule(vkDev, m_shader_module, nullptr);
}

void RendererShaderVk::Init()
{
	ShaderVkThreadPool.StartThreads();
}

void RendererShaderVk::Shutdown()
{
	ShaderVkThreadPool.StopThreads();
}

void RendererShaderVk::CreateVkShaderModule(std::span<uint32> spirvBuffer)
{
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = spirvBuffer.size_bytes();
	createInfo.pCode = spirvBuffer.data();

	VulkanRenderer* vkr = (VulkanRenderer*)g_renderer.get();

	VkDevice m_device = vkr->GetLogicalDevice();

	VkResult result = vkCreateShaderModule(m_device, &createInfo, nullptr, &m_shader_module);
	if (result != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan: Shader error");
		throw std::runtime_error(fmt::format("Failed to create shader module: {}", result));
	}

	// set debug name
	if (vkr->IsDebugMarkersEnabled())
	{
		VkDebugUtilsObjectNameInfoEXT objName{};
		objName.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		objName.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
		objName.pNext = nullptr;
		objName.objectHandle = (uint64_t)m_shader_module;
		auto objNameStr = fmt::format("shader_{:016x}_{:016x}", m_baseHash, m_auxHash);
		objName.pObjectName = objNameStr.c_str();
		vkSetDebugUtilsObjectNameEXT(vkr->GetLogicalDevice(), &objName);
	}
}

void RendererShaderVk::FinishCompilation()
{
	m_glslCode.clear();
	m_glslCode.shrink_to_fit();
}

void RendererShaderVk::CompileInternal(bool isRenderThread)
{
	// try to retrieve SPIR-V module from cache
	if (s_isLoadingShadersVk && (m_isGameShader && !m_isGfxPackShader) && s_spirvCache)
	{
		cemu_assert_debug(m_baseHash != 0);
		uint64 h1, h2;
		GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, h1, h2);
		std::vector<uint8> cacheFileData;
		if (s_spirvCache->GetFile({ h1, h2 }, cacheFileData))
		{
			// generate shader from cached SPIR-V buffer
			CreateVkShaderModule(std::span<uint32>((uint32*)cacheFileData.data(), cacheFileData.size() / sizeof(uint32)));
			FinishCompilation();
			return;
		}
	}

	enum rslc_stage stage;
	switch (GetType())
	{
	case ShaderType::kVertex:
		stage = RSLC_STAGE_VERTEX;
		break;
	case ShaderType::kFragment:
		stage = RSLC_STAGE_FRAGMENT;
		break;
	case ShaderType::kGeometry:
		stage = RSLC_STAGE_GEOMETRY;
		break;
	default:
		cemu_assert_debug(false);
		FinishCompilation();
		return;
	}

	const std::string glsl = RslcGlsl(m_glslCode, VulkanRenderer::GetInstance()->HasSPRIVRoundingModeRTE32());
	char err[512];
	err[0] = '\0';
	rslc_opts opts{};
	opts.dst = RSLC_DST_SPIRV;
	opts.err = err;
	opts.err_cap = sizeof(err);
	struct Result : rslc_result
	{
		const rslc_opts* opts;
		~Result() { rslc_result_free(this, opts); }
	} result{};
	result.opts = &opts;
	if (!rslc_compile(glsl.data(), glsl.size(), stage, &opts, &result))
	{
		cemuLog_log(LogType::Force, fmt::format("GLSL compilation failed for {:016x}_{:016x}: \"{}\"", m_baseHash, m_auxHash, err));
		cemuLog_logDebug(LogType::Force, "GLSL source:\n{}", m_glslCode);
		cemu_assert_debug(false);
		FinishCompilation();
		return;
	}
	std::span<uint32> spirv((uint32*)result.spirv, result.spirv_len);

	// store in cache, unless it is a modified shader from a gfx pack
	if (s_spirvCache && m_isGameShader && m_isGfxPackShader == false)
	{
		uint64 h1, h2;
		GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, h1, h2);
		s_spirvCache->AddFile({ h1, h2 }, (const uint8*)spirv.data(), spirv.size_bytes());
	}

	CreateVkShaderModule(spirv);

	// count compiled shader
	if (!s_isLoadingShadersVk)
	{
		if( m_isGameShader )
			++g_compiled_shaders_total;
	}

	FinishCompilation();
}

void RendererShaderVk::PreponeCompilation(bool isRenderThread)
{
	// Taken out of its ticket here, it is this thread's to compile; already
	// taken, a compile thread has it (or had it)
	const bool isStillQueued = m_ticket && m_ticket->Claim() != nullptr;
	if (!isStillQueued)
	{
		PSO_TRACE_SCOPE(isRenderThread ? "shader wait for its compile thread" : "shader wait (worker)", m_baseHash);
		WaitUntilCompiled();
		--g_compiled_shaders_async; // compilation caused a stall so we don't consider this one async
		return;
	}
	// compile synchronously
	PSO_TRACE_SCOPE(isRenderThread ? "shader compile on the GPU thread" : "shader compile (worker)", m_baseHash);
	m_compilationState.store(COMPILATION_STATE::COMPILING, std::memory_order_release);
	CompileInternal(isRenderThread);
	SetCompiled();
}

void RendererShaderVk::SetCompiled()
{
	m_compilationState.store(COMPILATION_STATE::DONE, std::memory_order_release);
	s_shaderCompiledWake->Notify();
}

void RendererShaderVk::WaitUntilCompiled()
{
	s_shaderCompiledWake->WaitUntil([this] {
		return m_compilationState.load(std::memory_order_acquire) == COMPILATION_STATE::DONE;
	});
}

bool RendererShaderVk::IsCompiled()
{
	return m_compilationState.load(std::memory_order_acquire) == COMPILATION_STATE::DONE;
};

bool RendererShaderVk::WaitForCompiled()
{
	WaitUntilCompiled();
	return true;
}

void RendererShaderVk::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
	if (s_spirvCache)
	{
		delete s_spirvCache;
		s_spirvCache = nullptr;
	}
	uint32 spirvCacheMagic = GeneratePrecompiledCacheId();
	const std::string cacheFilename = fmt::format("{:016x}_spirv.bin", cacheTitleId);
	const fs::path cachePath = ActiveSettings::GetCachePath("shaderCache/precompiled/{}", cacheFilename);
	s_spirvCache = FileCache::Open(cachePath, true, spirvCacheMagic);
	if (s_spirvCache == nullptr)
		cemuLog_log(LogType::Force, "Unable to open SPIR-V cache {}", cacheFilename);
	s_isLoadingShadersVk = true;
}

void RendererShaderVk::ShaderCacheLoading_end()
{
	// keep g_spirvCache open since we will write to it while the game is running
	s_isLoadingShadersVk = false;
}

void RendererShaderVk::ShaderCacheLoading_Close()
{
    delete s_spirvCache;
    s_spirvCache = nullptr;
}
