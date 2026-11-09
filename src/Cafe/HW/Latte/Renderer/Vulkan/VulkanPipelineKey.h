#pragma once

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include <vulkan/vulkan.h>

// What a pipeline bakes in of the register state - rasterizer, clipping,
// blending, depth and stencil - hashed for its key. State the pipelines
// take as dynamic state is left out: one pipeline serves every value of it.
// dynamic: what the draw sets instead (VULKAN_PIPELINE_KEY_DYNAMIC_*).
enum
{
	// VK_EXT_extended_dynamic_state: cull mode, front face and the depth and
	// stencil tests (VulkanRenderer::draw_updateDepthStencilCull)
	VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL = 1 << 0,
	// VK_EXT_extended_dynamic_state3: blend enables, blend equations and
	// color write masks (VulkanRenderer::draw_updateBlend)
	VULKAN_PIPELINE_KEY_DYNAMIC_BLEND = 1 << 1,
	// VK_EXT_extended_dynamic_state2: depth bias (polygon offset) enable and
	// rasterizer discard (VulkanRenderer::draw_updateRasterizer)
	VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER = 1 << 2,
};
uint64 VulkanPipelineKey_RenderState(const LatteContextRegister& lcr, uint32 dynamic);

// The cull mode, front face and depth and stencil tests of a register state,
// as the pipeline bakes them in or the draw sets them. The stencil
// reference and masks are not in it (always dynamic, draw_updateStencilState).
struct VulkanDepthStencilCull
{
	VkCullModeFlags cullMode;
	VkFrontFace frontFace;
	VkBool32 depthTestEnable;
	VkBool32 depthWriteEnable;
	VkCompareOp depthCompareOp;
	VkBool32 stencilTestEnable;
	VkStencilOp frontFailOp, frontPassOp, frontDepthFailOp;
	VkCompareOp frontCompareOp;
	VkStencilOp backFailOp, backPassOp, backDepthFailOp;
	VkCompareOp backCompareOp;

	bool operator==(const VulkanDepthStencilCull&) const = default;
};
VulkanDepthStencilCull VulkanPipelineKey_DepthStencilCull(const LatteContextRegister& lcr);

// The blend state of a register state for the color attachments of a render
// pass of colorFormats, as the pipeline bakes it in or the draw sets it.
// Returns whether a blend factor reads the blend constants.
bool VulkanPipelineKey_Blend(const LatteContextRegister& lcr, const VkFormat colorFormats[8],
	VkPipelineColorBlendAttachmentState out[8]);

// Whether polygon offset applies and whether nothing is rasterized, as the
// pipeline bakes them in or the draw sets them. The depth bias values are
// always dynamic (VulkanRenderer::draw_updateDepthBias).
struct VulkanRasterizerEnables
{
	VkBool32 depthBiasEnable;
	VkBool32 rasterizerDiscardEnable;

	bool operator==(const VulkanRasterizerEnables&) const = default;
};
VulkanRasterizerEnables VulkanPipelineKey_Rasterizer(const LatteContextRegister& lcr);

// Sets them on a command buffer, as dynamic state: what the draw does with
// VK_EXT_extended_dynamic_state (the first) and _3's blend states (the
// second, for count color attachments from the first)
void VulkanPipelineKey_SetDepthStencilCull(VkCommandBuffer cb, const VulkanDepthStencilCull& s);
void VulkanPipelineKey_SetBlend(VkCommandBuffer cb, const VkPipelineColorBlendAttachmentState* att, uint32 count);
// and _2's (VulkanRenderer::draw_updateRasterizer)
void VulkanPipelineKey_SetRasterizer(VkCommandBuffer cb, const VulkanRasterizerEnables& s);
