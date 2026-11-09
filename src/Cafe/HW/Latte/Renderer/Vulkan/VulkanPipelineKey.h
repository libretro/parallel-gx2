#pragma once

#include "Cafe/HW/Latte/ISA/LatteReg.h"

// What a pipeline bakes in of the register state - rasterizer, clipping,
// blending, depth and stencil - hashed for its key. State the pipelines
// take as dynamic state is left out: one pipeline serves every value of it.
uint64 VulkanPipelineKey_RenderState(const LatteContextRegister& lcr);
