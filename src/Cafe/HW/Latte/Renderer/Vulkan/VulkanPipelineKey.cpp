#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"

#include <bit>

uint64 VulkanPipelineKey_RenderState(const LatteContextRegister& lcr, bool dynamicDepthStencilCull)
{
	const uint32* ctxRegister = lcr.GetRawView();
	uint64 stateHash = 0;

	uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	// CULL_FRONT, CULL_BACK, FRONT_FACE: dynamic state with the extension
	stateHash += dynamicDepthStencilCull ? (polygonCtrl & ~7u) : polygonCtrl;
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += ctxRegister[Latte::REGADDR::PA_CL_CLIP_CNTL];
	stateHash = std::rotl<uint64>(stateHash, 7);
	// the pipeline drops rasterizer discard when the viewport's X offset is
	// off (GX2SetSpecialState(0, true), PipelineCompiler::InitRasterizerState)
	if (lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() && !lcr.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		stateHash += 0x2222;

	const auto colorControlReg = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
	stateHash += colorControlReg;

	stateHash += ctxRegister[Latte::REGADDR::CB_TARGET_MASK];

	const uint32 blendEnableMask = (colorControlReg >> 8) & 0xFF;
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

	// polygon offset
	if (polygonCtrl & (1 << 11))
	{
		// front offset enabled
		stateHash += 0x1111;
	}

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
