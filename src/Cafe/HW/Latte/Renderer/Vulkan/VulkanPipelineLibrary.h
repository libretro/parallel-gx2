#pragma once

#include <atomic>
#include <memory>
#include <vulkan/vulkan.h>

#include "util/helpers/FixedQueue.h"
#include "util/helpers/EventWake.h"

// Graphics pipeline libraries (VK_EXT_graphics_pipeline_library,
// docs/SHADER-STUTTER.md M2): a pipeline as four parts, each compiled once
// and shared by every pipeline that has the same part, and linked into a
// pipeline at the draw - a link, not a compile (AGENTS.md: never compile a
// pipeline on the draw path). The parts are made from the create info the
// pipeline compiler fills for a whole pipeline; each takes its own state,
// its shader stages and its dynamic states from it.
enum VulkanPipelineLibraryPart
{
	VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT = 0, // vertex input, input assembly
	VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION = 1, // vertex and geometry shaders, viewport, rasterizer
	VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER = 2, // pixel shader, depth and stencil, multisampling
	VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT = 3, // blending, multisampling, the attachments
	VULKAN_PIPELINE_LIBRARY_PARTS = 4,
};

// The part a dynamic state belongs to; -1 for one none of the parts takes
int VulkanPipelineLibrary_DynamicStatePart(VkDynamicState state);

// What tells a part apart from another: the shaders it runs (their hashes;
// shader module handles are reused by the driver), the render pass it is
// compatible with (VKRObjectRenderPass::m_hashForPipeline) and whether its
// own shaders' buffer access is robust (VK_EXT_pipeline_robustness chained
// to that part alone: a part's shaders take it whatever the other part's
// need) - and its state from the create info, without the fields its
// dynamic states make the draw's
struct VulkanPipelineLibraryIdentity
{
	uint64 vertexShader;
	uint64 geometryShader;
	uint64 pixelShader;
	uint64 renderPass;
	bool robustVertex; // the pre-rasterization part's
	bool robustPixel; // the fragment shader part's
};
uint64 VulkanPipelineLibrary_Key(VulkanPipelineLibraryPart part, const VkGraphicsPipelineCreateInfo& full,
	const VulkanPipelineLibraryIdentity& identity);

// The part of the pipeline full describes, as a library that keeps what a
// link with optimization wants. layout: the part's own, made with
// VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, with the sets of the
// part's shaders only (none for the vertex input and fragment output)
VkResult VulkanPipelineLibrary_Create(VkDevice device, VkPipelineCache cache, VulkanPipelineLibraryPart part,
	const VkGraphicsPipelineCreateInfo& full, VkPipelineLayout layout, VkPipeline* library);
// The four parts linked into a pipeline: as they are (fast, for the draw)
// or optimized across them (for a worker). layout: the whole pipeline's,
// made with VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT. The parts
// have to outlive the pipeline: one drawn with after its parts are gone
// draws wrong (lavapipe does, test/dynstate)
VkResult VulkanPipelineLibrary_Link(VkDevice device, VkPipelineCache cache, const VkPipeline libraries[VULKAN_PIPELINE_LIBRARY_PARTS],
	VkPipelineLayout layout, bool optimize, VkPipeline* pipeline);
// How a pipeline whose parts are made is linked: fast when a draw waits for
// it - on the GPU thread, or on a compile thread for a draw skipped until
// it lands - with the optimized link queued for a compile thread to swap
// in (queueOptimized); optimized at once when nothing waits (made ahead)
bool VulkanPipelineLibrary_LinkOptimized(bool drawWaits, bool* queueOptimized);

// What a pipeline's compile (PipelineCompiler::Compile) does: compile what
// is missing (forced: a draw that may not be skipped, or a compile thread);
// take only what is made or in the driver's cache (a try, asking the cache
// with VK_EXT_pipeline_creation_cache_control); or take only what is made -
// parts already made, linked, and no pipeline otherwise - where the device
// cannot ask its cache without compiling, which would compile on the draw
// path: the draw is then skipped until a compile thread lands the pipeline,
// as for one the cache does not have. A try at a pipeline known to be in
// the driver's cache there (driverCached: the pipeline cache loaded it at
// boot, or it was made before) makes it, its shaders compiled: a hit in
// that cache, the lookup a device with cache control makes, rather than
// skipping its draw until a compile thread makes the same lookup
enum class VulkanPipelineTry
{
	Compile,
	FromCache,
	Made,
};
VulkanPipelineTry VulkanPipeline_Try(bool forceCompile, bool cacheControl, bool driverCached);

// The parts made so far, found by key from any thread, without a lock: an
// insert-only table per part. A thread claims a key before making its part
// and publishes it when made; the others find it pending until then. A
// part made without a claim - its key pending elsewhere, or the table full
// - is adopted. All of them live until DestroyAll, after the last pipeline
// linked from them is gone.
class VulkanPipelineLibraryCache
{
public:
	VulkanPipelineLibraryCache();
	~VulkanPipelineLibraryCache();

