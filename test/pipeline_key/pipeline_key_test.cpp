// The Vulkan pipelines' key (src/Cafe/HW/Latte/Renderer/Vulkan/
// VulkanPipelineKey.cpp), against a replayed sequence of register states:
//  - state the pipelines take as dynamic state - the stencil reference,
//    compare mask and write mask, front and back - makes no new key, so a
//    game stepping its stencil reference compiles one pipeline, not one a
//    value;
//  - every state a pipeline bakes in still makes a key of its own, so
//    nothing is drawn with a pipeline made for other state;
//  - with VK_EXT_extended_dynamic_state the cull mode, front face and depth
//    and stencil tests make no new key either, with _3 the blend enables,
//    equations and write masks, with _2 the polygon offset enable and
//    rasterizer discard, and what the draw sets for them is what a
//    pipeline baked in before;
//  - the part of the key draws that keep their shaders compare: with
//    VK_EXT_vertex_input_dynamic_state the vertex buffer strides and
//    attribute offsets make no new key, with _2 rasterizer discard none
//    either, with dynamic topology a primitive type of the same class none,
//    and the vertex input, topology and restart the draw sets are what a
//    pipeline baked in;
//  - graphics pipeline libraries (VulkanPipelineLibrary.h): each dynamic
//    state the renderer sets goes to the part that takes it, a part's key
//    leaves out exactly what its dynamic states make the draw's, a shader
//    part's robust buffer access is its own shaders' alone, and the
//    lock-free cache of parts gives each key to one maker however many
//    threads race for it;
//  - a part another thread claimed is waited for (WaitFor), never made a
//    second time, and a failed one is found missing; a pipeline a draw waits
//    for is linked fast with the optimized link queued, one nothing waits
//    for optimized at once; a try on a device without
//    VK_EXT_pipeline_creation_cache_control takes only what is made rather
//    than compiling, unless the pipeline is known to be in the driver's
//    cache (VulkanPipeline_Try);
//  - parts made ahead (docs/SHADER-STUTTER.md M2): a shader part's template
//    taken from a pipeline makes, with another shader, the very key that
//    shader's pipeline with the same state looks its part up by, for random
//    state; state a template cannot copy is refused; the table of templates
//    keeps one per state, the most recently used, never replaces one a job
//    holds, and lets every render pass it kept go;
//  - a new pipeline's descriptor set bindings and dynamic states, built in
//    place, are what the vectors built before (the reference below) for
//    every resource mapping and every set of dynamic state, and building
//    them allocates nothing.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <memory>
#include <set>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

using namespace Latte;

// every operator new counted, for the allocations the pipeline state takes
static std::atomic<uint64> s_allocs{0};
void* operator new(size_t n)
{
	s_allocs.fetch_add(1, std::memory_order_relaxed);
	if (void* p = std::malloc(n ? n : 1))
		return p;
	throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

// What PipelineCompiler built, in vectors, before
// (PipelineCompiler::CreateDescriptorSetLayout and InitDynamicState)
static std::vector<VkDescriptorSetLayoutBinding> RefBindings(LatteDecompilerShaderResourceMapping& m,
	VkShaderStageFlags stageFlags, std::vector<uint8>& uniformBuffers, bool& hasUniformVar, bool& hasUniformBuffers)
{
	std::vector<VkDescriptorSetLayoutBinding> out;
	sint32 textureBindingBase = m.getTextureBaseBindingPoint();
	if (textureBindingBase >= 0)
	{
		sint32 textureCount = m.getTextureCount();
		for (sint32 i = 0; i < textureCount; i++)
		{
			VkDescriptorSetLayoutBinding entry{};
			entry.binding = (uint32)textureBindingBase + i;
			entry.descriptorCount = 1;
			entry.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			entry.stageFlags = stageFlags;
			out.emplace_back(entry);
		}
	}
	if (m.uniformVarsBufferBindingPoint >= 0)
	{
		VkDescriptorSetLayoutBinding entry{};
		entry.binding = m.uniformVarsBufferBindingPoint;
		entry.descriptorCount = 1;
		entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		entry.stageFlags = stageFlags;
		out.emplace_back(entry);
	}
	for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (m.uniformBuffersBindingPoint[i] >= 0)
		{
			VkDescriptorSetLayoutBinding entry{};
			entry.binding = m.uniformBuffersBindingPoint[i];
			entry.descriptorCount = 1;
			entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			entry.stageFlags = stageFlags;
			out.emplace_back(entry);
			uniformBuffers.emplace_back((uint8)i);
		}
	}
	if (m.tfStorageBindingPoint >= 0)
	{
		VkDescriptorSetLayoutBinding entry{};
		entry.binding = m.tfStorageBindingPoint;
		entry.descriptorCount = 1;
		entry.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		entry.stageFlags = stageFlags;
		out.emplace_back(entry);
	}
	hasUniformVar = m.uniformVarsBufferBindingPoint >= 0;
	hasUniformBuffers = m.hasUniformBuffers();
	return out;
}

