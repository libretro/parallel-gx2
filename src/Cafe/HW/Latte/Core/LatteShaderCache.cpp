#include "Cafe/CafeSystem.h"
#include "Common/FileStream.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/GameProfile/GameProfile.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#ifdef ENABLE_OPENGL
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#endif
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#endif
#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.h"
#endif


#include "config/ActiveSettings.h"
#include "Cafe/TitleList/GameInfo.h"

#include "util/helpers/SystemException.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cafe/HW/Latte/Common/ShaderSerializer.h"
#include "util/helpers/Serializer.h"

#include <thread>

#if BOOST_OS_WINDOWS
#include <psapi.h>
#endif

// Defined at global scope by the libretro glue (src/libretro/CemuLibretro.cpp).
bool libretro_gpu_context_gone();

#define SHADER_CACHE_COMPILE_QUEUE_SIZE		(32)

struct
{
	sint32 compiledShaderCount;
	// number of loaded shaders
	sint32 vertexShaderCount;
	sint32 geometryShaderCount;
	sint32 pixelShaderCount;
}shaderCacheScreenStats;

struct
{
	// shader loading
	sint32 loadedShaderFiles;
	sint32 shaderFileCount;
	// pipeline loading
	uint32 loadedPipelines;
	sint32 pipelineFileCount;
}g_shaderCacheLoaderState;

FileCache* s_shaderCacheGeneric = nullptr;	// contains hardware and version independent shader information

#define SHADER_CACHE_GENERIC_EXTRA_VERSION		2 // changing this constant will invalidate all hardware-independent cache files

#define SHADER_CACHE_TYPE_VERTEX				(0)
#define SHADER_CACHE_TYPE_GEOMETRY				(1)
#define SHADER_CACHE_TYPE_PIXEL					(2)

bool LatteShaderCache_readSeparableShader(uint8* shaderInfoData, sint32 shaderInfoSize);
void LatteShaderCache_LoadPipelineCache(uint64 cacheTitleId);
bool LatteShaderCache_updatePipelineLoadingProgress();
void LatteShaderCache_ShowProgress(const std::function <bool(void)>& loadUpdateFunc, bool isPipelines);

struct
{
	struct
	{
		LatteDecompilerShader* shader;
	}entry[SHADER_CACHE_COMPILE_QUEUE_SIZE];
	sint32 count;
}shaderCompileQueue;

void LatteShaderCache_initCompileQueue()
{
	shaderCompileQueue.count = 0;
}

void LatteShaderCache_addToCompileQueue(LatteDecompilerShader* shader)
{
	cemu_assert(shaderCompileQueue.count < SHADER_CACHE_COMPILE_QUEUE_SIZE);
	shaderCompileQueue.entry[shaderCompileQueue.count].shader = shader;
	shaderCompileQueue.count++;
}

void LatteShaderCache_removeFromCompileQueue(sint32 index)
{
	for (sint32 i = index; i<shaderCompileQueue.count-1; i++)
		shaderCompileQueue.entry[i].shader = shaderCompileQueue.entry[i + 1].shader;
	shaderCompileQueue.count--;
}

/*
 * Process entries from compile queue until there are equal or less entries
 * left than specified by maxRemainingEntries
 */
void LatteShaderCache_updateCompileQueue(sint32 maxRemainingEntries)
{
	while (true)
	{
		if (shaderCompileQueue.count <= maxRemainingEntries)
			break;
		auto shader = shaderCompileQueue.entry[0].shader;
		if (shader)
			LatteShader_FinishCompilation(shader);
		LatteShaderCache_removeFromCompileQueue(0);
	}
}

void LatteShaderCache_finish()
{
	switch (g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
		RendererShaderVk::ShaderCacheLoading_end();
		return;
#endif
#ifdef ENABLE_OPENGL
	case RendererAPI::OpenGL:
		RendererShaderGL::ShaderCacheLoading_end();
		return;
#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		RendererShaderMtl::ShaderCacheLoading_end();
		return;
#endif
	}
}

uint32 LatteShaderCache_getShaderCacheExtraVersion(uint64 titleId)
{
	// encode the titleId in the version to prevent users from swapping caches between titles
	const uint32 cacheFileVersion = 1;
	uint32 extraVersion = ((uint32)(titleId >> 32) + ((uint32)titleId) * 3) + cacheFileVersion + 0xe97af1ad;
	return extraVersion;
}

