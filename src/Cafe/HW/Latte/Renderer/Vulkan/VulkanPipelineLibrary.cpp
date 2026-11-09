#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

#include <algorithm>
#include <bit>

int VulkanPipelineLibrary_DynamicStatePart(VkDynamicState state)
{
	switch (state)
	{
	case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
	case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
	case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
	case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
		return VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT;
	case VK_DYNAMIC_STATE_VIEWPORT:
	case VK_DYNAMIC_STATE_SCISSOR:
	case VK_DYNAMIC_STATE_LINE_WIDTH:
	case VK_DYNAMIC_STATE_DEPTH_BIAS:
	case VK_DYNAMIC_STATE_CULL_MODE:
	case VK_DYNAMIC_STATE_FRONT_FACE:
	case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
	case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
	case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
	case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
		return VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION;
	case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
	case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
	case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
	case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
	case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
	case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
	case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
	case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
	case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
	case VK_DYNAMIC_STATE_STENCIL_OP:
		return VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER;
	case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
	case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
	case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
	case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
	case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
	case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
		return VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT;
	default:
		return -1;
	}
}

static const VkGraphicsPipelineLibraryFlagsEXT kPartFlags[VULKAN_PIPELINE_LIBRARY_PARTS] = {
	VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
	VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
	VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
	VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
};

static bool StageInPart(VkShaderStageFlagBits stage, VulkanPipelineLibraryPart part)
{
	if (part == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION)
		return stage == VK_SHADER_STAGE_VERTEX_BIT || stage == VK_SHADER_STAGE_GEOMETRY_BIT ||
			stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT || stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	if (part == VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER)
		return stage == VK_SHADER_STAGE_FRAGMENT_BIT;
	return false;
}

// the part's dynamic states, in the create info's order: on the stack, a
// key or part made without an allocation
struct PartDynamicStates
{
	VkDynamicState states[64];
	uint32 count = 0;
	PartDynamicStates(VulkanPipelineLibraryPart part, const VkGraphicsPipelineCreateInfo& full)
	{
		if (full.pDynamicState)
			for (uint32 i = 0; i < full.pDynamicState->dynamicStateCount && count < std::size(states); i++)
				if (VulkanPipelineLibrary_DynamicStatePart(full.pDynamicState->pDynamicStates[i]) == (int)part)
					states[count++] = full.pDynamicState->pDynamicStates[i];
	}
	VkDynamicState* begin() { return states; }
	VkDynamicState* end() { return states + count; }
};

namespace
{
	struct Hasher
	{
		uint64 h = 0xCBF29CE484222325ull;
		void Add(uint64 v)
		{
			h = std::rotl<uint64>((h ^ v) * 0x100000001B3ull, 29) + 0x9E3779B97F4A7C15ull;
		}
		void AddFloat(float f) { Add(std::bit_cast<uint32>(f)); }
	};
}