static std::vector<VkDynamicState> RefDynamicStates(uint32 dynamic, bool usesBlendConstants, bool usesDepthBias, bool feedbackLoop)
{
	std::vector<VkDynamicState> d = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	if (usesBlendConstants)
		d.emplace_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	if (usesDepthBias)
		d.emplace_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
	if (feedbackLoop)
		d.emplace_back(VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT);
	d.emplace_back(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
	d.emplace_back(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
	d.emplace_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
	if (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL)
	{
		d.emplace_back(VK_DYNAMIC_STATE_CULL_MODE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_FRONT_FACE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_STENCIL_OP_EXT);
	}
	if (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_BLEND)
	{
		d.emplace_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
	}
	if (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_VERTEX_INPUT)
		d.emplace_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
	if (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_TOPOLOGY)
	{
		d.emplace_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT);
	}
	if (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER)
	{
		d.emplace_back(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT);
		d.emplace_back(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT);
	}
	return d;
}

static bool SameBinding(const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b)
{
	return a.binding == b.binding && a.descriptorType == b.descriptorType && a.descriptorCount == b.descriptorCount &&
		a.stageFlags == b.stageFlags && a.pImmutableSamplers == b.pImmutableSamplers;
}

// a pipeline's create info with random state of every kind a part keys on
struct RandomPipeline
{
	VkGraphicsPipelineCreateInfo full{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	VkPipelineShaderStageCreateInfo stages[2]{};
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	VkViewport viewport{};
	VkRect2D scissor{};
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	VkSampleMask mask{};
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	VkDynamicState states[VULKAN_PIPELINE_MAX_DYNAMIC_STATES];

	RandomPipeline(std::mt19937& rng, VkShaderModule vs, VkShaderModule fs)
	{
		auto bit = [&] { return (VkBool32)(rng() & 1); };
		stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
		stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
		full.stageCount = 2;
		full.pStages = stages;
		viewport = {0, 0, (float)(rng() % 1280), (float)(rng() % 720), 0, 1};
		scissor = {{0, 0}, {(uint32)(rng() % 1280), (uint32)(rng() % 720)}};
		vp.viewportCount = vp.scissorCount = 1;
		if (bit())
		{
			vp.pViewports = &viewport;
			vp.pScissors = &scissor;
		}
		rs.depthClampEnable = bit();
		rs.rasterizerDiscardEnable = bit();
		rs.polygonMode = bit() ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_FILL_RECTANGLE_NV;
		rs.cullMode = rng() % 4;
		rs.frontFace = (VkFrontFace)(rng() % 2);
		rs.depthBiasEnable = bit();
		rs.depthBiasConstantFactor = (float)(rng() % 8);
		rs.lineWidth = 1.0f + (rng() % 2);
		clip.depthClipEnable = bit();
		if (bit())
			rs.pNext = &clip;
		ms.rasterizationSamples = bit() ? VK_SAMPLE_COUNT_1_BIT : VK_SAMPLE_COUNT_4_BIT;
		ms.alphaToCoverageEnable = bit();
		mask = rng();
		if (bit())
			ms.pSampleMask = &mask;
		ds.depthTestEnable = bit();
		ds.depthWriteEnable = bit();
		ds.depthCompareOp = (VkCompareOp)(rng() % 8);
		ds.stencilTestEnable = bit();
		ds.front = {(VkStencilOp)(rng() % 8), (VkStencilOp)(rng() % 8), (VkStencilOp)(rng() % 8), (VkCompareOp)(rng() % 8), (uint32)(rng() & 0xFF), (uint32)(rng() & 0xFF), (uint32)(rng() & 0xFF)};
		ds.back = {(VkStencilOp)(rng() % 8), (VkStencilOp)(rng() % 8), (VkStencilOp)(rng() % 8), (VkCompareOp)(rng() % 8), (uint32)(rng() & 0xFF), (uint32)(rng() & 0xFF), (uint32)(rng() & 0xFF)};
		dyn.dynamicStateCount = VulkanPipelineKey_DynamicStates(rng() % 32, rng() % 8, states);
		dyn.pDynamicStates = states;
		// the viewport and scissor baked in too (the renderer's are dynamic)
		if (bit())
		{
			dyn.pDynamicStates = states + 2;
			dyn.dynamicStateCount -= 2;
		}
		full.pViewportState = &vp;
		full.pRasterizationState = &rs;
		full.pMultisampleState = &ms;
		full.pDepthStencilState = &ds;
		full.pColorBlendState = &cb;
		full.pDynamicState = &dyn;
		full.renderPass = (VkRenderPass)(uintptr_t)(0x100 + rng() % 4);
	}
};

static void TestWaitFor()
{
	// the link: fast for a waiting draw, the optimized one after
	bool queue = true;
	CHECK(!VulkanPipelineLibrary_LinkOptimized(true, &queue) && queue);
	CHECK(VulkanPipelineLibrary_LinkOptimized(false, &queue) && !queue);
	// a compile forced compiles; a try asks the driver's cache only where it
	// can without compiling, and otherwise takes only what is made - never a
	// compile on the draw path for a device without cache control
	CHECK(VulkanPipeline_Try(true, true, false) == VulkanPipelineTry::Compile);
	CHECK(VulkanPipeline_Try(true, false, false) == VulkanPipelineTry::Compile);
	CHECK(VulkanPipeline_Try(false, true, false) == VulkanPipelineTry::FromCache);
	CHECK(VulkanPipeline_Try(false, true, true) == VulkanPipelineTry::FromCache); // asked the cache all the same
	CHECK(VulkanPipeline_Try(false, false, false) == VulkanPipelineTry::Made);
	// known to be in the driver's cache (loaded at boot): made, a hit there,
	// rather than its draw skipped until a compile thread makes the same hit
	CHECK(VulkanPipeline_Try(false, false, true) == VulkanPipelineTry::Compile);

	// one thread claims keys and makes their parts while eight others,
	// finding them pending, wait for them
	VulkanPipelineLibraryCache cache;
	const VulkanPipelineLibraryPart part = VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER;
	const uint64 keys = 200;
	for (uint64 k = 1; k <= keys; k++)
		CHECK(cache.Claim(part, k));
	std::atomic<int> entered{0};
	std::atomic<uint32> wrong{0}, madeTwice{0};
	std::vector<std::thread> waiters;
	for (int t = 0; t < 8; t++)
		waiters.emplace_back([&] {
			entered++;
			for (uint64 k = 1; k <= keys; k++)
			{
				VkPipeline library = VK_NULL_HANDLE;
				// the renderer's: pending is waited for, only missing claimed
				// (here, only ever after the maker's failure)
				const VulkanPipelineLibraryCache::State found = cache.Find(part, k, library);
				if (found == VulkanPipelineLibraryCache::State::Missing && k % 7)
					madeTwice++;
				if (found == VulkanPipelineLibraryCache::State::Missing)
					continue;
				const VulkanPipelineLibraryCache::State state = cache.WaitFor(part, k, library);
				// every seventh fails to be made
				const bool ok = k % 7 ? state == VulkanPipelineLibraryCache::State::Ready && library == (VkPipeline)(uintptr_t)(k * 16)
					: state == VulkanPipelineLibraryCache::State::Missing;
				if (!ok)
					wrong++;
			}
		});
	while (entered.load() < 8)
		std::this_thread::yield();
	for (uint64 k = 1; k <= keys; k++)
		cache.Publish(part, k, k % 7 ? (VkPipeline)(uintptr_t)(k * 16) : VK_NULL_HANDLE);
	for (auto& t : waiters)
		t.join();
	std::printf("parts waited for: %llu keys by 8 threads, %u found wrong, %u made a second time\n", (unsigned long long)keys,
		wrong.load(), madeTwice.load());
	CHECK(wrong == 0 && madeTwice == 0);
	// a key nobody claimed: no wait
	VkPipeline none;
	CHECK(cache.WaitFor(part, keys + 1, none) == VulkanPipelineLibraryCache::State::Missing);
	cache.DestroyAll(VK_NULL_HANDLE);
}

static void TestPartTemplates()
{
	std::mt19937 rng(77);
	const VulkanPipelineLibraryPart parts[2] = {VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER};
	uint32 compared = 0, mismatched = 0, emptyMismatched = 0;
	for (uint32 it = 0; it < 4000; it++)
	{
		// the state as the game left it, with the shaders it had; and the
		// same state with a new shader, whose part is made ahead
		const VkShaderModule vs = (VkShaderModule)(uintptr_t)0x10, fs = (VkShaderModule)(uintptr_t)0x20;
		const VkShaderModule newVs = (VkShaderModule)(uintptr_t)0x30, newFs = (VkShaderModule)(uintptr_t)0x40;
		std::mt19937 replay = rng;
		RandomPipeline seen(rng, vs, fs);
		RandomPipeline later(replay, newVs, newFs);
		const uint64 renderPassHash = 0x5000 + (uint64)(uintptr_t)seen.full.renderPass;
		const bool robust = it & 1;
		for (VulkanPipelineLibraryPart part : parts)
		{
			VulkanPipelinePartTemplate t;
			CHECK(VulkanPipelineLibrary_Template(part, seen.full, renderPassHash, t));
			const bool pre = part == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION;
			VulkanPipelineLibraryIdentity identity{pre ? 0x777ull + it : 0, 0, pre ? 0 : 0x999ull + it, renderPassHash, pre && robust, !pre && robust};
			VulkanPipelineLibraryIdentity none{0, 0, 0, renderPassHash, false, false};
			if (t.key != VulkanPipelineLibrary_Key(part, seen.full, none))
				emptyMismatched++;
			VulkanPipelinePartAhead ahead;
			VulkanPipelinePartTemplate copy = t; // as the table keeps it
			VulkanPipelineLibrary_FromTemplate(copy, &later.stages[pre ? 0 : 1], nullptr, ahead);
			if (VulkanPipelineLibrary_Key(part, ahead.full, identity) != VulkanPipelineLibrary_Key(part, later.full, identity))
				mismatched++;
			CHECK(ahead.full.stageCount == 1 && ahead.full.pStages[0].module == (pre ? newVs : newFs));
			compared++;
		}
	}
	std::printf("part templates: %u parts made ahead from random state, %u keyed apart from their pipeline's, %u templates mis-keyed\n",
		compared, mismatched, emptyMismatched);
	CHECK(mismatched == 0 && emptyMismatched == 0);

	// what a template cannot copy, refused
	{
		RandomPipeline p(rng, VK_NULL_HANDLE, VK_NULL_HANDLE);
		VulkanPipelinePartTemplate t;
		CHECK(!VulkanPipelineLibrary_Template(VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT, p.full, 1, t));
		CHECK(!VulkanPipelineLibrary_Template(VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT, p.full, 1, t));
		p.vp.viewportCount = 2;
		CHECK(!VulkanPipelineLibrary_Template(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, p.full, 1, t));
		p.vp.viewportCount = 1;
		VkPipelineRasterizationLineStateCreateInfoEXT line{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT};
		p.rs.pNext = &line;
		CHECK(!VulkanPipelineLibrary_Template(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, p.full, 1, t));
		CHECK(VulkanPipelineLibrary_Template(VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER, p.full, 1, t));
	}

	// the table: render passes kept as long as a template has them
	static int s_retained;
	s_retained = 0;
	{
		VulkanPipelinePartTemplates table([](void* owner, bool retain) { s_retained += retain ? 1 : -1; });
		auto make = [&](VulkanPipelineLibraryPart part, uint64 key) {
			VulkanPipelinePartTemplate t{};
			t.part = part;
			t.key = key;
			return t;
		};
		int owners[200];
		// one per state, seen again or not
		CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 1), &owners[0]));
		CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 1), &owners[0]));
		CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER, 1), &owners[1]));
		CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 2), &owners[2]));
		CHECK(table.Count() == 3 && s_retained == 3);
		// a part's templates but the pipeline's own, each held
		const uint64 pre = table.Take(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 2);
		CHECK(std::popcount(pre) == 1 && table.Get((uint32)std::countr_zero(pre)).key == 1);
		CHECK(table.Held((uint32)std::countr_zero(pre)) == 1);
		CHECK(std::popcount(table.Take(VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER, 0)) == 1);
		// filled past its slots: the least recently used go, the held stay
		for (int i = 0; i < 200; i++)
			CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 100 + i), &owners[i % 200]));
		CHECK(table.Count() == VulkanPipelinePartTemplates::kSlots);
		CHECK(table.Get((uint32)std::countr_zero(pre)).key == 1);
		bool newestKept = false;
		for (uint32 i = 0; i < VulkanPipelinePartTemplates::kSlots; i++)
			newestKept |= table.Get(i).key == 299;
		CHECK(newestKept);
		CHECK(s_retained == (int)VulkanPipelinePartTemplates::kSlots);
		// every slot held: nothing recorded, nothing replaced
		const uint64 all = table.Take(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 0);
		uint64 frag = 0;
		for (uint32 i = 0; i < VulkanPipelinePartTemplates::kSlots; i++)
			if (table.Get(i).part == VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER)
				frag |= 1ull << i;
		table.Take(VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER, 0);
		CHECK(!table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 5000), &owners[0]));
		table.Drop(all);
		table.Drop(pre);
		table.Drop(frag);
		table.Drop(frag);
		CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 5000), &owners[0]));
		CHECK(s_retained == (int)VulkanPipelinePartTemplates::kSlots);
		table.Clear();
		CHECK(table.Count() == 0 && s_retained == 0);
		CHECK(table.Record(make(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 7), &owners[0]));
	}
	// and when it goes
	CHECK(s_retained == 0);

	// compile threads reading and dropping the templates they were given
	// while the GPU thread records over the rest (under TSan in CI): a held
	// template is never written
	{
		VulkanPipelinePartTemplates table([](void*, bool) {});
		std::atomic<uint64> handoff[4];
		for (auto& h : handoff)
			h.store(0);
		std::atomic<bool> stop{false};
		std::atomic<uint32> torn{0};
		std::vector<std::thread> workers;
		for (int w = 0; w < 4; w++)
			workers.emplace_back([&, w] {
				for (;;)
				{
					uint64 slots = handoff[w].exchange(0, std::memory_order_acquire);
					if (!slots)
					{
						if (stop.load())
							return;
						std::this_thread::yield();
						continue;
					}
					for (uint64 m = slots; m; m &= m - 1)
					{
						const VulkanPipelinePartTemplate& t = table.Get((uint32)std::countr_zero(m));
						if (t.renderPassHash != t.key * 3)
							torn++;
					}
					table.Drop(slots);
				}
			});
		uint32 handed = 0;
		for (uint64 i = 1; i <= 20000; i++)
		{
			VulkanPipelinePartTemplate t{};
			t.part = VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION;
			t.key = i;
			t.renderPassHash = i * 3;
			table.Record(t, nullptr);
			if (i % 16 == 0)
			{
				const uint64 slots = table.Take(VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, 0);
				uint64 expected = 0;
				if (handoff[i / 16 % 4].compare_exchange_strong(expected, slots, std::memory_order_release))
					handed++;
				else
					table.Drop(slots);
			}
		}
		stop = true;
		for (auto& t : workers)
			t.join();
		for (auto& h : handoff)
			table.Drop(h.exchange(0));
		uint32 held = 0;
		for (uint32 i = 0; i < VulkanPipelinePartTemplates::kSlots; i++)
			held += table.Held(i);
		std::printf("part templates: %u sets handed to 4 threads while recorded over, %u read torn, %u left held\n", handed, torn.load(), held);
		CHECK(handed > 0 && torn == 0 && held == 0);
	}
	std::printf("part templates table: %u slots, render passes all let go\n", VulkanPipelinePartTemplates::kSlots);
}