uint32 LatteShaderCache_getPipelineCacheExtraVersion(uint64 titleId)
{
	const uint32 cacheFileVersion = 1;
	uint32 extraVersion = ((uint32)(titleId >> 32) + ((uint32)titleId) * 3) + cacheFileVersion;
	return extraVersion;
}

void LatteShaderCache_Load()
{
	shaderCacheScreenStats.compiledShaderCount = 0;
	shaderCacheScreenStats.vertexShaderCount = 0;
	shaderCacheScreenStats.geometryShaderCount = 0;
	shaderCacheScreenStats.pixelShaderCount = 0;

	uint64 cacheTitleId = CafeSystem::GetForegroundTitleId();

	const auto timeLoadStart = now_cached();
	// remember current amount of committed memory
#if BOOST_OS_WINDOWS
	PROCESS_MEMORY_COUNTERS pmc1;
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc1, sizeof(PROCESS_MEMORY_COUNTERS));
	LONGLONG totalMem1 = pmc1.PagefileUsage;
#endif
	// init shader parallel compile queue
	LatteShaderCache_initCompileQueue();
	// create directories
	FileStream::CreateDirectories(ActiveSettings::GetCachePath("shaderCache/transferable"));
	FileStream::CreateDirectories(ActiveSettings::GetCachePath("shaderCache/precompiled"));
	// initialize renderer specific caches
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
		RendererShaderVk::ShaderCacheLoading_begin(cacheTitleId);
		break;
#endif
#ifdef ENABLE_OPENGL
	case RendererAPI::OpenGL:
		RendererShaderGL::ShaderCacheLoading_begin(cacheTitleId);
		break;
#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		RendererShaderMtl::ShaderCacheLoading_begin(cacheTitleId);
		break;
#endif
	}

	// get cache file name
	fs::path pathGeneric;
	switch(g_renderer->GetType())
	{
	case RendererAPI::Metal:
	    pathGeneric = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_mtlshaders.bin", cacheTitleId);
		break;
	default:
	    pathGeneric = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_shaders.bin", cacheTitleId);
		break;
	}

	// calculate extraVersion for transferable and precompiled shader cache
	uint32 transferableExtraVersion = SHADER_CACHE_GENERIC_EXTRA_VERSION;
    s_shaderCacheGeneric = FileCache::Open(pathGeneric, false, transferableExtraVersion); // legacy extra version (1.25.0 - 1.25.1b)
	if(!s_shaderCacheGeneric)
        s_shaderCacheGeneric = FileCache::Open(pathGeneric, true, LatteShaderCache_getShaderCacheExtraVersion(cacheTitleId));
	if(!s_shaderCacheGeneric)
	{
		// no shader cache available yet
		cemuLog_log(LogType::Force, "Unable to open or create shader cache file \"{}\"", _pathToUtf8(pathGeneric));
		LatteShaderCache_finish();
		return;
	}
	s_shaderCacheGeneric->UseCompression(false);

	// load/compile cached shaders
	sint32 entryCount = s_shaderCacheGeneric->GetMaximumFileIndex();
	g_shaderCacheLoaderState.shaderFileCount = s_shaderCacheGeneric->GetFileCount();
	g_shaderCacheLoaderState.loadedShaderFiles = 0;

	sint32 numLoadedShaders = 0;
	uint32 loadIndex = 0;
	// one buffer for every entry: it grows to the largest and stays
	std::vector<uint8> fileData;

	auto LoadShadersUpdate = [&]() -> bool
	{
		if (loadIndex >= (uint32)s_shaderCacheGeneric->GetMaximumFileIndex())
			return false;
		LatteShaderCache_updateCompileQueue(SHADER_CACHE_COMPILE_QUEUE_SIZE - 2);
		uint64 name1;
		uint64 name2;
		if (!s_shaderCacheGeneric->GetFileByIndex(loadIndex, &name1, &name2, fileData))
		{
			loadIndex++;
			return true;
		}
		g_shaderCacheLoaderState.loadedShaderFiles++;
		if (LatteShaderCache_readSeparableShader(fileData.data(), fileData.size()) == false)
		{
			// something is wrong with the stored shader, remove entry from shader cache files
			cemuLog_log(LogType::Force, "Shader cache entry {} invalid, deleting...", loadIndex);
			s_shaderCacheGeneric->DeleteFile({name1, name2 });
		}
		numLoadedShaders++;
		loadIndex++;
		return true;
	};

	const auto timeShadersStart = now_cached();
	LatteShaderCache_ShowProgress(LoadShadersUpdate, false);

	LatteShaderCache_updateCompileQueue(0);
	// Both phases, in the log of every build. "Black screen for two minutes on
	// the second run" arrived as a report with nothing in the log to say which
	// half of this it was, or whether it was this at all - and a first run and
	// a second run differ here precisely because the first one fills the
	// pipeline cache that the second one then has to compile through.
	cemuLog_log(LogType::Force, "Shader cache: {} shaders loaded in {}ms",
		g_shaderCacheLoaderState.loadedShaderFiles,
		(sint32)std::chrono::duration_cast<std::chrono::milliseconds>(now_cached() - timeShadersStart).count());
	// write load time and RAM usage to log file (in dev build)