uint64 VulkanPipelineLibrary_Key(VulkanPipelineLibraryPart part, const VkGraphicsPipelineCreateInfo& full,
	const VulkanPipelineLibraryIdentity& identity)
{
	PartDynamicStates dyn(part, full);
	std::sort(dyn.begin(), dyn.end());
	auto isDynamic = [&](VkDynamicState s) { return std::binary_search(dyn.begin(), dyn.end(), s); };
	Hasher k;
	k.Add(part);
	for (VkDynamicState s : dyn)
		k.Add(s);
	switch (part)
	{
	case VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT:
	{
		const VkPipelineVertexInputStateCreateInfo* vi = full.pVertexInputState;
		if (vi && !isDynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT))
		{
			const bool dynamicStride = isDynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
			k.Add(vi->vertexBindingDescriptionCount);
			for (uint32 i = 0; i < vi->vertexBindingDescriptionCount; i++)
			{
				const auto& b = vi->pVertexBindingDescriptions[i];
				k.Add(b.binding);
				k.Add(dynamicStride ? 0 : b.stride);
				k.Add(b.inputRate);
			}
			k.Add(vi->vertexAttributeDescriptionCount);
			for (uint32 i = 0; i < vi->vertexAttributeDescriptionCount; i++)
			{
				const auto& a = vi->pVertexAttributeDescriptions[i];
				k.Add(a.location);
				k.Add(a.binding);
				k.Add(a.format);
				k.Add(a.offset);
			}
		}
		const VkPipelineInputAssemblyStateCreateInfo* ia = full.pInputAssemblyState;
		if (ia)
		{
			if (isDynamic(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY))
			{
				// the class the draw's topology has to be of
				switch (ia->topology)
				{
				case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
					k.Add(1);
					break;
				case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
				case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
				case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
				case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
					k.Add(2);
					break;
				case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
					k.Add(4);
					break;
				default:
					k.Add(3);
					break;
				}
			}
			else
				k.Add(ia->topology);
			k.Add(isDynamic(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE) ? 2 : ia->primitiveRestartEnable);
		}
		break;
	}
	case VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION:
	{
		k.Add(identity.vertexShader);
		k.Add(identity.geometryShader);
		k.Add(identity.renderPass);
		k.Add(identity.robustBufferAccess);
		if (const VkPipelineViewportStateCreateInfo* vp = full.pViewportState)
		{
			k.Add(isDynamic(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) ? 0 : vp->viewportCount);
			k.Add(isDynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) ? 0 : vp->scissorCount);
			if (!isDynamic(VK_DYNAMIC_STATE_VIEWPORT) && !isDynamic(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT))
				for (uint32 i = 0; vp->pViewports && i < vp->viewportCount; i++)
				{
					k.AddFloat(vp->pViewports[i].x);
					k.AddFloat(vp->pViewports[i].y);
					k.AddFloat(vp->pViewports[i].width);
					k.AddFloat(vp->pViewports[i].height);
					k.AddFloat(vp->pViewports[i].minDepth);
					k.AddFloat(vp->pViewports[i].maxDepth);
				}
			if (!isDynamic(VK_DYNAMIC_STATE_SCISSOR) && !isDynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT))
				for (uint32 i = 0; vp->pScissors && i < vp->scissorCount; i++)
				{
					k.Add((uint32)vp->pScissors[i].offset.x);
					k.Add((uint32)vp->pScissors[i].offset.y);
					k.Add(vp->pScissors[i].extent.width);
					k.Add(vp->pScissors[i].extent.height);
				}
		}
		if (const VkPipelineRasterizationStateCreateInfo* rs = full.pRasterizationState)
		{
			k.Add(rs->depthClampEnable);
			k.Add(isDynamic(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) ? 2 : rs->rasterizerDiscardEnable);
			k.Add(rs->polygonMode);
			k.Add(isDynamic(VK_DYNAMIC_STATE_CULL_MODE) ? 0xFF : rs->cullMode);
			k.Add(isDynamic(VK_DYNAMIC_STATE_FRONT_FACE) ? 2 : rs->frontFace);
			k.Add(isDynamic(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE) ? 2 : rs->depthBiasEnable);
			if (!isDynamic(VK_DYNAMIC_STATE_DEPTH_BIAS))
			{
				k.AddFloat(rs->depthBiasConstantFactor);
				k.AddFloat(rs->depthBiasClamp);
				k.AddFloat(rs->depthBiasSlopeFactor);
			}
			if (!isDynamic(VK_DYNAMIC_STATE_LINE_WIDTH))
				k.AddFloat(rs->lineWidth);
			// what is chained to it: the depth clip control (the one the
			// compiler chains); any other by its type, which keys it apart
			for (const VkBaseInStructure* ext = (const VkBaseInStructure*)rs->pNext; ext; ext = ext->pNext)
			{
				k.Add(ext->sType);
				if (ext->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT)
					k.Add(((const VkPipelineRasterizationDepthClipStateCreateInfoEXT*)ext)->depthClipEnable);
			}
		}
		break;
	}
	case VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER:
	case VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT:
	{
		k.Add(identity.renderPass);
		if (const VkPipelineMultisampleStateCreateInfo* ms = full.pMultisampleState)
		{
			k.Add(ms->rasterizationSamples);
			k.Add(ms->sampleShadingEnable);
			k.AddFloat(ms->minSampleShading);
			k.Add(ms->pSampleMask ? 1 + (uint64)ms->pSampleMask[0] : 0);
			k.Add(ms->alphaToCoverageEnable);
			k.Add(ms->alphaToOneEnable);
		}
		if (part == VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER)
		{
			k.Add(identity.pixelShader);
			k.Add(identity.robustBufferAccess);
			if (const VkPipelineDepthStencilStateCreateInfo* ds = full.pDepthStencilState)
			{
				k.Add(isDynamic(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) ? 2 : ds->depthTestEnable);
				k.Add(isDynamic(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE) ? 2 : ds->depthWriteEnable);
				k.Add(isDynamic(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP) ? 0xFF : ds->depthCompareOp);
				k.Add(isDynamic(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE) ? 2 : ds->depthBoundsTestEnable);
				k.Add(isDynamic(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE) ? 2 : ds->stencilTestEnable);
				for (const VkStencilOpState* op : {&ds->front, &ds->back})
				{
					if (!isDynamic(VK_DYNAMIC_STATE_STENCIL_OP))
					{
						k.Add(op->failOp);
						k.Add(op->passOp);
						k.Add(op->depthFailOp);
						k.Add(op->compareOp);
					}
					k.Add(isDynamic(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK) ? 0 : op->compareMask);
					k.Add(isDynamic(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK) ? 0 : op->writeMask);
					k.Add(isDynamic(VK_DYNAMIC_STATE_STENCIL_REFERENCE) ? 0 : op->reference);
				}
				if (!isDynamic(VK_DYNAMIC_STATE_DEPTH_BOUNDS))
				{
					k.AddFloat(ds->minDepthBounds);
					k.AddFloat(ds->maxDepthBounds);
				}
			}
		}
		else if (const VkPipelineColorBlendStateCreateInfo* cb = full.pColorBlendState)
		{
			k.Add(cb->logicOpEnable);
			k.Add(isDynamic(VK_DYNAMIC_STATE_LOGIC_OP_EXT) ? 0xFF : cb->logicOp);
			k.Add(cb->attachmentCount);
			for (uint32 i = 0; i < cb->attachmentCount; i++)
			{
				const VkPipelineColorBlendAttachmentState& a = cb->pAttachments[i];
				k.Add(isDynamic(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT) ? 2 : a.blendEnable);
				if (!isDynamic(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT))
				{
					k.Add(a.srcColorBlendFactor);
					k.Add(a.dstColorBlendFactor);
					k.Add(a.colorBlendOp);
					k.Add(a.srcAlphaBlendFactor);
					k.Add(a.dstAlphaBlendFactor);
					k.Add(a.alphaBlendOp);
				}
				k.Add(isDynamic(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT) ? 0xFF : a.colorWriteMask);
			}
			if (!isDynamic(VK_DYNAMIC_STATE_BLEND_CONSTANTS))
				for (float c : cb->blendConstants)
					k.AddFloat(c);
		}
		break;
	}
	default:
		break;
	}
	// 0 marks an empty slot in the cache
	return k.h ? k.h : 1;
}

