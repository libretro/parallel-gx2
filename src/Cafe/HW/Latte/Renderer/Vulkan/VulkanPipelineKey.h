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