#if BOOST_OS_WINDOWS
	const auto timeLoadEnd = now_cached();
	const auto timeLoad = std::chrono::duration_cast<std::chrono::milliseconds>(timeLoadEnd - timeLoadStart).count();
	PROCESS_MEMORY_COUNTERS pmc2;
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc2, sizeof(PROCESS_MEMORY_COUNTERS));
	LONGLONG totalMem2 = pmc2.PagefileUsage;
	LONGLONG memCommited = totalMem2 - totalMem1;
	cemuLog_log(LogType::Force, "Shader cache loaded with {} shaders. Commited mem {}MB. Took {}ms", numLoadedShaders, (sint32)(memCommited/1024/1024), timeLoad);
#endif
	LatteShaderCache_finish();
	// Same reason as in ShowProgress: the content can be closed while this
	// is still loading, and everything from here on draws the final frame
	// of the loading screen through the renderer. If it has gone there is
	// nothing left to draw it with, and nothing to clean up either - the
	// textures below belonged to it.
	Renderer* renderer = g_renderer.get();
	if (!renderer || ::libretro_gpu_context_gone())
	{
		return;
	}
	// if Vulkan or Metal then also load pipeline cache
#if defined(ENABLE_VULKAN) || defined(ENABLE_METAL)
	if (renderer->GetType() == RendererAPI::Vulkan || renderer->GetType() == RendererAPI::Metal)
	{
		const auto timePipelinesStart = now_cached();
		LatteShaderCache_LoadPipelineCache(cacheTitleId);
		cemuLog_log(LogType::Force, "Shader cache: {} of {} pipelines loaded in {}ms",
			g_shaderCacheLoaderState.loadedPipelines, g_shaderCacheLoaderState.pipelineFileCount,
			(sint32)std::chrono::duration_cast<std::chrono::milliseconds>(now_cached() - timePipelinesStart).count());
	}
#endif





	if(Latte_GetStopSignal())
		LatteThread_Exit();
}

void LatteShaderCache_ShowProgress(const std::function <bool(void)>& loadUpdateFunc, bool isPipelines)
{
	(void)isPipelines;

	while (true)
	{
        if (Latte_GetStopSignal())
            break; // thread stop requested, cancel shader loading
		// Closing content while this is still loading releases the renderer
		// from under this thread, and everything below draws through it - a
		// null dereference on the GPU thread, right after "dropping the
		// renderer". Take it once and stop if it has gone; release() does not
		// delete, so a pointer read here stays good for the rest of the pass.
		Renderer* renderer = g_renderer.get();
		if (!renderer || ::libretro_gpu_context_gone())
			break;
		// The frontend is taking its graphics context apart and is waiting for
		// this thread at the pause gate. There is no command boundary inside a
		// cache load, so without this the wait is spent on a thread that cannot
		// answer until the load ends - which on a first pipeline cache is
		// minutes, and what it ends with is the context being destroyed under
		// the teardown instead of before it. The load is resumable: what is
		// left of it is loaded again when the context comes back, and on a
		// close there is nothing left to load it for.
		if (Latte_IsGpuPauseRequested())
			break;
		bool r = loadUpdateFunc();
		if (!r)
			break;

		// The loading screen this loop used to draw reached nobody in a
		// libretro core (the renderer has no swapchain of its own), and imgui,
		// which drew it, is gone. The progress reaches the user through the log.
	}
}

