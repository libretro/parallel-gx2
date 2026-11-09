#pragma once

#include <atomic>
#include <memory>
#include <vulkan/vulkan.h>

#include "util/helpers/MpscList.h"

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
// buffer access is robust - and its state from the create info, without
// the fields its dynamic states make the draw's
struct VulkanPipelineLibraryIdentity
{
	uint64 vertexShader;
	uint64 geometryShader;
	uint64 pixelShader;
	uint64 renderPass;
	bool robustBufferAccess;
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
	MpscList<VkPipeline> m_adopted;
};