VkResult VulkanPipelineLibrary_Create(VkDevice device, VkPipelineCache cache, VulkanPipelineLibraryPart part,
	const VkGraphicsPipelineCreateInfo& full, VkPipelineLayout layout, VkPipeline* library)
{
	const PartDynamicStates dyn(part, full);
	VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamicState.dynamicStateCount = dyn.count;
	dynamicState.pDynamicStates = dyn.states;
	VkPipelineShaderStageCreateInfo stages[4];
	uint32 stageCount = 0;
	for (uint32 i = 0; i < full.stageCount && stageCount < std::size(stages); i++)
		if (StageInPart(full.pStages[i].stage, part))
			stages[stageCount++] = full.pStages[i];

	// what the part takes of the whole: its library flag in front of what
	// was chained (robust buffer access), and only its own state
	VkGraphicsPipelineLibraryCreateInfoEXT libraryInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
	libraryInfo.pNext = full.pNext;
	libraryInfo.flags = kPartFlags[part];
	VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	ci.pNext = &libraryInfo;
	ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT |
		(full.flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT);
	ci.stageCount = stageCount;
	ci.pStages = stageCount ? stages : nullptr;
	ci.pDynamicState = dyn.count ? &dynamicState : nullptr;
	switch (part)
	{
	case VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT:
		ci.pVertexInputState = full.pVertexInputState;
		ci.pInputAssemblyState = full.pInputAssemblyState;
		break;
	case VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION:
		ci.pViewportState = full.pViewportState;
		ci.pRasterizationState = full.pRasterizationState;
		ci.pTessellationState = full.pTessellationState;
		ci.layout = layout;
		ci.renderPass = full.renderPass;
		ci.subpass = full.subpass;
		break;
	case VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER:
		ci.pDepthStencilState = full.pDepthStencilState;
		ci.pMultisampleState = full.pMultisampleState;
		ci.layout = layout;
		ci.renderPass = full.renderPass;
		ci.subpass = full.subpass;
		break;
	case VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT:
		ci.pColorBlendState = full.pColorBlendState;
		ci.pMultisampleState = full.pMultisampleState;
		ci.renderPass = full.renderPass;
		ci.subpass = full.subpass;
		break;
	default:
		return VK_ERROR_UNKNOWN;
	}
	return vkCreateGraphicsPipelines(device, cache, 1, &ci, nullptr, library);
}

