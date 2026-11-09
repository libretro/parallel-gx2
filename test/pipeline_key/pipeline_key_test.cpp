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
//    pipeline baked in before;
//  - the part of the key draws that keep their shaders compare: with
//    VK_EXT_vertex_input_dynamic_state the vertex buffer strides and
//    attribute offsets make no new key, with _2 rasterizer discard none
//    either, with dynamic topology a primitive type of the same class none,
//    and the vertex input, topology and restart the draw sets are what a
//    pipeline baked in;
//  - graphics pipeline libraries (VulkanPipelineLibrary.h): each dynamic
//    state the renderer sets goes to the part that takes it, a part's key
//    leaves out exactly what its dynamic states make the draw's, and the
//    lock-free cache of parts gives each key to one maker however many
//    threads race for it.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"

#include <cstdio>
#include <memory>
#include <set>
#include <thread>
#include <vector>

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
	// the part of the key the draws that keep their shaders compare
	{
		LatteParsedFetchShaderAttribute attrs[3]{};
		attrs[0] = {3, 5, Latte::E_HWFMT::HWFMT_32_32_32_FLOAT, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 4};
		attrs[1] = {3, 9, Latte::E_HWFMT::HWFMT_8_8_8_8, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 16};
		attrs[2] = {7, 2, Latte::E_HWFMT::HWFMT_16_16_FLOAT, LatteConst::INSTANCE_DATA, 0, 0, {}, {}, 1, 0};
		LatteFetchShader fs;
		LatteParsedFetchShaderBufferGroup g0{}, g1{};
		g0.attributeBufferIndex = 3;
		g0.attribCount = 2;
		g0.attrib = attrs;
		g1.attributeBufferIndex = 7;
		g1.attribCount = 1;
		g1.attrib = attrs + 2;
		fs.bufferGroups = {g0, g1};
		fs.vkPipelineHashFragment = 0x1234;
		auto stride = [](LatteContextRegister& r, uint32 buffer, uint32 v) {
			SetReg(r, mmSQ_VTX_ATTRIBUTE_BLOCK_START + buffer * 7 + 2, v << 11);
		};
		auto s0 = std::make_unique<LatteContextRegister>(*base);
		stride(*s0, 3, 20);
		stride(*s0, 7, 4);
		SetReg(*s0, mmVGT_PRIMITIVE_TYPE, 4);
		const uint32 vi = VULKAN_PIPELINE_KEY_DYNAMIC_VERTEX_INPUT;
		const uint32 rz = VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER;
		const uint64 m0 = VulkanPipelineKey_Minimal(&fs, *s0, 0);
		const uint64 v0 = VulkanPipelineKey_Minimal(&fs, *s0, vi);
		// strides: keyed, unless the draw sets them
		auto s1 = std::make_unique<LatteContextRegister>(*s0);
		stride(*s1, 3, 24);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s1, 0) != m0);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s1, vi) == v0);
		stride(*s1, 7, 8);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s1, vi) == v0);
		// a fetch shader with other attribute offsets: the same
		LatteFetchShader fs2;
		fs2.bufferGroups = fs.bufferGroups;
		fs2.vkPipelineHashFragment = 0x5678;
		CHECK(VulkanPipelineKey_Minimal(&fs2, *s0, 0) != m0);
		CHECK(VulkanPipelineKey_Minimal(&fs2, *s0, vi) == v0);
		// the primitive type and streamout: keyed
		auto s2 = std::make_unique<LatteContextRegister>(*s0);
		SetReg(*s2, mmVGT_PRIMITIVE_TYPE, 5);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, vi | rz) != VulkanPipelineKey_Minimal(&fs, *s0, vi | rz));
		s2 = std::make_unique<LatteContextRegister>(*s0);
		SetReg(*s2, mmVGT_STRMOUT_EN, 1);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, vi | rz) != VulkanPipelineKey_Minimal(&fs, *s0, vi | rz));
		// rasterizer discard: keyed here too, unless the draw sets it
		s2 = std::make_unique<LatteContextRegister>(*s0);
		SetReg(*s2, REGADDR::PA_CL_CLIP_CNTL, 1u << 22);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, 0) != m0);
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, rz) == VulkanPipelineKey_Minimal(&fs, *s0, rz));
		CHECK(VulkanPipelineKey_Minimal(&fs, *s2, vi | rz) == VulkanPipelineKey_Minimal(&fs, *s0, vi | rz));
		// 256 vertex layouts, one key
		std::set<uint64> layouts;
		for (uint32 i = 0; i < 256; i++)
		{
			stride(*s1, 3, 12 + (i & 15) * 4);
			stride(*s1, 7, (i >> 4) * 2);
			LatteFetchShader f;
			f.bufferGroups = fs.bufferGroups;
			f.vkPipelineHashFragment = i * 0x9E3779B97F4A7C15ull;
			layouts.insert(VulkanPipelineKey_Minimal(&f, *s1, vi));
		}
		std::printf("pipeline keys for 256 vertex layouts with vertex input dynamic state: %zu\n", layouts.size());
		CHECK(layouts.size() == 1);

		// the primitive type: keyed, with dynamic topology by its class
		{
			using E = LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
			const uint32 tp = VULKAN_PIPELINE_KEY_DYNAMIC_TOPOLOGY;
			auto key = [&](E mode, uint32 flags) {
				auto r = std::make_unique<LatteContextRegister>(*s0);
				SetReg(*r, mmVGT_PRIMITIVE_TYPE, (uint32)mode);
				return VulkanPipelineKey_Minimal(&fs, *r, flags);
			};
			const E all[] = {E::POINTS, E::LINES, E::LINE_STRIP, E::TRIANGLES, E::TRIANGLE_FAN, E::TRIANGLE_STRIP,
				E::LINE_STRIP_ADJACENT, E::RECTS, E::LINE_LOOP, E::QUADS, E::QUAD_STRIP};
			std::set<uint64> raw, classes;
			for (E m : all)
			{
				raw.insert(key(m, 0));
				classes.insert(key(m, tp));
			}
			// points, lines, triangles, line strips with adjacency, rects
			std::printf("pipeline keys for %zu primitive types: %zu, with dynamic topology %zu\n", std::size(all), raw.size(), classes.size());
			CHECK(raw.size() == std::size(all));
			CHECK(classes.size() == 5);
			CHECK(key(E::TRIANGLES, tp) == key(E::TRIANGLE_STRIP, tp) && key(E::TRIANGLES, tp) == key(E::TRIANGLE_FAN, tp) &&
				key(E::TRIANGLES, tp) == key(E::QUADS, tp) && key(E::TRIANGLES, tp) == key(E::QUAD_STRIP, tp));
			CHECK(key(E::LINES, tp) == key(E::LINE_STRIP, tp) && key(E::LINES, tp) == key(E::LINE_LOOP, tp));
			CHECK(key(E::RECTS, tp) != key(E::TRIANGLES, tp));
			CHECK(key(E::LINE_STRIP_ADJACENT, tp) != key(E::LINE_STRIP, tp));
			CHECK(key(E::POINTS, tp) != key(E::LINES, tp) && key(E::POINTS, tp) != key(E::TRIANGLES, tp));
			// the topology and restart the draw sets and the pipeline bakes in
			struct Want { E mode; VkPrimitiveTopology topology; VkBool32 restart; };
			const Want want[] = {
				{E::POINTS, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE},
				{E::LINES, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_FALSE},
				{E::LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_TRUE},
				{E::LINE_LOOP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_TRUE},
				{E::LINE_STRIP_ADJACENT, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY, VK_TRUE},
				{E::TRIANGLES, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
				{E::TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_TRUE},
				{E::TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_TRUE},
				{E::QUADS, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
				{E::QUAD_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
				{E::RECTS, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE},
			};
			for (const Want& w : want)
			{
				const VulkanInputAssembly ia = VulkanPipelineKey_InputAssembly(w.mode);
				if (!ia.supported || ia.topology != w.topology || ia.primitiveRestartEnable != w.restart)
				{
					std::printf("FAIL: primitive type %x: topology %d restart %d\n", (uint32)w.mode, (int)ia.topology, (int)ia.primitiveRestartEnable);
					s_failures++;
				}
			}
			CHECK(!VulkanPipelineKey_InputAssembly(E::TRIANGLES_ADJACENT).supported);
		}
		// the whole key: each shader, the render pass and the rest
		{
			const uint32 f = vi | rz;
			const uint64 g = VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s0, f);
			CHECK(VulkanPipelineKey_Graphics(&fs, 12, 0, 13, 17, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 5, 13, 17, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 14, 17, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 18, *s0, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s1, f) == g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s1, 0) != VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s0, 0));
			auto c = std::make_unique<LatteContextRegister>(*s0);
			SetReg(*c, REGADDR::CB_BLEND0_CONTROL, 0x00030003);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *c, f) != g);
			CHECK(VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *c, f | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND) ==
				VulkanPipelineKey_Graphics(&fs, 11, 0, 13, 17, *s0, f | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND));
		}

		// the vertex input: the attributes the vertex shader maps, at its
		// locations, with the register's strides
		sint8 mapping[LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS];
		std::fill(std::begin(mapping), std::end(mapping), (sint8)-1);
		mapping[5] = 0;
		mapping[2] = 1; // semantic 9 not read by the shader
		auto in = std::make_unique<VulkanVertexInput>();
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in);
		CHECK(in->bindingCount == 2 && in->attributeCount == 2);
		if (in->bindingCount == 2 && in->attributeCount == 2)
		{
			CHECK(in->bindings[0].binding == 3 && in->bindings[0].stride == 20 && in->bindings[0].inputRate == VK_VERTEX_INPUT_RATE_VERTEX);
			CHECK(in->bindings[1].binding == 7 && in->bindings[1].stride == 4 && in->bindings[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
			CHECK(in->bindings[0].divisor == 1 && in->bindings[1].divisor == 1);
			CHECK(in->attributes[0].location == 0 && in->attributes[0].binding == 3 && in->attributes[0].offset == 4 &&
				in->attributes[0].format == VK_FORMAT_R32G32B32_UINT);
			CHECK(in->attributes[1].location == 1 && in->attributes[1].binding == 7 && in->attributes[1].offset == 0 &&
				in->attributes[1].format == VK_FORMAT_R16G16_UINT);
			CHECK(in->bindings[0].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT);
			CHECK(in->attributes[0].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT);
		}
		// what the draw compares to set it again
		auto in2 = std::make_unique<VulkanVertexInput>();
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(VulkanPipelineKey_SameVertexInput(*in, *in2));
		VulkanPipelineKey_VertexInput(fs, mapping, *s1, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[0].offset = 8;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[0].offset = 4;
		mapping[9] = 2;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		CHECK(in2->attributeCount == 3);
		mapping[9] = -1;
		attrs[2].format = Latte::E_HWFMT::HWFMT_32_FLOAT;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[2].format = Latte::E_HWFMT::HWFMT_16_16_FLOAT;
		attrs[2].fetchType = LatteConst::VERTEX_DATA;
		VulkanPipelineKey_VertexInput(fs, mapping, *s0, *in2);
		CHECK(!VulkanPipelineKey_SameVertexInput(*in, *in2));
		attrs[2].fetchType = LatteConst::INSTANCE_DATA;
		fs.bufferGroups.clear();
		fs2.bufferGroups.clear();
	}
	// graphics pipeline libraries: the part each dynamic state the renderer
	// sets belongs to
	{
		struct Want { VkDynamicState state; int part; };
		const Want want[] = {
			{VK_DYNAMIC_STATE_VIEWPORT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_SCISSOR, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_DEPTH_BIAS, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_CULL_MODE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_FRONT_FACE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION},
			{VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_REFERENCE, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_STENCIL_OP_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER},
			{VK_DYNAMIC_STATE_BLEND_CONSTANTS, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT},
			{VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT},
			{VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT, VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT},
			{VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT, VULKAN_PIPELINE_LIBRARY_VERTEX_INPUT},
		};
		for (const Want& w : want)
			if (VulkanPipelineLibrary_DynamicStatePart(w.state) != w.part)
			{
				std::printf("FAIL: dynamic state %d belongs to part %d, not %d\n", (int)w.state,
					VulkanPipelineLibrary_DynamicStatePart(w.state), w.part);
				s_failures++;
			}
	}
	// a part's key: what it bakes in, not what its dynamic states leave to
	// the draw, nor the other parts' state
	{
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkVertexInputBindingDescription binding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
		vi.vertexBindingDescriptionCount = 1;
		vi.pVertexBindingDescriptions = &binding;
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = vp.scissorCount = 1;
		VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.pNext = &clip;
		rs.cullMode = VK_CULL_MODE_BACK_BIT;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		ds.depthTestEnable = VK_TRUE;
		ds.depthCompareOp = VK_COMPARE_OP_LESS;
		VkPipelineColorBlendAttachmentState att{};
		att.blendEnable = VK_TRUE;
		att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		att.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &att;
		std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.pVertexInputState = &vi;
		gp.pInputAssemblyState = &ia;
		gp.pViewportState = &vp;
		gp.pRasterizationState = &rs;
		gp.pMultisampleState = &ms;
		gp.pDepthStencilState = &ds;
		gp.pColorBlendState = &cb;
		gp.pDynamicState = &dsi;
		const VulkanPipelineLibraryIdentity id{1, 0, 2, 3, false};
		auto keys = [&](const VulkanPipelineLibraryIdentity& identity) {
			dsi.dynamicStateCount = (uint32)dyn.size();
			dsi.pDynamicStates = dyn.data();
			std::vector<uint64> k;
			for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
				k.push_back(VulkanPipelineLibrary_Key((VulkanPipelineLibraryPart)p, gp, identity));
			return k;
		};
		// which parts a change moves: a bit per part
		auto moved = [&](const std::vector<uint64>& before, const std::vector<uint64>& after) {
			uint32 m = 0;
			for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
				m |= (before[p] != after[p]) << p;
			return m;
		};
		const uint32 VI = 1, PRS = 2, FS = 4, FO = 8;
		const std::vector<uint64> k0 = keys(id);
		std::set<uint64> distinct(k0.begin(), k0.end());
		CHECK(distinct.size() == 4);
		// the shaders, the render pass and robustness
		CHECK(moved(k0, keys({9, 0, 2, 3, false})) == PRS);
		CHECK(moved(k0, keys({1, 7, 2, 3, false})) == PRS);
		CHECK(moved(k0, keys({1, 0, 9, 3, false})) == FS);
		CHECK(moved(k0, keys({1, 0, 2, 9, false})) == (PRS | FS | FO));
		CHECK(moved(k0, keys({1, 0, 2, 3, true})) == (PRS | FS));
		// baked state moves its own part only
		binding.stride = 20;
		CHECK(moved(k0, keys(id)) == VI);
		binding.stride = 16;
		rs.cullMode = VK_CULL_MODE_NONE;
		CHECK(moved(k0, keys(id)) == PRS);
		rs.cullMode = VK_CULL_MODE_BACK_BIT;
		clip.depthClipEnable = VK_TRUE;
		CHECK(moved(k0, keys(id)) == PRS);
		clip.depthClipEnable = VK_FALSE;
		ds.depthCompareOp = VK_COMPARE_OP_GREATER;
		CHECK(moved(k0, keys(id)) == FS);
		ds.depthCompareOp = VK_COMPARE_OP_LESS;
		att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		CHECK(moved(k0, keys(id)) == FO);
		att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
		CHECK(moved(k0, keys(id)) == (FS | FO));
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		CHECK(moved(k0, keys(id)) == 0);
		// with the state dynamic, its baked value keys nothing
		dyn.insert(dyn.end(), {VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_CULL_MODE_EXT,
			VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT});
		const std::vector<uint64> kd = keys(id);
		CHECK(moved(k0, kd) == (VI | PRS | FS | FO)); // the dynamic states themselves are keyed
		binding.stride = 20;
		rs.cullMode = VK_CULL_MODE_NONE;
		ds.depthCompareOp = VK_COMPARE_OP_GREATER;
		att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		CHECK(moved(kd, keys(id)) == 0);
		// a topology of another class still keys the vertex input
		ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		CHECK(moved(kd, keys(id)) == VI);
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		// and what stays baked still does
		rs.polygonMode = VK_POLYGON_MODE_LINE;
		CHECK(moved(kd, keys(id)) == PRS);
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		att.blendEnable = VK_FALSE;
		CHECK(moved(kd, keys(id)) == FO);
		att.blendEnable = VK_TRUE;
		ds.stencilTestEnable = VK_TRUE;
		CHECK(moved(kd, keys(id)) == FS);
	}
	// the cache: one maker per key, however many threads race for it
	{
		auto cache = std::make_unique<VulkanPipelineLibraryCache>();
		const VulkanPipelineLibraryPart part = VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER;
		VkPipeline found;
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Missing);
		CHECK(cache->Claim(part, 42));
		CHECK(!cache->Claim(part, 42));
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Pending);
		// a part that failed: missing again, and claimed again
		cache->Publish(part, 42, VK_NULL_HANDLE);
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Missing);
		CHECK(cache->Claim(part, 42));
		cache->Publish(part, 42, (VkPipeline)(uintptr_t)0x4200);
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Ready && found == (VkPipeline)(uintptr_t)0x4200);
		CHECK(!cache->Claim(part, 42));
		// other parts are other tables
		CHECK(cache->Find(VULKAN_PIPELINE_LIBRARY_FRAGMENT_OUTPUT, 42, found) == VulkanPipelineLibraryCache::State::Missing);
		cache->DestroyAll(VK_NULL_HANDLE);
		CHECK(cache->Find(part, 42, found) == VulkanPipelineLibraryCache::State::Missing && cache->Count(part) == 0);

		// eight threads claiming the same 4000 keys, colliding in the table:
		// each key made once, every thread then finds every part
		const int threads = 8, keys = 4000;
		std::vector<std::atomic<int>> makers(keys);
		std::vector<std::thread> pool;
		std::atomic<int> wrong{0};
		for (int t = 0; t < threads; t++)
			pool.emplace_back([&, t] {
				for (int n = 0; n < keys; n++)
				{
					const int i = (n * 7 + t * 613) % keys;
					const uint64 key = 0x100000000ull * (uint64)(i % 61) + (uint64)i + 1; // shared low bits
					if (cache->Claim(part, key))
					{
						makers[i].fetch_add(1);
						cache->Publish(part, key, (VkPipeline)(uintptr_t)(0x1000 + i));
					}
				}
				for (int i = 0; i < keys; i++)
				{
					const uint64 key = 0x100000000ull * (uint64)(i % 61) + (uint64)i + 1;
					VkPipeline f;
					// made, or still being made by another thread
					const auto state = cache->Find(part, key, f);
					if (state == VulkanPipelineLibraryCache::State::Missing ||
						(state == VulkanPipelineLibraryCache::State::Ready && f != (VkPipeline)(uintptr_t)(0x1000 + i)))
						wrong++;
				}
			});
		for (auto& t : pool)
			t.join();
		int once = 0;
		for (auto& m : makers)
			once += m.load() == 1;
		std::printf("library cache: %d keys claimed by %d threads, %d made once, %zu kept\n", keys, threads, once, cache->Count(part));
		CHECK(once == keys && wrong == 0 && cache->Count(part) == (size_t)keys);
		cache->DestroyAll(VK_NULL_HANDLE);
		// a full table: no claim, the part adopted instead
		for (size_t i = 0; i < VulkanPipelineLibraryCache::kCapacity; i++)
			CHECK(cache->Claim(part, i + 1));
		CHECK(!cache->Claim(part, VulkanPipelineLibraryCache::kCapacity + 7));
		CHECK(cache->Find(part, VulkanPipelineLibraryCache::kCapacity + 7, found) == VulkanPipelineLibraryCache::State::Missing);
		cache->Adopt((VkPipeline)(uintptr_t)0x77);
		cache->DestroyAll(VK_NULL_HANDLE);
		CHECK(cache->Claim(part, 5));
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