static void TestPipelineState()
{
	// every set of dynamic state and of what the pipeline reads
	uint32 dynamicStateSets = 0;
	uint64 allocs = 0;
	for (uint32 dynamic = 0; dynamic < 32; dynamic++)
	{
		for (uint32 uses = 0; uses < 8; uses++)
		{
			const auto ref = RefDynamicStates(dynamic, uses & VULKAN_PIPELINE_USES_BLEND_CONSTANTS,
				uses & VULKAN_PIPELINE_USES_DEPTH_BIAS, uses & VULKAN_PIPELINE_FEEDBACK_LOOP_DYNAMIC);
			VkDynamicState got[VULKAN_PIPELINE_MAX_DYNAMIC_STATES];
			const uint64 before = s_allocs.load();
			const uint32 n = VulkanPipelineKey_DynamicStates(dynamic, uses, got);
			allocs += s_allocs.load() - before;
			CHECK(n == ref.size() && n <= VULKAN_PIPELINE_MAX_DYNAMIC_STATES);
			CHECK(n == ref.size() && std::equal(ref.begin(), ref.end(), got));
			dynamicStateSets++;
		}
	}
	std::printf("dynamic states: %u sets as before, %llu allocations\n", dynamicStateSets, (unsigned long long)allocs);
	CHECK(allocs == 0);

	// resource mappings as the decompiler leaves them, and at their limits
	std::mt19937 rng(1234);
	auto pick = [&](sint32 lo, sint32 hi) { return (sint32)(rng() % (uint32)(hi - lo + 1)) + lo; };
	const VkShaderStageFlags stages[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
	uint32 mappings = 0, mismatched = 0;
	allocs = 0;
	for (uint32 it = 0; it < 20000; it++)
	{
		LatteDecompilerShaderResourceMapping m;
		const bool full = it < 2;
		m.textureUnitBaseBindingPoint = full ? 0 : (sint8)(pick(0, 3) ? pick(0, 8) : -1);
		m.textureUnitCount = full ? LATTE_NUM_MAX_TEX_UNITS : (sint8)pick(0, LATTE_NUM_MAX_TEX_UNITS);
		sint32 next = m.textureUnitBaseBindingPoint + m.textureUnitCount + 1;
		m.uniformVarsBufferBindingPoint = (full && it == 0) || pick(0, 1) ? (sint8)next++ : -1;
		for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
			m.uniformBuffersBindingPoint[i] = full || pick(0, 3) == 0 ? (sint8)next++ : -1;
		m.tfStorageBindingPoint = full || pick(0, 7) == 0 ? (sint8)next++ : -1;
		const VkShaderStageFlags stage = stages[it % 3];

		std::vector<uint8> refUB;
		bool refVar = false, refUBs = false;
		const auto ref = RefBindings(m, stage, refUB, refVar, refUBs);
		VulkanDescriptorBindings b;
		std::memset(&b, 0xA5, sizeof(b));
		const uint64 before = s_allocs.load();
		VulkanPipelineKey_DescriptorBindings(m, stage, b);
		allocs += s_allocs.load() - before;
		bool same = b.count == ref.size() && b.count <= VULKAN_PIPELINE_MAX_SET_BINDINGS &&
			b.uniformBufferCount == refUB.size() && b.hasUniformVar == refVar && b.hasUniformBuffers == refUBs;
		for (uint32 i = 0; same && i < b.count; i++)
			same = SameBinding(b.bindings[i], ref[i]);
		for (uint32 i = 0; same && i < b.uniformBufferCount; i++)
			same = b.uniformBuffers[i] == refUB[i];
		if (!same)
			mismatched++;
		mappings++;
	}
	std::printf("descriptor set bindings: %u mappings, %u not as before, %llu allocations\n", mappings, mismatched,
		(unsigned long long)allocs);
	CHECK(mismatched == 0);
	CHECK(allocs == 0);
}

static void SetReg(LatteContextRegister& r, uint32 addr, uint32 v)
{
	r.GetRawView()[addr] = v;
}

int main()
{
	auto base = std::make_unique<LatteContextRegister>();
	std::memset(base.get(), 0, sizeof(LatteContextRegister));
	// depth on, less-equal, written; stencil on, two-sided
	const uint32 depthControl = 1 | (1 << 1) | (1 << 2) | (3 << 4) | (1 << 7) | (2 << 8) | (2 << 20);
	SetReg(*base, REGADDR::DB_DEPTH_CONTROL, depthControl);
	SetReg(*base, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (1 << 8));
	SetReg(*base, REGADDR::CB_BLEND0_CONTROL, 0x00010001);
	SetReg(*base, REGADDR::CB_TARGET_MASK, 0xF);
	const uint64 key0 = VulkanPipelineKey_RenderState(*base, 0);

	// the replayed sequence: every stencil reference, with masks and the
	// back face's values stepping along
	auto r = std::make_unique<LatteContextRegister>(*base);
	std::set<uint64> keys;
	for (uint32 i = 0; i < 1024; i++)
	{
		SetReg(*r, REGADDR::DB_STENCILREFMASK, (i & 0xFF) | ((i * 7 & 0xFF) << 8) | ((i * 13 & 0xFF) << 16));
		SetReg(*r, REGADDR::DB_STENCILREFMASK_BF, ((i * 3) & 0xFF) | ((i * 5 & 0xFF) << 8) | ((i * 11 & 0xFF) << 16));
		keys.insert(VulkanPipelineKey_RenderState(*r, 0));
	}
	std::printf("pipeline keys for 1024 stencil reference and mask states: %zu\n", keys.size());
	CHECK(keys.size() == 1 && *keys.begin() == key0);

	// what the pipeline bakes in: each change a key of its own
	struct Change { const char* what; uint32 reg; uint32 value; };
	const Change baked[] = {
		{"depth compare", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 4)},
		{"depth test", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 1)},
		{"depth write", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 2)},
		{"stencil test", REGADDR::DB_DEPTH_CONTROL, depthControl ^ 1},
		{"back stencil", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 7)},
		{"stencil compare", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 8)},
		{"stencil pass op", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 14)},
		{"back stencil compare", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 20)},
		{"cull front", REGADDR::PA_SU_SC_MODE_CNTL, 1},
		{"cull back", REGADDR::PA_SU_SC_MODE_CNTL, 2},
		{"front face", REGADDR::PA_SU_SC_MODE_CNTL, 4},
		{"polygon offset", REGADDR::PA_SU_SC_MODE_CNTL, 1 << 11},
		{"rasterizer discard", REGADDR::PA_CL_CLIP_CNTL, 1 << 22},
		{"color control", REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (1 << 8) | 1},
		{"blend enable", REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (3 << 8)},
		{"blend factors", REGADDR::CB_BLEND0_CONTROL, 0x00020001},
		{"write mask", REGADDR::CB_TARGET_MASK, 0x7},
	};
	std::set<uint64> distinct{key0};
	for (const Change& c : baked)
	{
		auto s = std::make_unique<LatteContextRegister>(*base);
		SetReg(*s, c.reg, c.value);
		const uint64 k = VulkanPipelineKey_RenderState(*s, 0);
		if (k == key0)
		{
			std::printf("FAIL: %s does not change the key\n", c.what);
			s_failures++;
		}
		distinct.insert(k);
	}
	CHECK(distinct.size() == 1 + sizeof(baked) / sizeof(baked[0]));
	// rasterizer discard kept or dropped with the viewport's X offset
	{
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::PA_CL_CLIP_CNTL, 1 << 22);
		auto b = std::make_unique<LatteContextRegister>(*a);
		SetReg(*b, REGADDR::PA_CL_VTE_CNTL, 1 << 1); // VPORT_X_OFFSET_ENA
		CHECK(VulkanPipelineKey_RenderState(*a, 0) != VulkanPipelineKey_RenderState(*b, 0));
	}
	// with stencil off, its fields are no part of the key
	{
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::DB_DEPTH_CONTROL, depthControl & ~1u);
		auto b = std::make_unique<LatteContextRegister>(*a);
		SetReg(*b, REGADDR::DB_DEPTH_CONTROL, (depthControl & ~1u) ^ (1 << 8));
		CHECK(VulkanPipelineKey_RenderState(*a, 0) == VulkanPipelineKey_RenderState(*b, 0));
	}
	// extended dynamic state: cull, front face, depth and stencil are the
	// draw's, everything else still keyed
	{
		std::set<uint64> dyn;
		const uint64 dkey = VulkanPipelineKey_RenderState(*base, VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL);
		size_t keyed = 0;
		for (const Change& c : baked)
		{
			auto s2 = std::make_unique<LatteContextRegister>(*base);
			SetReg(*s2, c.reg, c.value);
			const uint64 k = VulkanPipelineKey_RenderState(*s2, VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL);
			const bool dynamic = c.reg == REGADDR::DB_DEPTH_CONTROL ||
				(c.reg == REGADDR::PA_SU_SC_MODE_CNTL && c.value < 8);
			if (dynamic && k != dkey)
			{
				std::printf("FAIL: %s makes a new key with extended dynamic state\n", c.what);
				s_failures++;
			}
			if (!dynamic)
			{
				keyed++;
				dyn.insert(k);
				if (k == dkey)
				{
					std::printf("FAIL: %s does not change the key with extended dynamic state\n", c.what);
					s_failures++;
				}
			}
		}
		CHECK(dyn.size() == keyed);
		// a replayed sequence of depth, stencil and cull states: one key
		std::set<uint64> seq;
		auto r2 = std::make_unique<LatteContextRegister>(*base);
		for (uint32 i = 0; i < 4096; i++)
		{
			SetReg(*r2, REGADDR::DB_DEPTH_CONTROL, i * 0x9E3779B1u);
			SetReg(*r2, REGADDR::PA_SU_SC_MODE_CNTL, i & 7);
			seq.insert(VulkanPipelineKey_RenderState(*r2, VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL));
		}
		std::printf("pipeline keys for 4096 depth, stencil and cull states with extended dynamic state: %zu\n", seq.size());
		CHECK(seq.size() == 1);
	}
	// extended dynamic state 3: blend enables, equations and write masks are
	// the draw's, everything else (depth too, without the first) still keyed
	{
		const uint32 flags = VULKAN_PIPELINE_KEY_DYNAMIC_BLEND;
		const uint64 bkey = VulkanPipelineKey_RenderState(*base, flags);
		std::set<uint64> others;
		size_t keyed = 0;
		for (const Change& c : baked)
		{
			auto s2 = std::make_unique<LatteContextRegister>(*base);
			SetReg(*s2, c.reg, c.value);
			const uint64 k = VulkanPipelineKey_RenderState(*s2, flags);
			const bool dynamic = c.reg == REGADDR::CB_BLEND0_CONTROL || c.reg == REGADDR::CB_TARGET_MASK ||
				(c.reg == REGADDR::CB_COLOR_CONTROL && ((c.value ^ (0xCC0000 | (1 << 8))) & ~0xFF00u) == 0);
			if (dynamic && k != bkey)
			{
				std::printf("FAIL: %s makes a new key with dynamic blending\n", c.what);
				s_failures++;
			}
			if (!dynamic)
			{
				keyed++;
				others.insert(k);
				if (k == bkey)
				{
					std::printf("FAIL: %s does not change the key with dynamic blending\n", c.what);
					s_failures++;
				}
			}
		}
		CHECK(others.size() == keyed);
		// the logic op stays in the pipeline
		auto rop = std::make_unique<LatteContextRegister>(*base);
		SetReg(*rop, REGADDR::CB_COLOR_CONTROL, 0xEE0000 | (1 << 8));
		CHECK(VulkanPipelineKey_RenderState(*rop, flags) != bkey);
		std::set<uint64> seq;
		auto r2 = std::make_unique<LatteContextRegister>(*base);
		for (uint32 i = 0; i < 4096; i++)
		{
			SetReg(*r2, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | ((i & 0xFF) << 8));
			SetReg(*r2, REGADDR::CB_TARGET_MASK, i * 0x9E3779B1u);
			for (int t = 0; t < 8; t++)
				SetReg(*r2, REGADDR::CB_BLEND0_CONTROL + t, (i * 0x85EBCA6Bu) ^ (t * 0x1010101u));
			seq.insert(VulkanPipelineKey_RenderState(*r2, flags));
		}
		std::printf("pipeline keys for 4096 blend states with dynamic blending: %zu\n", seq.size());
		CHECK(seq.size() == 1);
	}
	// extended dynamic state 2: polygon offset enable and rasterizer discard
	// are the draw's, everything else still keyed
	{
		const uint32 flags = VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER;
		const uint64 rkey = VulkanPipelineKey_RenderState(*base, flags);
		std::set<uint64> others;
		size_t keyed = 0;
		for (const Change& c : baked)
		{
			auto s2 = std::make_unique<LatteContextRegister>(*base);
			SetReg(*s2, c.reg, c.value);
			const uint64 k = VulkanPipelineKey_RenderState(*s2, flags);
			const bool dynamic = (c.reg == REGADDR::PA_SU_SC_MODE_CNTL && c.value == (1u << 11)) ||
				(c.reg == REGADDR::PA_CL_CLIP_CNTL && c.value == (1u << 22));
			if (dynamic && k != rkey)
			{
				std::printf("FAIL: %s makes a new key with dynamic depth bias and discard\n", c.what);
				s_failures++;
			}
			if (!dynamic)
			{
				keyed++;
				others.insert(k);
				if (k == rkey)
				{
					std::printf("FAIL: %s does not change the key with dynamic depth bias and discard\n", c.what);
					s_failures++;
				}
			}
		}
		CHECK(others.size() == keyed);
		// the rest of the clip control (the depth clip) stays in the pipeline
		auto clip = std::make_unique<LatteContextRegister>(*base);
		SetReg(*clip, REGADDR::PA_CL_CLIP_CNTL, (1u << 22) | (1u << 27));
		CHECK(VulkanPipelineKey_RenderState(*clip, flags) != rkey);
		std::set<uint64> seq;
		auto r2 = std::make_unique<LatteContextRegister>(*base);
		for (uint32 i = 0; i < 64; i++)
		{
			SetReg(*r2, REGADDR::PA_SU_SC_MODE_CNTL, (i & 1) << 11);
			SetReg(*r2, REGADDR::PA_CL_CLIP_CNTL, ((i >> 1) & 1) << 22);
			SetReg(*r2, REGADDR::PA_CL_VTE_CNTL, ((i >> 2) & 1) << 1);
			seq.insert(VulkanPipelineKey_RenderState(*r2, flags));
		}
		std::printf("pipeline keys for 64 depth bias and discard states with extended dynamic state 2: %zu\n", seq.size());
		CHECK(seq.size() == 1);
		// all three together: one key for every state the draw sets
		const uint32 all = VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND | flags;
		std::set<uint64> allKeys;
		for (uint32 i = 0; i < 1024; i++)
		{
			SetReg(*r2, REGADDR::PA_SU_SC_MODE_CNTL, (i & 7) | (((i >> 3) & 1) << 11));
			SetReg(*r2, REGADDR::PA_CL_CLIP_CNTL, ((i >> 4) & 1) << 22);
			SetReg(*r2, REGADDR::DB_DEPTH_CONTROL, i * 0x9E3779B1u);
			SetReg(*r2, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | ((i * 37 & 0xFF) << 8));
			SetReg(*r2, REGADDR::CB_TARGET_MASK, i * 0x85EBCA6Bu);
			allKeys.insert(VulkanPipelineKey_RenderState(*r2, all));
		}
		std::printf("pipeline keys for 1024 states with all of it dynamic: %zu\n", allKeys.size());
		CHECK(allKeys.size() == 1);
	}
	// the enables the draw sets and the pipeline bakes in
	{
		auto s2 = std::make_unique<LatteContextRegister>(*base);
		VulkanRasterizerEnables e = VulkanPipelineKey_Rasterizer(*s2);
		CHECK(!e.depthBiasEnable && !e.rasterizerDiscardEnable);
		SetReg(*s2, REGADDR::PA_SU_SC_MODE_CNTL, 1u << 11);
		SetReg(*s2, REGADDR::PA_CL_CLIP_CNTL, 1u << 22);
		SetReg(*s2, REGADDR::PA_CL_VTE_CNTL, 1u << 1);
		e = VulkanPipelineKey_Rasterizer(*s2);
		CHECK(e.depthBiasEnable && e.rasterizerDiscardEnable);
		// GX2SetSpecialState(0, true): the viewport's X offset off keeps rasterizing
		SetReg(*s2, REGADDR::PA_CL_VTE_CNTL, 0);
		e = VulkanPipelineKey_Rasterizer(*s2);
		CHECK(e.depthBiasEnable && !e.rasterizerDiscardEnable);
		// the key without the extension is as it was: discard only with the
		// viewport's X offset on, polygon offset by its enable
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::PA_SU_SC_MODE_CNTL, 1u << 11);
		CHECK(VulkanPipelineKey_RenderState(*a, 0) != key0);
	}
	// the part of the key the draws that keep their shaders compare
	{
		LatteParsedFetchShaderAttribute attrs[3]{};
		attrs[0] = {3, 5, Latte::E_HWFMT::HWFMT_32_32_32_FLOAT, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 4};
		attrs[1] = {3, 9, Latte::E_HWFMT::HWFMT_8_8_8_8, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 16};
		attrs[2] = {7, 2, Latte::E_HWFMT::HWFMT_16_16_FLOAT, LatteConst::INSTANCE_DATA, 0, 0, {}, {}, 1, 0};
		LatteFetchShader fs;
		LatteParsedFetchShaderBufferGroup g0{}, g1{};
		g0.attributeBufferIndex = 3;
		g0.attribCount = 2;
		g0.attrib = attrs;
		g1.attributeBufferIndex = 7;
		g1.attribCount = 1;
		g1.attrib = attrs + 2;
		fs.bufferGroups = {g0, g1};
		fs.vkPipelineHashFragment = 0x1234;
		auto stride = [](LatteContextRegister& r, uint32 buffer, uint32 v) {
			SetReg(r, mmSQ_VTX_ATTRIBUTE_BLOCK_START + buffer * 7 + 2, v << 11);
		};
		auto s0 = std::make_unique<LatteContextRegister>(*base);
		stride(*s0, 3, 20);
		stride(*s0, 7, 4);
		SetReg(*s0, mmVGT_PRIMITIVE_TYPE, 4);
		const uint32 vi = VULKAN_PIPELINE_KEY_DYNAMIC_VERTEX_INPUT;
		const uint32 rz = VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER;
		const uint64 m0 = VulkanPipelineKey_Minimal(&fs, *s0, 0);
		const uint64 v0 = VulkanPipelineKey_Minimal(&fs, *s0, vi);
		// strides: keyed, unless the draw sets them
		auto s1 = std::make_unique<LatteContextRegister>(*s0);
		stride(*s1, 3, 24);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s1, 0) != m0);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s1, vi) == v0);
		stride(*s1, 7, 8);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s1, vi) == v0);
		// a fetch shader with other attribute offsets: the same
		LatteFetchShader fs2;
		fs2.bufferGroups = fs.bufferGroups;
		fs2.vkPipelineHashFragment = 0x5678;
		CHECK(VulkanPipelineKey_Minimal(&fs2, *s0, 0) != m0);
		CHECK(VulkanPipelineKey_Minimal(&fs2, *s0, vi) == v0);
		// the primitive type and streamout: keyed
		auto s2 = std::make_unique<LatteContextRegister>(*s0);
		SetReg(*s2, mmVGT_PRIMITIVE_TYPE, 5);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, vi | rz) != VulkanPipelineKey_Minimal(&fs, *s0, vi | rz));
		s2 = std::make_unique<LatteContextRegister>(*s0);
		SetReg(*s2, mmVGT_STRMOUT_EN, 1);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, vi | rz) != VulkanPipelineKey_Minimal(&fs, *s0, vi | rz));
		// rasterizer discard: keyed here too, unless the draw sets it
		s2 = std::make_unique<LatteContextRegister>(*s0);
		SetReg(*s2, REGADDR::PA_CL_CLIP_CNTL, 1u << 22);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, 0) != m0);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, rz) == VulkanPipelineKey_Minimal(&fs, *s0, rz));
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, vi | rz) == VulkanPipelineKey_Minimal(&fs, *s0, vi | rz));
		// 256 vertex layouts, one key
		std::set<uint64> layouts;
		for (uint32 i = 0; i < 256; i++)
		{
			stride(*s1, 3, 12 + (i & 15) * 4);
			stride(*s1, 7, (i >> 4) * 2);
			LatteFetchShader f;
			f.bufferGroups = fs.bufferGroups;
			f.vkPipelineHashFragment = i * 0x9E3779B97F4A7C15ull;
			layouts.insert(VulkanPipelineKey_Minimal(&f, *s1, vi));
		}
		std::printf("pipeline keys for 256 vertex layouts with vertex input dynamic state: %zu\n", layouts.size());
		CHECK(layouts.size() == 1);

		// the primitive type: keyed, with dynamic topology by its class
		{
			using E = LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
			const uint32 tp = VULKAN_PIPELINE_KEY_DYNAMIC_TOPOLOGY;
			auto key = [&](E mode, uint32 flags) {
				auto r = std::make_unique<LatteContextRegister>(*s0);
				SetReg(*r, mmVGT_PRIMITIVE_TYPE, (uint32)mode);
				return VulkanPipelineKey_Minimal(&fs, *r, flags);
			};
			const E all[] = {E::POINTS, E::LINES, E::LINE_STRIP, E::TRIANGLES, E::TRIANGLE_FAN, E::TRIANGLE_STRIP,
				E::LINE_STRIP_ADJACENT, E::RECTS, E::LINE_LOOP, E::QUADS, E::QUAD_STRIP};
			std::set<uint64> raw, classes;
			for (E m : all)
			{
				raw.insert(key(m, 0));
				classes.insert(key(m, tp));
			}
			// points, lines, triangles, line strips with adjacency, rects
			std::printf("pipeline keys for %zu primitive types: %zu, with dynamic topology %zu\n", std::size(all), raw.size(), classes.size());
			CHECK(raw.size() == std::size(all));
			CHECK(classes.size() == 5);
			CHECK(key(E::TRIANGLES, tp) == key(E::TRIANGLE_STRIP, tp) && key(E::TRIANGLES, tp) == key(E::TRIANGLE_FAN, tp) &&
				key(E::TRIANGLES, tp) == key(E::QUADS, tp) && key(E::TRIANGLES, tp) == key(E::QUAD_STRIP, tp));
			CHECK(key(E::LINES, tp) == key(E::LINE_STRIP, tp) && key(E::LINES, tp) == key(E::LINE_LOOP, tp));
			CHECK(key(E::RECTS, tp) != key(E::TRIANGLES, tp));
			CHECK(key(E::LINE_STRIP_ADJACENT, tp) != key(E::LINE_STRIP, tp));
			CHECK(key(E::POINTS, tp) != key(E::LINES, tp) && key(E::POINTS, tp) != key(E::TRIANGLES, tp));
			// the topology and restart the draw sets and the pipeline bakes in
			struct Want { E mode; VkPrimitiveTopology topology; VkBool32 restart; };
			const Want want[] = {
				{E::POINTS, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE},
				{E::LINES, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_FALSE},
				{E::LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_TRUE},
				{E::LINE_LOOP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_TRUE},
				{E::LINE_STRIP_ADJACENT, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY, VK_TRUE},
				{E::TRIANGLES, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
				{E::TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_TRUE},
				{E::TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_TRUE},
				{E::QUADS, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
				{E::QUAD_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
				{E::RECTS, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
			};
			for (const Want& w : want)
			{
				const VulkanInputAssembly ia = VulkanPipelineKey_InputAssembly(w.mode);
				if (!ia.supported || ia.topology != w.topology || ia.primitiveRestartEnable != w.restart)
				{
					std::printf("FAIL: primitive type %x: topology %d restart %d\n", (uint32)w.mode, (int)ia.topology, (int)ia.primitiveRestartEnable);
					s_failures++;
				}
			}
			CHECK(!VulkanPipelineKey_InputAssembly(E::TRIANGLES_ADJACENT).supported);
		}
		// the whole key: each shader, the render pass and the rest
		{
			const uint32 f = vi | rz;
			const uint64 g = VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s0, f);
			CHECK(VulkanPipelineKey_Graphics(&fs, 12, 0, 13, 17, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 5, 13, 17, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 14, 17, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 18, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s1, f) == g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s1, 0) != VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s0, 0));
			auto c = std::make_unique<LatteContextRegister>(*s0);
			SetReg(*c, REGADDR::CB_BLEND0_CONTROL, 0x00030003);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *c, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *c, f | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND) ==
				VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s0, f | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND));
		}

		// the vertex input: the attributes the vertex shader maps, at its
		// locations, with the register's strides
		sint8 mapping[LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS];
		std::fill(std::begin(mapping), std::end(mapping), (sint8)-1);
		mapping[5] = 0;
		mapping[2] = 1; // semantic 9 not read by the shader
		auto in = std::make_unique<VulkanVertexInput>();
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in);
		CHECK(in->bindingCount == 2 && in->attributeCount == 2);
		if (in->bindingCount == 2 && in->attributeCount == 2)
		{
			CHECK(in->bindings[0].binding == 3 && in->bindings[0].stride == 20 && in->bindings[0].inputRate == VK_VERTEX_INPUT_RATE_VERTEX);
			CHECK(in->bindings[1].binding == 7 && in->bindings[1].stride == 4 && in->bindings[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
			CHECK(in->bindings[0].divisor == 1 && in->bindings[1].divisor == 1);
			CHECK(in->attributes[0].location == 0 && in->attributes[0].binding == 3 && in->attributes[0].offset == 4 &&
				in->attributes[0].format == VK_FORMAT_R32G32B32_UINT);
			CHECK(in->attributes[1].location == 1 && in->attributes[1].binding == 7 && in->attributes[1].offset == 0 &&
				in->attributes[1].format == VK_FORMAT_R16G16_UINT);
			CHECK(in->bindings[0].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT);
			CHECK(in->attributes[0].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT);
		}
		// what the draw compares to set it again
		auto in2 = std::make_unique<VulkanVertexInput>();
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(VulkanPipelineKey_SameVertexInput(*in, *in2));
		VulkanPipelineKey_VertexInput(fs, mapping, *s1, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[0].offset = 8;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[0].offset = 4;
		mapping[9] = 2;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		CHECK(in2->attributeCount == 3);
		mapping[9] = -1;
		attrs[2].format = Latte::E_HWFMT::HWFMT_32_FLOAT;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[2].format = Latte::E_HWFMT::HWFMT_16_16_FLOAT;
		attrs[2].fetchType = LatteConst::VERTEX_DATA;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[2].fetchType = LatteConst::INSTANCE_DATA;
		fs.bufferGroups.clear();
		fs2.bufferGroups.clear();
	}
	// graphics pipeline libraries: the part each dynamic state the renderer
	// sets belongs to
	{
		struct Want { VkDynamicState state; int part; };
		const Want want[] = {
			{VK_DYNAMIC_STATE_VIEWPORT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_SCISSOR, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_DEPTH_BIAS, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_CULL_MODE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_FRONT_FACE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_REFERENCE, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_OP_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_BLEND_CONSTANTS, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT},
			{VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT, VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT},
			{VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT},
		};
		for (const Want& w : want)
			if (VulkanPipelineLibrary_DynamicStatePart(w.state) != w.part)
			{
				std::printf("FAIL: dynamic state %d belongs to part %d, not %d\n", (int)w.state,
					VulkanPipelineLibrary_DynamicStatePart(w.state), w.part);
				s_failures++;
			}
	}
	// a part's key: what it bakes in, not what its dynamic states leave to
	// the draw, nor the other parts' state
	{
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkVertexInputBindingDescription binding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
		vi.vertexBindingDescriptionCount = 1;
		vi.pVertexBindingDescriptions = &binding;
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = vp.scissorCount = 1;
		VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.pNext = &clip;
		rs.cullMode = VK_CULL_MODE_BACK_BIT;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		ds.depthTestEnable = VK_TRUE;
		ds.depthCompareOp = VK_COMPARE_OP_LESS;
		VkPipelineColorBlendAttachmentState att{};
		att.blendEnable = VK_TRUE;
		att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		att.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &att;
		std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.pVertexInputState = &vi;
		gp.pInputAssemblyState = &ia;
		gp.pViewportState = &vp;
		gp.pRasterizationState = &rs;
		gp.pMultisampleState = &ms;
		gp.pDepthStencilState = &ds;
		gp.pColorBlendState = &cb;
		gp.pDynamicState = &dsi;
		const VulkanPipelineLibraryIdentity id{1, 0, 2, 3, false, false};
		auto keys = [&](const VulkanPipelineLibraryIdentity& identity) {
			dsi.dynamicStateCount = (uint32)dyn.size();
			dsi.pDynamicStates = dyn.data();
			std::vector<uint64> k;
			for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
				k.push_back(VulkanPipelineLibrary_Key((VulkanPipelineLibraryPart)p, gp, identity));
			return k;
		};
		// which parts a change moves: a bit per part
		auto moved = [&](const std::vector<uint64>& before, const std::vector<uint64>& after) {
			uint32 m = 0;
			for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
				m |= (before[p] != after[p]) << p;
			return m;
		};
		const uint32 VI = 1, PRS = 2, FS = 4, FO = 8;
		const std::vector<uint64> k0 = keys(id);
		std::set<uint64> distinct(k0.begin(), k0.end());
		CHECK(distinct.size() == 4);
		// the shaders, the render pass and robustness: each shader part's own,
		// so a part made ahead from one stage keys as its pipeline's whatever
		// the other stage asks for
		CHECK(moved(k0, keys({9, 0, 2, 3, false, false})) == PRS);
		CHECK(moved(k0, keys({1, 7, 2, 3, false, false})) == PRS);
		CHECK(moved(k0, keys({1, 0, 9, 3, false, false})) == FS);
		CHECK(moved(k0, keys({1, 0, 2, 9, false, false})) == (PRS | FS | FO));
		CHECK(moved(k0, keys({1, 0, 2, 3, true, false})) == PRS);
		CHECK(moved(k0, keys({1, 0, 2, 3, false, true})) == FS);
		CHECK(moved(k0, keys({1, 0, 2, 3, true, true})) == (PRS | FS));
		// baked state moves its own part only
		binding.stride = 20;
		CHECK(moved(k0, keys(id)) == VI);
		binding.stride = 16;
		rs.cullMode = VK_CULL_MODE_NONE;
		CHECK(moved(k0, keys(id)) == PRS);
		rs.cullMode = VK_CULL_MODE_BACK_BIT;
		clip.depthClipEnable = VK_TRUE;
		CHECK(moved(k0, keys(id)) == PRS);
		clip.depthClipEnable = VK_FALSE;
		ds.depthCompareOp = VK_COMPARE_OP_GREATER;
		CHECK(moved(k0, keys(id)) == FS);
		ds.depthCompareOp = VK_COMPARE_OP_LESS;
		att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		CHECK(moved(k0, keys(id)) == FO);
		att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
		CHECK(moved(k0, keys(id)) == (FS | FO));
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		CHECK(moved(k0, keys(id)) == 0);
		// with the state dynamic, its baked value keys nothing
		dyn.insert(dyn.end(), {VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_CULL_MODE_EXT,
			VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT});
		const std::vector<uint64> kd = keys(id);
		CHECK(moved(k0, kd) == (VI | PRS | FS | FO)); // the dynamic states themselves are keyed
		binding.stride = 20;
		rs.cullMode = VK_CULL_MODE_NONE;
		ds.depthCompareOp = VK_COMPARE_OP_GREATER;
		att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		CHECK(moved(kd, keys(id)) == 0);
		// a topology of another class still keys the vertex input
		ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		CHECK(moved(kd, keys(id)) == VI);
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		// and what stays baked still does
		rs.polygonMode = VK_POLYGON_MODE_LINE;
		CHECK(moved(kd, keys(id)) == PRS);
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		att.blendEnable = VK_FALSE;
		CHECK(moved(kd, keys(id)) == FO);
		att.blendEnable = VK_TRUE;
		ds.stencilTestEnable = VK_TRUE;
		CHECK(moved(kd, keys(id)) == FS);
	}
	// the cache: one maker per key, however many threads race for it
	{
		auto cache = std::make_unique<VulkanPipelineLibraryCache>();
		const VulkanPipelineLibraryPart part = VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER;
		VkPipeline found;
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Missing);
		CHECK(cache->Claim(part, 42));
		CHECK(!cache->Claim(part, 42));
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Pending);
		// a part that failed: missing again, and claimed again
		cache->Publish(part, 42, VK_NULL_HANDLE);
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Missing);
		CHECK(cache->Claim(part, 42));
		cache->Publish(part, 42, (VkPipeline)(uintptr_t)0x4200);
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Ready && found == (VkPipeline)(uintptr_t)0x4200);
		CHECK(!cache->Claim(part, 42));
		// other parts are other tables
		CHECK(cache->Find(VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT, 42, found) == VulkanPipelineLibraryCache::State::Missing);
		cache->DestroyAll(VK_NULL_HANDLE);
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Missing && cache->Count(part) == 0);

		// eight threads claiming the same 4000 keys, colliding in the table:
		// each key made once, every thread then finds every part
		const int threads = 8, keys = 4000;
		std::vector<std::atomic<int>> makers(keys);
		std::vector<std::thread> pool;
		std::atomic<int> wrong{0};
		for (int t = 0; t < threads; t++)
			pool.emplace_back([&, t] {
				for (int n = 0; n < keys; n++)
				{
					const int i = (n * 7 + t * 613) % keys;
					const uint64 key = 0x100000000ull * (uint64)(i % 61) + (uint64)i + 1; // shared low bits
					if (cache->Claim(part, key))
					{
						makers[i].fetch_add(1);
						cache->Publish(part, key, (VkPipeline)(uintptr_t)(0x1000 + i));
					}
				}
				for (int i = 0; i < keys; i++)
				{
					const uint64 key = 0x100000000ull * (uint64)(i % 61) + (uint64)i + 1;
					VkPipeline f;
					// made, or still being made by another thread
					const auto state = cache->Find(part, key, f);
					if (state == VulkanPipelineLibraryCache::State::Missing ||
						(state == VulkanPipelineLibraryCache::State::Ready && f != (VkPipeline)(uintptr_t)(0x1000 + i)))
						wrong++;
				}
			});
		for (auto& t : pool)
			t.join();
		int once = 0;
		for (auto& m : makers)
			once += m.load() == 1;
		std::printf("library cache: %d keys claimed by %d threads, %d made once, %zu kept\n", keys, threads, once, cache->Count(part));
		CHECK(once == keys && wrong == 0 && cache->Count(part) == (size_t)keys);
		cache->DestroyAll(VK_NULL_HANDLE);
		// a full table: no claim, the part adopted instead
		for (size_t i = 0; i < VulkanPipelineLibraryCache::kCapacity; i++)
			CHECK(cache->Claim(part, i + 1));
		CHECK(!cache->Claim(part, VulkanPipelineLibraryCache::kCapacity + 7));
		CHECK(cache->Find(part, VulkanPipelineLibraryCache::kCapacity + 7, found) == VulkanPipelineLibraryCache::State::Missing);
		cache->Adopt((VkPipeline)(uintptr_t)0x77);
		cache->DestroyAll(VK_NULL_HANDLE);
		CHECK(cache->Claim(part, 5));
	}
	// the blend state the draw sets and the pipeline bakes in
	{
		auto s2 = std::make_unique<LatteContextRegister>(*base);
		VkFormat formats[8] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM};
		// blending on 0 and 1; 1 is an integer format
		SetReg(*s2, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (3 << 8));
		// 0: src alpha, one minus src alpha, add
		SetReg(*s2, REGADDR::CB_BLEND0_CONTROL, 4 | (0 << 5) | (5 << 8));
		// 1: separate alpha, a constant color factor
		SetReg(*s2, REGADDR::CB_BLEND0_CONTROL + 1, 1 | (0xD << 8) | (1u << 29) | (1 << 16) | (0 << 24));
		SetReg(*s2, REGADDR::CB_TARGET_MASK, 0x7F);
		VkPipelineColorBlendAttachmentState att[8];
		const bool constants = VulkanPipelineKey_Blend(*s2, formats, att);
		CHECK(att[0].blendEnable == VK_TRUE && att[1].blendEnable == VK_FALSE && att[2].blendEnable == VK_FALSE);
		CHECK(att[0].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA && att[0].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
		CHECK(att[0].colorBlendOp == VK_BLEND_OP_ADD && att[0].srcAlphaBlendFactor == att[0].srcColorBlendFactor);
		CHECK(att[0].colorWriteMask == 0xF && att[1].colorWriteMask == 0x7 && att[2].colorWriteMask == 0);
		CHECK(att[1].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR && att[1].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
		CHECK(constants);
		SetReg(*s2, REGADDR::CB_BLEND0_CONTROL + 1, 0);
		CHECK(!VulkanPipelineKey_Blend(*s2, formats, att));
	}
	// what the draw sets is what the pipeline bakes in
	{
		auto s2 = std::make_unique<LatteContextRegister>(*base);
		VulkanDepthStencilCull d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.depthTestEnable && d.depthWriteEnable && d.depthCompareOp == VK_COMPARE_OP_LESS_OR_EQUAL);
		CHECK(d.stencilTestEnable && d.frontCompareOp == VK_COMPARE_OP_EQUAL && d.backCompareOp == VK_COMPARE_OP_EQUAL);
		CHECK(d.cullMode == VK_CULL_MODE_NONE && d.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
		// one-sided stencil: the back face as the front
		SetReg(*s2, REGADDR::DB_DEPTH_CONTROL, (depthControl & ~(1u << 7)) | (5u << 14));
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.backCompareOp == d.frontCompareOp && d.backPassOp == d.frontPassOp && d.frontPassOp == VK_STENCIL_OP_INVERT);
		// cull back, clockwise
		SetReg(*s2, REGADDR::PA_SU_SC_MODE_CNTL, 2 | 4);
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_BACK_BIT && d.frontFace == VK_FRONT_FACE_CLOCKWISE);
		// rects: culled as one winding, either bit culls both faces
		SetReg(*s2, REGADDR::VGT_PRIMITIVE_TYPE, (uint32)LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS);
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_FRONT_AND_BACK);
		SetReg(*s2, REGADDR::PA_SU_SC_MODE_CNTL, 1); // cull front, counter-clockwise
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_FRONT_AND_BACK);
		SetReg(*s2, REGADDR::VGT_PRIMITIVE_TYPE, 4); // triangles: the front alone
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_FRONT_BIT);
	}
	TestPipelineState();
	TestPartTemplates();
	TestWaitFor();
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("pipeline_key: all passed\n");
	return 0;
}
