#pragma once

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include <vulkan/vulkan.h>

// What a pipeline bakes in of the register state - rasterizer, clipping,
// blending, depth and stencil - hashed for its key. State the pipelines
// take as dynamic state is left out: one pipeline serves every value of it.
// dynamicDepthStencilCull: the device has VK_EXT_extended_dynamic_state,
// and the cull mode, front face and depth and stencil tests are set by the
// draw (VulkanRenderer::draw_updateDepthStencilCull).
uint64 VulkanPipelineKey_RenderState(const LatteContextRegister& lcr, bool dynamicDepthStencilCull);

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