VkResult VulkanPipelineLibrary_Link(VkDevice device, VkPipelineCache cache, const VkPipeline libraries[VULKAN_PIPELINE_LIBRARY_PARTS],
	VkPipelineLayout layout, bool optimize, VkPipeline* pipeline)
{
	VkPipelineLibraryCreateInfoKHR linkInfo{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
	linkInfo.libraryCount = VULKAN_PIPELINE_LIBRARY_PARTS;
	linkInfo.pLibraries = libraries;
	VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	ci.pNext = &linkInfo;
	ci.flags = optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
	ci.layout = layout;
	return vkCreateGraphicsPipelines(device, cache, 1, &ci, nullptr, pipeline);
}

// --- the cache ---------------------------------------------------------------

static constexpr uint64 kFailed = ~0ull;

VulkanPipelineLibraryCache::VulkanPipelineLibraryCache()
{
	for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
	{
		m_slots[p] = std::make_unique<Slot[]>(kCapacity);
		for (size_t i = 0; i < kCapacity; i++)
		{
			m_slots[p][i].key.store(0, std::memory_order_relaxed);
			m_slots[p][i].library.store(0, std::memory_order_relaxed);
		}
		m_count[p].store(0, std::memory_order_relaxed);
	}
}

VulkanPipelineLibraryCache::~VulkanPipelineLibraryCache() = default;

// the key's slot, or the empty one where it would go, or null when the
// table is full; linear probing from the key's hash
VulkanPipelineLibraryCache::Slot* VulkanPipelineLibraryCache::FindSlot(VulkanPipelineLibraryPart part, uint64 key) const
{
	Slot* slots = m_slots[part].get();
	size_t i = (size_t)(key ^ (key >> 29)) & (kCapacity - 1);
	for (size_t n = 0; n < kCapacity; n++, i = (i + 1) & (kCapacity - 1))
	{
		const uint64 k = slots[i].key.load(std::memory_order_acquire);
		if (k == key || k == 0)
			return &slots[i];
	}
	return nullptr;
}

VulkanPipelineLibraryCache::State VulkanPipelineLibraryCache::Find(VulkanPipelineLibraryPart part, uint64 key, VkPipeline& library) const
{
	library = VK_NULL_HANDLE;
	const Slot* slot = FindSlot(part, key);
	if (!slot || slot->key.load(std::memory_order_acquire) != key)
		return State::Missing;
	const uint64 v = slot->library.load(std::memory_order_acquire);
	if (v == 0)
		return State::Pending;
	if (v == kFailed)
		return State::Missing;
	library = (VkPipeline)v;
	return State::Ready;
}

bool VulkanPipelineLibraryCache::Claim(VulkanPipelineLibraryPart part, uint64 key)
{
	for (;;)
	{
		Slot* slot = FindSlot(part, key);
		if (!slot)
			return false; // full: made by the caller, not kept
		uint64 expected = 0;
		if (slot->key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
			return true; // a new slot, pending
		if (expected != key)
			continue; // another key took the empty slot first: probe on
		// the key's own slot: claimable again only after a failure
		uint64 failed = kFailed;
		return slot->library.compare_exchange_strong(failed, 0, std::memory_order_acq_rel);
	}
}

void VulkanPipelineLibraryCache::Publish(VulkanPipelineLibraryPart part, uint64 key, VkPipeline library)
{
	Slot* slot = FindSlot(part, key);
	if (!slot || slot->key.load(std::memory_order_acquire) != key)
		return;
	if (library != VK_NULL_HANDLE)
		m_count[part].fetch_add(1, std::memory_order_relaxed);
	slot->library.store(library != VK_NULL_HANDLE ? (uint64)library : kFailed, std::memory_order_release);
}

void VulkanPipelineLibraryCache::DestroyAll(VkDevice device)
{
	m_adopted.TakeAll([&](VkPipeline&& library) {
		if (device != VK_NULL_HANDLE)
			vkDestroyPipeline(device, library, nullptr);
	});
	for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
	{
		for (size_t i = 0; i < kCapacity; i++)
		{
			const uint64 v = m_slots[p][i].library.load(std::memory_order_acquire);
			if (v != 0 && v != kFailed && device != VK_NULL_HANDLE)
				vkDestroyPipeline(device, (VkPipeline)v, nullptr);
			m_slots[p][i].key.store(0, std::memory_order_relaxed);
			m_slots[p][i].library.store(0, std::memory_order_relaxed);
		}
		m_count[p].store(0, std::memory_order_relaxed);
	}
}

size_t VulkanPipelineLibraryCache::Count(VulkanPipelineLibraryPart part) const
{
	return m_count[part].load(std::memory_order_relaxed);
}
