#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

#include <bit>

uint64 VulkanPipelineKey_RenderState(const LatteContextRegister& lcr, uint32 dynamic)
{
	const bool dynamicDepthStencilCull = (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL) != 0;
	const bool dynamicBlend = (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_BLEND) != 0;
	const bool dynamicRasterizer = (dynamic & VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER) != 0;
	const uint32* ctxRegister = lcr.GetRawView();
	uint64 stateHash = 0;

	uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	uint32 polygonCtrlKeyed = polygonCtrl;
	// CULL_FRONT, CULL_BACK, FRONT_FACE: dynamic state with the extension
	if (dynamicDepthStencilCull)
		polygonCtrlKeyed &= ~7u;
	// OFFSET_FRONT_ENABLED: dynamic state with _2
	if (dynamicRasterizer)
		polygonCtrlKeyed &= ~(1u << 11);
	stateHash += polygonCtrlKeyed;
	stateHash = std::rotl<uint64>(stateHash, 7);

	uint32 clipCtrl = ctxRegister[Latte::REGADDR::PA_CL_CLIP_CNTL];
	// DX_RASTERIZATION_KILL: dynamic state with _2
	if (dynamicRasterizer)
		clipCtrl &= ~(1u << 22);
	stateHash += clipCtrl;
	stateHash = std::rotl<uint64>(stateHash, 7);
	const VulkanRasterizerEnables rasterizer = VulkanPipelineKey_Rasterizer(lcr);
	// the pipeline drops rasterizer discard when the viewport's X offset is
	// off (GX2SetSpecialState(0, true), VulkanPipelineKey_Rasterizer)
	if (!dynamicRasterizer && lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() && !rasterizer.rasterizerDiscardEnable)
		stateHash += 0x2222;

	const auto colorControlReg = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
	// BLEND_MASK (8-15), the write masks and the blend controls are dynamic
	// state with the extension; the logic op and the rest are not
	stateHash += dynamicBlend ? (colorControlReg & ~0xFF00u) : colorControlReg;

	if (!dynamicBlend)
		stateHash += ctxRegister[Latte::REGADDR::CB_TARGET_MASK];

	const uint32 blendEnableMask = dynamicBlend ? 0 : (colorControlReg >> 8) & 0xFF;
	if (blendEnableMask)
	{
		for (auto i = 0; i < 8; ++i)
		{
			if (((blendEnableMask & (1 << i))) == 0)
				continue;
			stateHash = std::rotl<uint64>(stateHash, 7);
			stateHash += ctxRegister[Latte::REGADDR::CB_BLEND0_CONTROL + i];
		}
	}

	// The stencil reference, compare mask and write mask are dynamic state
	// (VulkanRenderer::draw_updateStencilState), so DB_STENCILREFMASK and
	// DB_STENCILREFMASK_BF are not part of the key
	uint32 depthControl = ctxRegister[Latte::REGADDR::DB_DEPTH_CONTROL];
	bool stencilTestEnable = depthControl & 1;
	// all of it - depth and stencil tests, functions and operations - is
	// dynamic state with the extension
	if (dynamicDepthStencilCull)
		depthControl = 0;
	else if (!stencilTestEnable)
	{
		// zero out stencil related bits (8-31)
		depthControl &= 0xFF;
	}

	stateHash = std::rotl<uint64>(stateHash, 17);
	stateHash += depthControl;

	// polygon offset: front offset enabled
	if (!dynamicRasterizer && rasterizer.depthBiasEnable)
		stateHash += 0x1111;

	return stateHash;
}