void LatteShaderCache_LoadPipelineCache(uint64 cacheTitleId)
{
	switch(g_renderer->GetType())
	{
	#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
	    g_shaderCacheLoaderState.pipelineFileCount = VulkanPipelineStableCache::GetInstance().BeginLoading(cacheTitleId);
	    break;
	#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		g_shaderCacheLoaderState.pipelineFileCount = MetalPipelineCache::GetInstance().BeginLoading(cacheTitleId);
		break;
#endif
	}

	g_shaderCacheLoaderState.loadedPipelines = 0;
	LatteShaderCache_ShowProgress(LatteShaderCache_updatePipelineLoadingProgress, true);

	switch(g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
	    VulkanPipelineStableCache::GetInstance().EndLoading();
	    break;
#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		MetalPipelineCache::GetInstance().EndLoading();
		break;
#endif
	}
}

bool LatteShaderCache_updatePipelineLoadingProgress()
{
	uint32 pipelinesMissingShaders = 0;
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
	    return VulkanPipelineStableCache::GetInstance().UpdateLoading(g_shaderCacheLoaderState.loadedPipelines, pipelinesMissingShaders);
#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		return MetalPipelineCache::GetInstance().UpdateLoading(g_shaderCacheLoaderState.loadedPipelines, pipelinesMissingShaders);
#endif
	}

	return false;
}

uint64 LatteShaderCache_getShaderNameInTransferableCache(uint64 baseHash, uint32 shaderType)
{
	baseHash &= ~(7ULL << 61ULL);
	baseHash |= ((uint64)shaderType << 61ULL);
	return baseHash;
}

void LatteShaderCache_writeSeparableVertexShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* fetchShader, uint32 fetchShaderSize, uint8* vertexShader, uint32 vertexShaderSize, uint32* contextRegisters, bool usesGeometryShader)
{
	if (!s_shaderCacheGeneric)
		return;
	MemStreamWriter streamWriter(128 * 1024);
	// header
	streamWriter.writeBE<uint8>(1 | (SHADER_CACHE_TYPE_VERTEX << 4)); // version and type (shared field)
	streamWriter.writeBE<uint64>(shaderBaseHash);
	streamWriter.writeBE<uint64>(shaderAuxHash);
	streamWriter.writeBE<uint8>(usesGeometryShader ? 1 : 0);
	// register state
	Latte::GPUCompactedRegisterState compactRegState;
	Latte::StoreGPURegisterState(*(LatteContextRegister*)contextRegisters, compactRegState);
	Latte::SerializeRegisterState(compactRegState, streamWriter);
	// fetch shader
	Latte::SerializeShaderProgram(fetchShader, fetchShaderSize, streamWriter);
	// vertex shader
	Latte::SerializeShaderProgram(vertexShader, vertexShaderSize, streamWriter);
	// write to cache
	uint64 shaderCacheName = LatteShaderCache_getShaderNameInTransferableCache(shaderBaseHash, SHADER_CACHE_TYPE_VERTEX);
	std::span<uint8> dataBlob = streamWriter.getResult();
	s_shaderCacheGeneric->AddFileAsync({shaderCacheName, shaderAuxHash }, dataBlob.data(), dataBlob.size());
}

