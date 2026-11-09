#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"

#include <bit>

uint64 VulkanPipelineKey_RenderState(const LatteContextRegister& lcr)
{
	const uint32* ctxRegister = lcr.GetRawView();
	uint64 stateHash = 0;

	uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	stateHash += polygonCtrl;
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
	if (!stencilTestEnable)
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
