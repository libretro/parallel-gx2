// The Vulkan pipelines' key (src/Cafe/HW/Latte/Renderer/Vulkan/
// VulkanPipelineKey.cpp), against a replayed sequence of register states:
//  - state the pipelines take as dynamic state - the stencil reference,
//    compare mask and write mask, front and back - makes no new key, so a
//    game stepping its stencil reference compiles one pipeline, not one a
//    value;
//  - every state a pipeline bakes in still makes a key of its own, so
//    nothing is drawn with a pipeline made for other state;
//  - with VK_EXT_extended_dynamic_state the cull mode, front face and depth
//    and stencil tests make no new key either, with _3 the blend enables,
//    equations and write masks, with _2 the polygon offset enable and
//    rasterizer discard, and what the draw sets for them is what a
//    pipeline baked in before.
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
	const uint64 key0 = VulkanPipelineKey_RenderState(*base, 0);

	// the replayed sequence: every stencil reference, with masks and the
	// back face's values stepping along
	auto r = std::make_unique<LatteContextRegister>(*base);
	std::set<uint64> keys;
	for (uint32 i = 0; i < 1024; i++)
	{
		SetReg(*r, REGADDR::DB_STENCILREFMASK, (i & 0xFF) | ((i * 7 & 0xFF) << 8) | ((i * 13 & 0xFF) << 16));
		SetReg(*r, REGADDR::DB_STENCILREFMASK_BF, ((i * 3) & 0xFF) | ((i * 5 & 0xFF) << 8) | ((i * 11 & 0xFF) << 16));
		keys.insert(VulkanPipelineKey_RenderState(*r, 0));
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
		const uint64 k = VulkanPipelineKey_RenderState(*s, 0);
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
		CHECK(VulkanPipelineKey_RenderState(*a, 0) != VulkanPipelineKey_RenderState(*b, 0));
	}
	// with stencil off, its fields are no part of the key
	{
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::DB_DEPTH_CONTROL, depthControl & ~1u);
		auto b = std::make_unique<LatteContextRegister>(*a);
		SetReg(*b, REGADDR::DB_DEPTH_CONTROL, (depthControl & ~1u) ^ (1 << 8));
		CHECK(VulkanPipelineKey_RenderState(*a, 0) == VulkanPipelineKey_RenderState(*b, 0));
	}
	// extended dynamic state: cull, front face, depth and stencil are the
	// draw's, everything else still keyed
	{
		std::set<uint64> dyn;
		const uint64 dkey = VulkanPipelineKey_RenderState(*base, VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL);
		size_t keyed = 0;
		for (const Change& c : baked)
		{
			auto s2 = std::make_unique<LatteContextRegister>(*base);
			SetReg(*s2, c.reg, c.value);
			const uint64 k = VulkanPipelineKey_RenderState(*s2, VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL);
			const bool dynamic = c.reg == REGADDR::DB_DEPTH_CONTROL ||
				(c.reg == REGADDR::PA_SU_SC_MODE_CNTL && c.value < 8);
			if (dynamic && k != dkey)
			{
				std::printf("FAIL: %s makes a new key with extended dynamic state\n", c.what);
				s_failures++;
			}
			if (!dynamic)
			{
				keyed++;
				dyn.insert(k);
				if (k == dkey)
				{
					std::printf("FAIL: %s does not change the key with extended dynamic state\n", c.what);
					s_failures++;
				}
			}
		}
		CHECK(dyn.size() == keyed);
		// a replayed sequence of depth, stencil and cull states: one key
		std::set<uint64> seq;
		auto r2 = std::make_unique<LatteContextRegister>(*base);
		for (uint32 i = 0; i < 4096; i++)
		{
			SetReg(*r2, REGADDR::DB_DEPTH_CONTROL, i * 0x9E3779B1u);
			SetReg(*r2, REGADDR::PA_SU_SC_MODE_CNTL, i & 7);
			seq.insert(VulkanPipelineKey_RenderState(*r2, VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL));
		}
		std::printf("pipeline keys for 4096 depth, stencil and cull states with extended dynamic state: %zu\n", seq.size());
		CHECK(seq.size() == 1);
	}
	// extended dynamic state 3: blend enables, equations and write masks are
	// the draw's, everything else (depth too, without the first) still keyed
	{
		const uint32 flags = VULKAN_PIPELINE_KEY_DYNAMIC_BLEND;
		const uint64 bkey = VulkanPipelineKey_RenderState(*base, flags);
		std::set<uint64> others;
		size_t keyed = 0;
		for (const Change& c : baked)
		{
			auto s2 = std::make_unique<LatteContextRegister>(*base);
			SetReg(*s2, c.reg, c.value);
			const uint64 k = VulkanPipelineKey_RenderState(*s2, flags);
			const bool dynamic = c.reg == REGADDR::CB_BLEND0_CONTROL || c.reg == REGADDR::CB_TARGET_MASK ||
				(c.reg == REGADDR::CB_COLOR_CONTROL && ((c.value ^ (0xCC0000 | (1 << 8))) & ~0xFF00u) == 0);
			if (dynamic && k != bkey)
			{
				std::printf("FAIL: %s makes a new key with dynamic blending\n", c.what);
				s_failures++;
			}
			if (!dynamic)
			{
				keyed++;
				others.insert(k);
				if (k == bkey)
				{
					std::printf("FAIL: %s does not change the key with dynamic blending\n", c.what);
					s_failures++;
				}
			}
		}
		CHECK(others.size() == keyed);
		// the logic op stays in the pipeline
		auto rop = std::make_unique<LatteContextRegister>(*base);
		SetReg(*rop, REGADDR::CB_COLOR_CONTROL, 0xEE0000 | (1 << 8));
		CHECK(VulkanPipelineKey_RenderState(*rop, flags) != bkey);
		std::set<uint64> seq;
		auto r2 = std::make_unique<LatteContextRegister>(*base);
		for (uint32 i = 0; i < 4096; i++)
		{
			SetReg(*r2, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | ((i & 0xFF) << 8));
			SetReg(*r2, REGADDR::CB_TARGET_MASK, i * 0x9E3779B1u);
			for (int t = 0; t < 8; t++)
				SetReg(*r2, REGADDR::CB_BLEND0_CONTROL + t, (i * 0x85EBCA6Bu) ^ (t * 0x1010101u));
			seq.insert(VulkanPipelineKey_RenderState(*r2, flags));
		}
		std::printf("pipeline keys for 4096 blend states with dynamic blending: %zu\n", seq.size());
		CHECK(seq.size() == 1);
	}
	// extended dynamic state 2: polygon offset enable and rasterizer discard
	// are the draw's, everything else still keyed
	{
		const uint32 flags = VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER;
		const uint64 rkey = VulkanPipelineKey_RenderState(*base, flags);
		std::set<uint64> others;
		size_t keyed = 0;
		for (const Change& c : baked)
		{
			auto s2 = std::make_unique<LatteContextRegister>(*base);
			SetReg(*s2, c.reg, c.value);
			const uint64 k = VulkanPipelineKey_RenderState(*s2, flags);
			const bool dynamic = (c.reg == REGADDR::PA_SU_SC_MODE_CNTL && c.value == (1u << 11)) ||
				(c.reg == REGADDR::PA_CL_CLIP_CNTL && c.value == (1u << 22));
			if (dynamic && k != rkey)
			{
				std::printf("FAIL: %s makes a new key with dynamic depth bias and discard\n", c.what);
				s_failures++;
			}
			if (!dynamic)
			{
				keyed++;
				others.insert(k);
				if (k == rkey)
				{
					std::printf("FAIL: %s does not change the key with dynamic depth bias and discard\n", c.what);
					s_failures++;
				}
			}
		}
		CHECK(others.size() == keyed);
		// the rest of the clip control (the depth clip) stays in the pipeline
		auto clip = std::make_unique<LatteContextRegister>(*base);
		SetReg(*clip, REGADDR::PA_CL_CLIP_CNTL, (1u << 22) | (1u << 27));
		CHECK(VulkanPipelineKey_RenderState(*clip, flags) != rkey);
		std::set<uint64> seq;
		auto r2 = std::make_unique<LatteContextRegister>(*base);
		for (uint32 i = 0; i < 64; i++)
		{
			SetReg(*r2, REGADDR::PA_SU_SC_MODE_CNTL, (i & 1) << 11);
			SetReg(*r2, REGADDR::PA_CL_CLIP_CNTL, ((i >> 1) & 1) << 22);
			SetReg(*r2, REGADDR::PA_CL_VTE_CNTL, ((i >> 2) & 1) << 1);
			seq.insert(VulkanPipelineKey_RenderState(*r2, flags));
		}
		std::printf("pipeline keys for 64 depth bias and discard states with extended dynamic state 2: %zu\n", seq.size());
		CHECK(seq.size() == 1);
		// all three together: one key for every state the draw sets
		const uint32 all = VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND | flags;
		std::set<uint64> allKeys;
		for (uint32 i = 0; i < 1024; i++)
		{
			SetReg(*r2, REGADDR::PA_SU_SC_MODE_CNTL, (i & 7) | (((i >> 3) & 1) << 11));
			SetReg(*r2, REGADDR::PA_CL_CLIP_CNTL, ((i >> 4) & 1) << 22);
			SetReg(*r2, REGADDR::DB_DEPTH_CONTROL, i * 0x9E3779B1u);
			SetReg(*r2, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | ((i * 37 & 0xFF) << 8));
			SetReg(*r2, REGADDR::CB_TARGET_MASK, i * 0x85EBCA6Bu);
			allKeys.insert(VulkanPipelineKey_RenderState(*r2, all));
		}
		std::printf("pipeline keys for 1024 states with all of it dynamic: %zu\n", allKeys.size());
		CHECK(allKeys.size() == 1);
	}
	// the enables the draw sets and the pipeline bakes in
	{
		auto s2 = std::make_unique<LatteContextRegister>(*base);
		VulkanRasterizerEnables e = VulkanPipelineKey_Rasterizer(*s2);
		CHECK(!e.depthBiasEnable && !e.rasterizerDiscardEnable);
		SetReg(*s2, REGADDR::PA_SU_SC_MODE_CNTL, 1u << 11);
		SetReg(*s2, REGADDR::PA_CL_CLIP_CNTL, 1u << 22);
		SetReg(*s2, REGADDR::PA_CL_VTE_CNTL, 1u << 1);
		e = VulkanPipelineKey_Rasterizer(*s2);
		CHECK(e.depthBiasEnable && e.rasterizerDiscardEnable);
		// GX2SetSpecialState(0, true): the viewport's X offset off keeps rasterizing
		SetReg(*s2, REGADDR::PA_CL_VTE_CNTL, 0);
		e = VulkanPipelineKey_Rasterizer(*s2);
		CHECK(e.depthBiasEnable && !e.rasterizerDiscardEnable);
		// the key without the extension is as it was: discard only with the
		// viewport's X offset on, polygon offset by its enable
		auto a = std::make_unique<LatteContextRegister>(*base);
		SetReg(*a, REGADDR::PA_SU_SC_MODE_CNTL, 1u << 11);
		CHECK(VulkanPipelineKey_RenderState(*a, 0) != key0);
	}
	// the blend state the draw sets and the pipeline bakes in
	{
		auto s2 = std::make_unique<LatteContextRegister>(*base);
		VkFormat formats[8] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM};
		// blending on 0 and 1; 1 is an integer format
		SetReg(*s2, REGADDR::CB_COLOR_CONTROL, 0xCC0000 | (3 << 8));
		// 0: src alpha, one minus src alpha, add
		SetReg(*s2, REGADDR::CB_BLEND0_CONTROL, 4 | (0 << 5) | (5 << 8));
		// 1: separate alpha, a constant color factor
		SetReg(*s2, REGADDR::CB_BLEND0_CONTROL + 1, 1 | (0xD << 8) | (1u << 29) | (1 << 16) | (0 << 24));
		SetReg(*s2, REGADDR::CB_TARGET_MASK, 0x7F);
		VkPipelineColorBlendAttachmentState att[8];
		const bool constants = VulkanPipelineKey_Blend(*s2, formats, att);
		CHECK(att[0].blendEnable == VK_TRUE && att[1].blendEnable == VK_FALSE && att[2].blendEnable == VK_FALSE);
		CHECK(att[0].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA && att[0].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
		CHECK(att[0].colorBlendOp == VK_BLEND_OP_ADD && att[0].srcAlphaBlendFactor == att[0].srcColorBlendFactor);
		CHECK(att[0].colorWriteMask == 0xF && att[1].colorWriteMask == 0x7 && att[2].colorWriteMask == 0);
		CHECK(att[1].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR && att[1].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
		CHECK(constants);
		SetReg(*s2, REGADDR::CB_BLEND0_CONTROL + 1, 0);
		CHECK(!VulkanPipelineKey_Blend(*s2, formats, att));
	}
	// what the draw sets is what the pipeline bakes in
	{
		auto s2 = std::make_unique<LatteContextRegister>(*base);
		VulkanDepthStencilCull d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.depthTestEnable && d.depthWriteEnable && d.depthCompareOp == VK_COMPARE_OP_LESS_OR_EQUAL);
		CHECK(d.stencilTestEnable && d.frontCompareOp == VK_COMPARE_OP_EQUAL && d.backCompareOp == VK_COMPARE_OP_EQUAL);
		CHECK(d.cullMode == VK_CULL_MODE_NONE && d.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
		// one-sided stencil: the back face as the front
		SetReg(*s2, REGADDR::DB_DEPTH_CONTROL, (depthControl & ~(1u << 7)) | (5u << 14));
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.backCompareOp == d.frontCompareOp && d.backPassOp == d.frontPassOp && d.frontPassOp == VK_STENCIL_OP_INVERT);
		// cull back, clockwise
		SetReg(*s2, REGADDR::PA_SU_SC_MODE_CNTL, 2 | 4);
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_BACK_BIT && d.frontFace == VK_FRONT_FACE_CLOCKWISE);
		// rects: culled as one winding, either bit culls both faces
		SetReg(*s2, REGADDR::VGT_PRIMITIVE_TYPE, (uint32)LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS);
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_FRONT_AND_BACK);
		SetReg(*s2, REGADDR::PA_SU_SC_MODE_CNTL, 1); // cull front, counter-clockwise
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_FRONT_AND_BACK);
		SetReg(*s2, REGADDR::VGT_PRIMITIVE_TYPE, 4); // triangles: the front alone
		d = VulkanPipelineKey_DepthStencilCull(*s2);
		CHECK(d.cullMode == VK_CULL_MODE_FRONT_BIT);
	}
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("pipeline_key: all passed\n");
	return 0;
}
