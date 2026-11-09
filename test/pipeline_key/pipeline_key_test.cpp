// The Vulkan pipelines' key (src/Cafe/HW/Latte/Renderer/Vulkan/
// VulkanPipelineKey.cpp), against a replayed sequence of register states:
//  - state the pipelines take as dynamic state - the stencil reference,
//    compare mask and write mask, front and back - makes no new key, so a
//    game stepping its stencil reference compiles one pipeline, not one a
//    value;
//  - every state a pipeline bakes in still makes a key of its own, so
//    nothing is drawn with a pipeline made for other state.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"

#include <cstdio>
#include <memory>
#include <set>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

using namespace Latte;

static void SetReg(LatteContextRegister& r, uint32 addr, uint32 v)
{
	r.GetRawView()[addr] = v;
}

int main()
{
	auto base = std::make_unique<LatteContextRegister>();
	std::memset(base.get(), 0, sizeof(LatteContextRegister));
	// depth on, less-equal, written; stencil on, two-sided
	const uint32 depthControl = 1 | (1 << 1) | (1 << 2) | (3 << 4) | (1 << 7) | (2 << 8) | (2 << 20);
	SetReg(*base, REGADDR::DB_DEPTH_CONTROL, depthControl);
	SetReg(*base, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (1 << 8));
	SetReg(*base, REGADDR::CB_BLEND0_CONTROL, 0x00010001);
	SetReg(*base, REGADDR::CB_TARGET_MASK, 0xF);
	const uint64 key0 = VulkanPipelineKey_RenderState(*base);

	// the replayed sequence: every stencil reference, with masks and the
	// back face's values stepping along
	auto r = std::make_unique<LatteContextRegister>(*base);
	std::set<uint64> keys;
	for (uint32 i = 0; i < 1024; i++)
	{
		SetReg(*r, REGADDR::DB_STENCILREFMASK, (i & 0xFF) | ((i * 7 & 0xFF) << 8) | ((i * 13 & 0xFF) << 16));
		SetReg(*r, REGADDR::DB_STENCILREFMASK_BF, ((i * 3) & 0xFF) | ((i * 5 & 0xFF) << 8) | ((i * 11 & 0xFF) << 16));
		keys.insert(VulkanPipelineKey_RenderState(*r));
	}
	std::printf("pipeline keys for 1024 stencil reference and mask states: %zu\n", keys.size());
	CHECK(keys.size() == 1 && *keys.begin() == key0);

	// what the pipeline bakes in: each change a key of its own
	struct Change { const char* what; uint32 reg; uint32 value; };
	const Change baked[] = {
		{"depth compare", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 4)},
		{"depth test", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 1)},
		{"depth write", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 2)},
		{"stencil test", REGADDR::DB_DEPTH_CONTROL, depthControl ^ 1},
		{"back stencil", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 7)},
		{"stencil compare", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 8)},
		{"stencil pass op", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 14)},
		{"back stencil compare", REGADDR::DB_DEPTH_CONTROL, depthControl ^ (1 << 20)},
		{"cull front", REGADDR::PA_SU_SC_MODE_CNTL, 1},
		{"cull back", REGADDR::PA_SU_SC_MODE_CNTL, 2},
		{"front face", REGADDR::PA_SU_SC_MODE_CNTL, 4},
		{"polygon offset", REGADDR::PA_SU_SC_MODE_CNTL, 1 << 11},
		{"rasterizer discard", REGADDR::PA_CL_CLIP_CNTL, 1 << 22},
		{"color control", REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (1 << 8) | 1},
		{"blend enable", REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (3 << 8)},
		{"blend factors", REGADDR::CB_BLEND0_CONTROL, 0x00020001},
		{"write mask", REGADDR::CB_TARGET_MASK, 0x7},
	};
	std::set<uint64> distinct{key0};
	for (const Change& c : baked)
	{
		auto s = std::make_unique<LatteContextRegister>(*base);
		SetReg(*s, c.reg, c.value);
		const uint64 k = VulkanPipelineKey_RenderState(*s);
		if (k == key0)
		{
			std::printf("FAIL: %s does not change the key\n", c.what);
			s_failures++;
		}
		distinct.insert(k);
	}
	CHECK(distinct.size() == 1 + sizeof(baked) / sizeof(baked[0]));
	// rasterizer discard kept or dropped with the viewport's X offset
	{
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::PA_CL_CLIP_CNTL, 1 << 22);
		auto b = std::make_unique<LatteContextRegister>(*a);
		SetReg(*b, REGADDR::PA_CL_VTE_CNTL, 1 << 1); // VPORT_X_OFFSET_ENA
		CHECK(VulkanPipelineKey_RenderState(*a) != VulkanPipelineKey_RenderState(*b));
	}
	// with stencil off, its fields are no part of the key
	{
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::DB_DEPTH_CONTROL, depthControl & ~1u);
		auto b = std::make_unique<LatteContextRegister>(*a);
		SetReg(*b, REGADDR::DB_DEPTH_CONTROL, (depthControl & ~1u) ^ (1 << 8));
		CHECK(VulkanPipelineKey_RenderState(*a) == VulkanPipelineKey_RenderState(*b));
	}
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("pipeline_key: all passed\n");
	return 0;
}