void LatteShaderCache_writeSeparableGeometryShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* geometryShader, uint32 geometryShaderSize, uint8* gsCopyShader, uint32 gsCopyShaderSize, uint32* contextRegisters, uint32* hleSpecialState, uint32 vsRingParameterCount)
{
	if (!s_shaderCacheGeneric)
		return;
	MemStreamWriter streamWriter(128 * 1024);
	// header
	streamWriter.writeBE<uint8>(1 | (SHADER_CACHE_TYPE_GEOMETRY << 4)); // version and type (shared field)
	streamWriter.writeBE<uint64>(shaderBaseHash);
	streamWriter.writeBE<uint64>(shaderAuxHash);
	cemu_assert_debug(vsRingParameterCount < 0x10000);
	streamWriter.writeBE<uint16>(vsRingParameterCount);
	// register state
	Latte::GPUCompactedRegisterState compactRegState;
	Latte::StoreGPURegisterState(*(LatteContextRegister*)contextRegisters, compactRegState);
	Latte::SerializeRegisterState(compactRegState, streamWriter);
	// geometry copy shader
	Latte::SerializeShaderProgram(gsCopyShader, gsCopyShaderSize, streamWriter);
	// geometry shader
	Latte::SerializeShaderProgram(geometryShader, geometryShaderSize, streamWriter);
	// write to cache
	uint64 shaderCacheName = LatteShaderCache_getShaderNameInTransferableCache(shaderBaseHash, SHADER_CACHE_TYPE_GEOMETRY);
	std::span<uint8> dataBlob = streamWriter.getResult();
	s_shaderCacheGeneric->AddFileAsync({shaderCacheName, shaderAuxHash }, dataBlob.data(), dataBlob.size());
}

void LatteShaderCache_writeSeparablePixelShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* pixelShader, uint32 pixelShaderSize, uint32* contextRegisters, bool usesGeometryShader)
{
	if (!s_shaderCacheGeneric)
		return;
	MemStreamWriter streamWriter(128 * 1024);
	streamWriter.writeBE<uint8>(1 | (SHADER_CACHE_TYPE_PIXEL << 4)); // version and type (shared field)
	streamWriter.writeBE<uint64>(shaderBaseHash);
	streamWriter.writeBE<uint64>(shaderAuxHash);
	streamWriter.writeBE<uint8>(usesGeometryShader ? 1 : 0);
	// register state
	Latte::GPUCompactedRegisterState compactRegState;
	Latte::StoreGPURegisterState(*(LatteContextRegister*)contextRegisters, compactRegState);
	Latte::SerializeRegisterState(compactRegState, streamWriter);
	// pixel shader
	Latte::SerializeShaderProgram(pixelShader, pixelShaderSize, streamWriter);
	// write to cache
	uint64 shaderCacheName = LatteShaderCache_getShaderNameInTransferableCache(shaderBaseHash, SHADER_CACHE_TYPE_PIXEL);
	std::span<uint8> dataBlob = streamWriter.getResult();
	s_shaderCacheGeneric->AddFileAsync({shaderCacheName, shaderAuxHash }, dataBlob.data(), dataBlob.size());
}

void LatteShaderCache_loadOrCompileSeparableShader(LatteDecompilerShader* shader, uint64 shaderBaseHash, uint64 shaderAuxHash)
{
	RendererShader::ShaderType shaderType;
	if (shader->shaderType == LatteConst::ShaderType::Vertex)
	{
		shaderType = RendererShader::ShaderType::kVertex;
		shaderCacheScreenStats.vertexShaderCount++;
	}
	else if (shader->shaderType == LatteConst::ShaderType::Geometry)
	{
		shaderType = RendererShader::ShaderType::kGeometry;
		shaderCacheScreenStats.geometryShaderCount++;
	}
	else if (shader->shaderType == LatteConst::ShaderType::Pixel)
	{
		shaderType = RendererShader::ShaderType::kFragment;
		shaderCacheScreenStats.pixelShaderCount++;
	}
	// compile shader
	shaderCacheScreenStats.compiledShaderCount++;
	LatteShader_CreateRendererShader(shader, true);
	if (shader->shader == nullptr)
		return;
	LatteShaderCache_addToCompileQueue(shader);
}