VulkanDepthStencilCull VulkanPipelineKey_DepthStencilCull(const LatteContextRegister& lcr)
{
	static const VkCompareOp compareTable[8] = {
		VK_COMPARE_OP_NEVER,
		VK_COMPARE_OP_LESS,
		VK_COMPARE_OP_EQUAL,
		VK_COMPARE_OP_LESS_OR_EQUAL,
		VK_COMPARE_OP_GREATER,
		VK_COMPARE_OP_NOT_EQUAL,
		VK_COMPARE_OP_GREATER_OR_EQUAL,
		VK_COMPARE_OP_ALWAYS
	};
	static const VkStencilOp stencilOpTable[8] = {
		VK_STENCIL_OP_KEEP,
		VK_STENCIL_OP_ZERO,
		VK_STENCIL_OP_REPLACE,
		VK_STENCIL_OP_INCREMENT_AND_CLAMP,
		VK_STENCIL_OP_DECREMENT_AND_CLAMP,
		VK_STENCIL_OP_INVERT,
		VK_STENCIL_OP_INCREMENT_AND_WRAP,
		VK_STENCIL_OP_DECREMENT_AND_WRAP
	};
	VulkanDepthStencilCull s{};

	// polygon control
	const auto& polygonControlReg = lcr.PA_SU_SC_MODE_CNTL;
	const auto frontFace = polygonControlReg.get_FRONT_FACE();
	uint32 cullFront = polygonControlReg.get_CULL_FRONT();
	uint32 cullBack = polygonControlReg.get_CULL_BACK();
	// todo - how does culling behave with rects?
	// right now we just assume that their winding is always CW
	if (lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE() == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
	{
		if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			cullFront = cullBack;
		else
			cullBack = cullFront;
	}
	if (cullFront && cullBack)
		s.cullMode = VK_CULL_MODE_FRONT_AND_BACK;
	else if (cullFront)
		s.cullMode = VK_CULL_MODE_FRONT_BIT;
	else if (cullBack)
		s.cullMode = VK_CULL_MODE_BACK_BIT;
	else
		s.cullMode = VK_CULL_MODE_NONE;
	s.frontFace = frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;

	// depth
	const auto& depthControl = lcr.DB_DEPTH_CONTROL;
	s.depthTestEnable = depthControl.get_Z_ENABLE() ? VK_TRUE : VK_FALSE;
	s.depthWriteEnable = depthControl.get_Z_WRITE_ENABLE() ? VK_TRUE : VK_FALSE;
	s.depthCompareOp = compareTable[(size_t)depthControl.get_Z_FUNC()];

	// stencil: the back face as the front without two-sided stencil
	s.stencilTestEnable = depthControl.get_STENCIL_ENABLE() ? VK_TRUE : VK_FALSE;
	s.frontCompareOp = compareTable[(size_t)depthControl.get_STENCIL_FUNC_F()];
	s.frontPassOp = stencilOpTable[(size_t)depthControl.get_STENCIL_ZPASS_F()];
	s.frontDepthFailOp = stencilOpTable[(size_t)depthControl.get_STENCIL_ZFAIL_F()];
	s.frontFailOp = stencilOpTable[(size_t)depthControl.get_STENCIL_FAIL_F()];
	if (depthControl.get_BACK_STENCIL_ENABLE())
	{
		s.backCompareOp = compareTable[(size_t)depthControl.get_STENCIL_FUNC_B()];
		s.backPassOp = stencilOpTable[(size_t)depthControl.get_STENCIL_ZPASS_B()];
		s.backDepthFailOp = stencilOpTable[(size_t)depthControl.get_STENCIL_ZFAIL_B()];
		s.backFailOp = stencilOpTable[(size_t)depthControl.get_STENCIL_FAIL_B()];
	}
	else
	{
		s.backCompareOp = s.frontCompareOp;
		s.backPassOp = s.frontPassOp;
		s.backDepthFailOp = s.frontDepthFailOp;
		s.backFailOp = s.frontFailOp;
	}
	return s;
}

VulkanRasterizerEnables VulkanPipelineKey_Rasterizer(const LatteContextRegister& lcr)
{
	VulkanRasterizerEnables s{};
	s.depthBiasEnable = lcr.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED() ? VK_TRUE : VK_FALSE;
	s.rasterizerDiscardEnable = lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() ? VK_TRUE : VK_FALSE;
	// GX2SetSpecialState(0, true) sets DX_RASTERIZATION_KILL with the
	// viewport's X offset off and still expects the depth writes
	if (!lcr.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		s.rasterizerDiscardEnable = VK_FALSE;
	return s;
}

static VkBlendOp BlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC combineFunc)
{
	switch (combineFunc)
	{
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::DST_PLUS_SRC:
		return VK_BLEND_OP_ADD;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::SRC_MINUS_DST:
		return VK_BLEND_OP_SUBTRACT;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::MIN_DST_SRC:
		return VK_BLEND_OP_MIN;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::MAX_DST_SRC:
		return VK_BLEND_OP_MAX;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::DST_MINUS_SRC:
		return VK_BLEND_OP_REVERSE_SUBTRACT;
	default:
		cemu_assert_suspicious();
		return VK_BLEND_OP_ADD;
	}
}

static VkBlendFactor BlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
{
	static const VkBlendFactor factors[] =
	{
		/* 0x00 */ VK_BLEND_FACTOR_ZERO,
		/* 0x01 */ VK_BLEND_FACTOR_ONE,
		/* 0x02 */ VK_BLEND_FACTOR_SRC_COLOR,
		/* 0x03 */ VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
		/* 0x04 */ VK_BLEND_FACTOR_SRC_ALPHA,
		/* 0x05 */ VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		/* 0x06 */ VK_BLEND_FACTOR_DST_ALPHA,
		/* 0x07 */ VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
		/* 0x08 */ VK_BLEND_FACTOR_DST_COLOR,
		/* 0x09 */ VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
		/* 0x0A */ VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
		/* 0x0B */ VK_BLEND_FACTOR_MAX_ENUM, // todo
		/* 0x0C */ VK_BLEND_FACTOR_MAX_ENUM, // todo
		/* 0x0D */ VK_BLEND_FACTOR_CONSTANT_COLOR,
		/* 0x0E */ VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
		/* 0x0F */ VK_BLEND_FACTOR_SRC1_COLOR,
		/* 0x10 */ VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
		/* 0x11 */ VK_BLEND_FACTOR_SRC1_ALPHA,
		/* 0x12 */ VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
		/* 0x13 */ VK_BLEND_FACTOR_CONSTANT_ALPHA,
		/* 0x14 */ VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
	};
	cemu_assert_debug((uint32)factor < std::size(factors));
	return (uint32)factor < std::size(factors) ? factors[(uint32)factor] : VK_BLEND_FACTOR_MAX_ENUM;
}

static bool ConsumesBlendConstants(VkBlendFactor f)
{
	return f == VK_BLEND_FACTOR_CONSTANT_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR ||
		f == VK_BLEND_FACTOR_CONSTANT_ALPHA || f == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
}

static bool IsIntegerFormat(VkFormat fmt)
{
	return
		// 8bit integer formats
		fmt == VK_FORMAT_R8_UINT || fmt == VK_FORMAT_R8_SINT ||
		fmt == VK_FORMAT_R8G8_UINT || fmt == VK_FORMAT_R8G8_SINT ||
		fmt == VK_FORMAT_R8G8B8_UINT || fmt == VK_FORMAT_R8G8B8_SINT ||
		fmt == VK_FORMAT_R8G8B8A8_UINT || fmt == VK_FORMAT_R8G8B8A8_SINT ||
		fmt == VK_FORMAT_B8G8R8A8_UINT || fmt == VK_FORMAT_B8G8R8A8_SINT ||
		// 16bit integer formats
		fmt == VK_FORMAT_R16_UINT || fmt == VK_FORMAT_R16_SINT ||
		fmt == VK_FORMAT_R16G16_UINT || fmt == VK_FORMAT_R16G16_SINT ||
		fmt == VK_FORMAT_R16G16B16_UINT || fmt == VK_FORMAT_R16G16B16_SINT ||
		fmt == VK_FORMAT_R16G16B16A16_UINT || fmt == VK_FORMAT_R16G16B16A16_SINT ||
		// 32bit integer formats
		fmt == VK_FORMAT_R32_UINT || fmt == VK_FORMAT_R32_SINT ||
		fmt == VK_FORMAT_R32G32_UINT || fmt == VK_FORMAT_R32G32_SINT ||
		fmt == VK_FORMAT_R32G32B32_UINT || fmt == VK_FORMAT_R32G32B32_SINT ||
		fmt == VK_FORMAT_R32G32B32A32_UINT || fmt == VK_FORMAT_R32G32B32A32_SINT;
}

bool VulkanPipelineKey_Blend(const LatteContextRegister& lcr, const VkFormat colorFormats[8],
	VkPipelineColorBlendAttachmentState out[8])
{
	const uint32 blendEnableMask = lcr.CB_COLOR_CONTROL.get_BLEND_MASK();
	const uint32 renderTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	bool usesBlendConstants = false;
	for (size_t i = 0; i < 8; i++)
	{
		auto& entry = out[i];
		entry.blendEnable = (blendEnableMask & (1 << i)) != 0 ? VK_TRUE : VK_FALSE;
		// force-disable blending for integer formats
		if (entry.blendEnable != VK_FALSE && IsIntegerFormat(colorFormats[i]))
			entry.blendEnable = VK_FALSE;

		const auto& blendControlReg = lcr.CB_BLENDN_CONTROL[i];
		entry.colorWriteMask = (renderTargetMask >> (i * 4)) & 0xF;
		entry.colorBlendOp = BlendOp(blendControlReg.get_COLOR_COMB_FCN());
		entry.srcColorBlendFactor = BlendFactor(blendControlReg.get_COLOR_SRCBLEND());
		entry.dstColorBlendFactor = BlendFactor(blendControlReg.get_COLOR_DSTBLEND());
		if (blendControlReg.get_SEPARATE_ALPHA_BLEND())
		{
			entry.alphaBlendOp = BlendOp(blendControlReg.get_ALPHA_COMB_FCN());
			entry.srcAlphaBlendFactor = BlendFactor(blendControlReg.get_ALPHA_SRCBLEND());
			entry.dstAlphaBlendFactor = BlendFactor(blendControlReg.get_ALPHA_DSTBLEND());
		}
		else
		{
			entry.alphaBlendOp = entry.colorBlendOp;
			entry.srcAlphaBlendFactor = entry.srcColorBlendFactor;
			entry.dstAlphaBlendFactor = entry.dstColorBlendFactor;
		}
		usesBlendConstants |= ConsumesBlendConstants(entry.srcColorBlendFactor) ||
			ConsumesBlendConstants(entry.dstColorBlendFactor) ||
			ConsumesBlendConstants(entry.srcAlphaBlendFactor) ||
			ConsumesBlendConstants(entry.dstAlphaBlendFactor);
	}
	return usesBlendConstants;
}

void VulkanPipelineKey_SetDepthStencilCull(VkCommandBuffer cb, const VulkanDepthStencilCull& s)
{
	vkCmdSetCullModeEXT(cb, s.cullMode);
	vkCmdSetFrontFaceEXT(cb, s.frontFace);
	vkCmdSetDepthTestEnableEXT(cb, s.depthTestEnable);
	vkCmdSetDepthWriteEnableEXT(cb, s.depthWriteEnable);
	vkCmdSetDepthCompareOpEXT(cb, s.depthCompareOp);
	vkCmdSetStencilTestEnableEXT(cb, s.stencilTestEnable);
	vkCmdSetStencilOpEXT(cb, VK_STENCIL_FACE_FRONT_BIT, s.frontFailOp, s.frontPassOp, s.frontDepthFailOp, s.frontCompareOp);
	vkCmdSetStencilOpEXT(cb, VK_STENCIL_FACE_BACK_BIT, s.backFailOp, s.backPassOp, s.backDepthFailOp, s.backCompareOp);
}

void VulkanPipelineKey_SetBlend(VkCommandBuffer cb, const VkPipelineColorBlendAttachmentState* att, uint32 count)
{
	VkBool32 enables[8];
	VkColorBlendEquationEXT equations[8];
	VkColorComponentFlags masks[8];
	if (count > 8)
		count = 8;
	for (uint32 i = 0; i < count; i++)
	{
		enables[i] = att[i].blendEnable;
		equations[i].srcColorBlendFactor = att[i].srcColorBlendFactor;
		equations[i].dstColorBlendFactor = att[i].dstColorBlendFactor;
		equations[i].colorBlendOp = att[i].colorBlendOp;
		equations[i].srcAlphaBlendFactor = att[i].srcAlphaBlendFactor;
		equations[i].dstAlphaBlendFactor = att[i].dstAlphaBlendFactor;
		equations[i].alphaBlendOp = att[i].alphaBlendOp;
		masks[i] = att[i].colorWriteMask;
	}
	vkCmdSetColorBlendEnableEXT(cb, 0, count, enables);
	vkCmdSetColorBlendEquationEXT(cb, 0, count, equations);
	vkCmdSetColorWriteMaskEXT(cb, 0, count, masks);
}

void VulkanPipelineKey_SetRasterizer(VkCommandBuffer cb, const VulkanRasterizerEnables& s)
{
	vkCmdSetDepthBiasEnableEXT(cb, s.depthBiasEnable);
	vkCmdSetRasterizerDiscardEnableEXT(cb, s.rasterizerDiscardEnable);
}