	enum class State
	{
		Missing, // claimed by nobody
		Pending, // claimed, being made
		Ready,
	};
	// The part with the key, if made
	State Find(VulkanPipelineLibraryPart part, uint64 key, VkPipeline& library) const;
	// The part with the key once made: waits while the thread that claimed
	// it makes it - which it does at once, waiting for nothing - rather than
	// make it a second time; Missing when unclaimed or failed
	State WaitFor(VulkanPipelineLibraryPart part, uint64 key, VkPipeline& library) const;
	// Claims the key for the calling thread to make the part; false if
	// another thread has (it is pending or made) or the table is full
	bool Claim(VulkanPipelineLibraryPart part, uint64 key);
	// The part made for a key the thread claimed; VK_NULL_HANDLE if making
	// it failed (found Missing again then, and claimed again by the next
	// thread to try)
	void Publish(VulkanPipelineLibraryPart part, uint64 key, VkPipeline library);
	// A part made without a claim, kept with the rest
	void Adopt(VkPipeline library) { m_adopted.Push(library); }
	// Every part made, destroyed; no thread may use the cache meanwhile,
	// and no pipeline linked from them may be left
	void DestroyAll(VkDevice device);
	size_t Count(VulkanPipelineLibraryPart part) const;

	static constexpr size_t kCapacity = 1 << 14; // per part

private:
	struct Slot
	{
		std::atomic<uint64> key; // 0: empty
		std::atomic<uint64> library; // 0: pending; kFailed: failed
	};
	Slot* FindSlot(VulkanPipelineLibraryPart part, uint64 key) const;
	std::unique_ptr<Slot[]> m_slots[VULKAN_PIPELINE_LIBRARY_PARTS];
	std::atomic<size_t> m_count[VULKAN_PIPELINE_LIBRARY_PARTS];
	FixedList<VkPipeline> m_adopted;
	// every part published, for WaitFor
	mutable EventWake m_published;
};

// Parts made ahead (docs/SHADER-STUTTER.md M2): the state of a shader part -
// the pre-rasterization shaders' or the fragment shader's - taken from a
// pipeline the renderer made, without its shaders, so that a new shader's
// part can be made from it on a compile thread before a pipeline needs it,
// one per render pass and state seen so far. Copies, with no pointer into
// the pipeline it came from.
struct VulkanPipelinePartTemplate
{
	VulkanPipelineLibraryPart part; // PRE_RASTERIZATION or FRAGMENT_SHADER
	uint64 key; // the part's key with no shaders, which tells templates apart
	uint64 renderPassHash;
	VkRenderPass renderPass;
	uint32 subpass;
	uint32 dynamicCount;
	VkDynamicState dynamic[32]; // the part's own
	VkPipelineViewportStateCreateInfo viewport;
	VkViewport viewports[1];
	VkRect2D scissors[1];
	VkPipelineRasterizationStateCreateInfo rasterizer;
	VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClip;
	bool hasDepthClip;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkSampleMask sampleMask;
	bool hasSampleMask;
};
// The template of part in full; false for a part with no template (vertex
// input, fragment output) or state it cannot copy (more than one viewport,
// an unknown structure chained to the rasterizer)
bool VulkanPipelineLibrary_Template(VulkanPipelineLibraryPart part, const VkGraphicsPipelineCreateInfo& full,
	uint64 renderPassHash, VulkanPipelinePartTemplate& out);

// A pipeline's create info as a template and a shader stage make it: what
// VulkanPipelineLibrary_Key and _Create take for the part. stage: the
// part's shader (the vertex shader, no geometry shader, for
// pre-rasterization), null for none; next: what is chained to the pipeline
// (robust buffer access)
struct VulkanPipelinePartAhead
{
	VkGraphicsPipelineCreateInfo full;
	VkPipelineShaderStageCreateInfo stage;
	VkPipelineDynamicStateCreateInfo dynamicState;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo rasterizer;
	VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClip;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineMultisampleStateCreateInfo multisample;
};
void VulkanPipelineLibrary_FromTemplate(const VulkanPipelinePartTemplate& t, const VkPipelineShaderStageCreateInfo* stage,
	const void* next, VulkanPipelinePartAhead& out);

// The templates seen so far: a fixed table of the most recently used, which
// the GPU thread records into and takes from, and the compile threads read
// the slots they were given. A slot given out is not replaced until every
// job given it has dropped it. The render pass a template was made with is
// kept alive by the slot (retain and release, on the recording thread).
class VulkanPipelinePartTemplates
{
public:
	static constexpr uint32 kSlots = 64;
	using RetainFn = void (*)(void* owner, bool retain);

	explicit VulkanPipelinePartTemplates(RetainFn retain) : m_retain(retain) {}
	~VulkanPipelinePartTemplates() { Clear(); }
	VulkanPipelinePartTemplates(const VulkanPipelinePartTemplates&) = delete;
	VulkanPipelinePartTemplates& operator=(const VulkanPipelinePartTemplates&) = delete;

	// The recording thread's: the template seen again, or kept in a free
	// slot or the least recently used one no job holds; false when every
	// slot is held
	bool Record(const VulkanPipelinePartTemplate& t, void* owner);
	// The recording thread's: the slots of a part's templates, but the one
	// keyed skipKey, each held by the caller until it drops them
	uint64 Take(VulkanPipelineLibraryPart part, uint64 skipKey);
	// Any thread's, for a slot it holds
	const VulkanPipelinePartTemplate& Get(uint32 slot) const { return m_slots[slot].t; }
	// Any thread's: the slots let go of
	void Drop(uint64 slots);
	// The recording thread's, with no slot held: every template forgotten
	void Clear();
	uint32 Count() const;
	uint32 Held(uint32 slot) const { return m_slots[slot].users.load(std::memory_order_acquire); }

private:
	struct Slot
	{
		VulkanPipelinePartTemplate t{};
		void* owner{};
		uint64 lastUse{};
		bool used{};
		std::atomic<uint32> users{0};
	};
	Slot m_slots[kSlots];
	uint64 m_clock{0};
	RetainFn m_retain;
};