bool LatteShaderCache_readSeparableVertexShader(MemStreamReader& streamReader, uint8 version)
{
	if (version != 1)
		return false;
	uint64 shaderBaseHash = streamReader.readBE<uint64>();
	uint64 shaderAuxHash = streamReader.readBE<uint64>();
	bool usesGeometryShader = streamReader.readBE<uint8>() != 0;
	// context registers
	Latte::GPUCompactedRegisterState regState;
	if (!Latte::DeserializeRegisterState(regState, streamReader))
		return false;
	if (streamReader.hasError())
		return false;
	LatteContextRegister* lcr = &Latte::LoadGPURegisterStateScratch(regState);
	// fetch shader
	std::vector<uint8> fetchShaderData;
	if (!Latte::DeserializeShaderProgram(fetchShaderData, streamReader))
		return false;
	if (streamReader.hasError())
		return false;
	// vertex shader
	std::vector<uint8> vertexShaderData;
	if (!Latte::DeserializeShaderProgram(vertexShaderData, streamReader))
		return false;
	if (streamReader.hasError() || !streamReader.isEndOfStream())
		return false;
	// update PS inputs (affects VS shader outputs)
	LatteShader_UpdatePSInputs(lcr->GetRawView());
	// get fetch shader
	LatteFetchShader::CacheHash fsHash = LatteFetchShader::CalculateCacheHash((uint32*)fetchShaderData.data(), fetchShaderData.size());
	LatteFetchShader* fetchShader = LatteShaderRecompiler_createFetchShader(fsHash, lcr->GetRawView(), (uint32*)fetchShaderData.data(), fetchShaderData.size());
	// determine decompiler options
	LatteDecompilerOptions options;
	LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Vertex, usesGeometryShader);
	// decompile vertex shader
	LatteDecompilerOutput_t decompilerOutput{};
	LatteDecompiler_DecompileVertexShader(shaderBaseHash, lcr->GetRawView(), vertexShaderData.data(), vertexShaderData.size(), fetchShader, options, &decompilerOutput);
	LatteDecompilerShader* vertexShader = LatteShader_CreateShaderFromDecompilerOutput(decompilerOutput, shaderBaseHash, false, shaderAuxHash, lcr->GetRawView());
	// compile
	LatteShader_DumpShader(shaderBaseHash, shaderAuxHash, vertexShader);
	LatteShader_DumpRawShader(shaderBaseHash, shaderAuxHash, SHADER_DUMP_TYPE_VERTEX, vertexShaderData.data(), vertexShaderData.size());
	LatteShaderCache_loadOrCompileSeparableShader(vertexShader, shaderBaseHash, shaderAuxHash);
	LatteSHRC_RegisterShader(vertexShader, shaderBaseHash, shaderAuxHash);
	return true;
}

bool LatteShaderCache_readSeparableGeometryShader(MemStreamReader& streamReader, uint8 version)
{
	if (version != 1)
		return false;
	uint64 shaderBaseHash = streamReader.readBE<uint64>();
	uint64 shaderAuxHash = streamReader.readBE<uint64>();
	uint32 vsRingParameterCount = streamReader.readBE<uint16>();
	// context registers
	Latte::GPUCompactedRegisterState regState;
	if (!Latte::DeserializeRegisterState(regState, streamReader))
		return false;
	if (streamReader.hasError())
		return false;
	LatteContextRegister* lcr = &Latte::LoadGPURegisterStateScratch(regState);
	// geometry copy shader
	std::vector<uint8> geometryCopyShaderData;
	if (!Latte::DeserializeShaderProgram(geometryCopyShaderData, streamReader))
		return false;
	// geometry shader
	std::vector<uint8> geometryShaderData;
	if (!Latte::DeserializeShaderProgram(geometryShaderData, streamReader))
		return false;
	if (streamReader.hasError() || !streamReader.isEndOfStream())
		return false;
	// update PS inputs
	LatteShader_UpdatePSInputs(lcr->GetRawView());
	// determine decompiler options
	LatteDecompilerOptions options;
	LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Geometry, true);
	// decompile geometry shader
	LatteDecompilerOutput_t decompilerOutput{};
	LatteDecompiler_DecompileGeometryShader(shaderBaseHash, lcr->GetRawView(), geometryShaderData.data(), geometryShaderData.size(), geometryCopyShaderData.data(), geometryCopyShaderData.size(), vsRingParameterCount, options, &decompilerOutput);
	LatteDecompilerShader* geometryShader = LatteShader_CreateShaderFromDecompilerOutput(decompilerOutput, shaderBaseHash, false, shaderAuxHash, lcr->GetRawView());
	// compile
	LatteShader_DumpShader(shaderBaseHash, shaderAuxHash, geometryShader);
	LatteShader_DumpRawShader(shaderBaseHash, shaderAuxHash, SHADER_DUMP_TYPE_GEOMETRY, geometryShaderData.data(), geometryShaderData.size());
	LatteShaderCache_loadOrCompileSeparableShader(geometryShader, shaderBaseHash, shaderAuxHash);
	LatteSHRC_RegisterShader(geometryShader, shaderBaseHash, shaderAuxHash);
	return true;
}

bool LatteShaderCache_readSeparablePixelShader(MemStreamReader& streamReader, uint8 version)
{
	if (version != 1)
		return false;
	uint64 shaderBaseHash = streamReader.readBE<uint64>();
	uint64 shaderAuxHash = streamReader.readBE<uint64>();
	bool usesGeometryShader = streamReader.readBE<uint8>() != 0;
	// context registers
	Latte::GPUCompactedRegisterState regState;
	if (!Latte::DeserializeRegisterState(regState, streamReader))
		return false;
	if (streamReader.hasError())
		return false;
	LatteContextRegister* lcr = &Latte::LoadGPURegisterStateScratch(regState);
	// pixel shader
	std::vector<uint8> pixelShaderData;
	if (!Latte::DeserializeShaderProgram(pixelShaderData, streamReader))
		return false;
	if (streamReader.hasError() || !streamReader.isEndOfStream())
		return false;
	// update PS inputs
	LatteShader_UpdatePSInputs(lcr->GetRawView());
	// determine decompiler options
	LatteDecompilerOptions options;
	LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Pixel, usesGeometryShader);
	// decompile pixel shader
	LatteDecompilerOutput_t decompilerOutput{};
	LatteDecompiler_DecompilePixelShader(shaderBaseHash, lcr->GetRawView(), pixelShaderData.data(), pixelShaderData.size(), options, &decompilerOutput);
	LatteDecompilerShader* pixelShader = LatteShader_CreateShaderFromDecompilerOutput(decompilerOutput, shaderBaseHash, false, shaderAuxHash, lcr->GetRawView());
	// compile
	LatteShader_DumpShader(shaderBaseHash, shaderAuxHash, pixelShader);
	LatteShader_DumpRawShader(shaderBaseHash, shaderAuxHash, SHADER_DUMP_TYPE_PIXEL, pixelShaderData.data(), pixelShaderData.size());
	LatteShaderCache_loadOrCompileSeparableShader(pixelShader, shaderBaseHash, shaderAuxHash);
	LatteSHRC_RegisterShader(pixelShader, shaderBaseHash, shaderAuxHash);
	return true;
}

// read shader info from shader cache
bool LatteShaderCache_readSeparableShader(uint8* shaderInfoData, sint32 shaderInfoSize)
{
	if (shaderInfoSize < 8)
		return false;
	MemStreamReader streamReader(shaderInfoData, shaderInfoSize);
	uint8 versionAndType = streamReader.readBE<uint8>();
	uint8 version = versionAndType & 0xF;
	uint8 type = (versionAndType >> 4) & 0xF;
	if (type == SHADER_CACHE_TYPE_VERTEX)
		return LatteShaderCache_readSeparableVertexShader(streamReader, version);
	else if (type == SHADER_CACHE_TYPE_GEOMETRY)
		return LatteShaderCache_readSeparableGeometryShader(streamReader, version);
	else if (type == SHADER_CACHE_TYPE_PIXEL)
		return LatteShaderCache_readSeparablePixelShader(streamReader, version);
	return false;
}

void LatteShaderCache_Close()
{
    if(s_shaderCacheGeneric)
    {
        delete s_shaderCacheGeneric;
        s_shaderCacheGeneric = nullptr;
    }
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
		RendererShaderVk::ShaderCacheLoading_Close();
		break;
#endif
#ifdef ENABLE_OPENGL
	case RendererAPI::OpenGL:
		RendererShaderGL::ShaderCacheLoading_Close();
		break;
#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		RendererShaderMtl::ShaderCacheLoading_Close();
		break;
#endif
	}

    // if Vulkan or Metal then also close pipeline cache
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
	    VulkanPipelineStableCache::GetInstance().Close();
		break;
#endif
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
	    MetalPipelineCache::GetInstance().Close();
		break;
#endif
	}
}
