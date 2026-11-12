// The shader/PSO gate (AGENTS.md, "Shader compilation and stutter";
// docs/SHADER-STUTTER.md, M0). Shader compilation only ever gets better: this
// measures what the core's shader path costs and fails when any of it got
// worse than the baseline checked in beside it (baseline.txt).
//
// Counted, exact, gated:
//  - the shader corpus test/shaders compiles (the core's own shaders, the
//    shapes the decompiler writes, every graphic pack shader shipped for
//    Vulkan), through rslc as RendererShaderVk hands it GLSL: each module's
//    SPIR-V instruction count, and the corpus' modules, words and
//    instructions;
//  - the file cache the shader and pipeline caches are kept in: the file
//    table entries looked at to write 1000 entries and to look each of them
//    up again, and the heap allocations and bytes allocated to read them
//    back (those on Linux only: the stream is per platform);
//  - the register file a cached shader or pipeline is rebuilt with at boot:
//    the heap allocations and bytes allocated to rebuild 1000;
//  - the pipelines a replayed draw sequence makes (the Vulkan renderer's
//    pipeline key, VulkanPipelineKey_Graphics), with each set of state the
//    device can leave to the draw: a frame's shape from a seed, shader sets
//    drawn into a scene, shadow and post pass with the materials, decals,
//    depth-only passes, strips, fans and vertex layouts a title draws -
//    and the same again after a round trip through the recording a
//    PSO_TRACE build makes of a title's draws (VulkanPipelineRecord.h).
//
// Timed, reported, never gated (CI machines are shared; times are taken on
// the developer's machines, docs/SHADER-STUTTER.md): rslc per shader, median,
// p95, p99 and max over the corpus, with --repeat N.
//
// pso_gate <graphic packs> [--baseline FILE] [--write FILE] [--repeat N] [--record FILE]
//   --record    write the replayed draws as a recording (an example for --replay)
// pso_gate --replay FILE
//   the pipelines a title's recorded draws make with each set of dynamic
//   state (PSO_TRACE=1 build, PSO_RECORD=FILE): a private corpus, not gated
//   --baseline  compare with FILE; worse fails, better fails too until FILE
//               is updated in the same commit (the gate only ratchets down)
//   --write     write the measured numbers to FILE
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#define SHADERS_CORPUS_ONLY
#include "../shaders/shaders_test.cpp"

#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineRecord.h"
#include "util/helpers/MappedFile.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"

#include <random>
#include <set>


// --- counting the heap -------------------------------------------------------

static std::atomic<bool> s_counting{false};
static std::atomic<uint64_t> s_allocs{0};
static std::atomic<uint64_t> s_allocBytes{0};

static void Count(size_t n)
{
	if (s_counting.load(std::memory_order_relaxed))
	{
		s_allocs.fetch_add(1, std::memory_order_relaxed);
		s_allocBytes.fetch_add(n, std::memory_order_relaxed);
	}
}

// operator new, and malloc through the linker's --wrap (Makefile)
void* operator new(size_t n)
{
	Count(n);
	if (void* p = std::malloc(n ? n : 1))
		return p;
	throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

extern "C"
{
	void* __real_malloc(size_t n);
	void* __real_calloc(size_t c, size_t n);
	void* __real_realloc(void* p, size_t n);
	void* __wrap_malloc(size_t n)
	{
		Count(n);
		return __real_malloc(n);
	}
	void* __wrap_calloc(size_t c, size_t n)
	{
		Count(c * n);
		return __real_calloc(c, n);
	}
	void* __wrap_realloc(void* p, size_t n)
	{
		Count(n);
		return __real_realloc(p, n);
	}
}

// --- what is measured ----------------------------------------------------------

static std::map<std::string, uint64_t> s_metrics;
static std::vector<double> s_compileMicros;

static uint64_t Instructions(const uint32_t* w, size_t n)
{
	uint64_t count = 0;
	for (size_t i = 5; i < n && (w[i] >> 16);)
	{
		count++;
		i += w[i] >> 16;
	}
	return count;
}

static void OnModule(const std::string& name, const uint32_t* words, size_t count, double micros)
{
	const uint64_t instructions = Instructions(words, count);
	s_metrics["shader " + name + " instructions"] = instructions;
	s_metrics["corpus modules"]++;
	s_metrics["corpus words"] += count;
	s_metrics["corpus instructions"] += instructions;
	s_compileMicros.push_back(micros);
}

static void MeasureCorpus(const fs::path& packs, int repeat)
{
	for (int r = 0; r < repeat; r++)
	{
		// each repeat measures the same modules: keep the counts of one
		if (r > 0)
			s_onModule = [](const std::string&, const uint32_t*, size_t, double micros) { s_compileMicros.push_back(micros); };
		TestCoreShaders(fs::path(__FILE__).parent_path() / "../../src");
		TestDecompilerShapes();
		TestGraphicPacks(packs);
	}
}

// A cache of shader-cache-sized entries (a few hundred bytes to a few KiB of
// compressible microcode-like words), written, then read back entry by
// entry the way the boot load does
static void MeasureFileCache()
{
	constexpr int ENTRIES = 1000;
	const fs::path path = fs::temp_directory_path() / "pso_gate_filecache.bin";
	std::error_code ec;
	fs::remove(path, ec);
	{
		FileCache* cache = FileCache::Create(path, 1);
		if (!cache)
		{
			std::fprintf(stderr, "FAIL: cannot create %s\n", path.string().c_str());
			s_failures++;
			return;
		}
		uint32_t seed = 1;
		file_cache_probes_reset();
		for (int i = 0; i < ENTRIES; i++)
		{
			std::vector<uint8_t> data(256 + (i * 37) % 4096);
			for (size_t b = 0; b < data.size(); b += 4)
			{
				seed = seed * 1103515245u + 12345u;
				uint32_t w = (b % 32 < 16) ? (uint32_t)(b * 0x01000193u) : (seed >> 8) & 0x00FF00FF;
				std::memcpy(&data[b], &w, std::min<size_t>(4, data.size() - b));
			}
			cache->AddFile({ (uint64)i + 1, 7 }, data.data(), (sint32)data.size());
		}
		// written by the cache's writer thread
		cache->WaitForWrites();
		s_metrics["filecache table entries looked at to write 1000 entries"] = file_cache_probes();
		delete cache;
	}
	FileCache* cache = FileCache::Open(path, false, 1);
	if (!cache)
	{
		std::fprintf(stderr, "FAIL: cannot reopen %s\n", path.string().c_str());
		s_failures++;
		return;
	}
	std::vector<uint8> out;
	cache->GetFile({ 1, 7 }, out); // whatever the first read sets up once
	s_allocs = 0;
	s_allocBytes = 0;
	file_cache_probes_reset();
	s_counting = true;
	int read = 0;
	for (int i = 0; i < ENTRIES; i++)
	{
		std::vector<uint8> data;
		if (cache->GetFile({ (uint64)i + 1, 7 }, data) && data.size() == (size_t)(256 + (i * 37) % 4096))
			read++;
	}
	s_counting = false;
	s_metrics["filecache table entries looked at to find 1000 entries"] = file_cache_probes();
	delete cache;
	fs::remove(path, ec);
	if (read != ENTRIES)
	{
		std::fprintf(stderr, "FAIL: read back %d of %d cache entries\n", read, ENTRIES);
		s_failures++;
	}
#if defined(__linux__)
	const char* platform = "linux";
#elif defined(_WIN32)
	const char* platform = "windows";
#else
	const char* platform = "other";
#endif
	// per 1000 entries, so a fraction of an allocation per entry still counts
	s_metrics[std::string("filecache.") + platform + " allocations per 1000 entries read"] = s_allocs.load();
	s_metrics[std::string("filecache.") + platform + " bytes allocated per 1000 entries read"] = s_allocBytes.load();
}

static void MeasureRegisterRebuild()
{
	static Latte::GPUCompactedRegisterState state;
	uint64 check = 0;
	Latte::LoadGPURegisterStateScratch(state); // whatever the first sets up once
	s_allocs = 0;
	s_allocBytes = 0;
	s_counting = true;
	for (int i = 0; i < 1000; i++)
	{
		for (int r = 0; r < Latte::GPUCompactedRegisterState::NUM_REGS; r += 7)
			state.rawArray[r] = (uint32)(i * 31 + r);
		check += Latte::LoadGPURegisterStateScratch(state).GetRawView()[0];
	}
	s_counting = false;
	(void)check;
	s_metrics["cache entry register files: allocations per 1000 rebuilt"] = s_allocs.load();
	s_metrics["cache entry register files: bytes allocated per 1000 rebuilt"] = s_allocBytes.load();
}

// Each set of state the device can leave to the draw: none, then each
// extension the core enables on top of the ones before
struct DynamicCaps
{
	const char* name;
	uint32 flags;
};
static const DynamicCaps s_caps[] = {
	{"nothing dynamic", 0},
	{"extended dynamic state", VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL},
	{"+ _3 blend", VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND},
	{"+ _2", VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND | VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER},
	{"+ vertex input", VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND |
		VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER | VULKAN_PIPELINE_KEY_DYNAMIC_VERTEX_INPUT},
	{"+ topology", VULKAN_PIPELINE_KEY_DYNAMIC_DEPTH_STENCIL_CULL | VULKAN_PIPELINE_KEY_DYNAMIC_BLEND |
		VULKAN_PIPELINE_KEY_DYNAMIC_RASTERIZER | VULKAN_PIPELINE_KEY_DYNAMIC_VERTEX_INPUT | VULKAN_PIPELINE_KEY_DYNAMIC_TOPOLOGY},
};

// the pipelines a recording's states make with each set
static std::vector<size_t> CountRecording(std::span<const uint8> data, size_t& states, bool& valid)
{
	std::vector<std::set<uint64>> keys(std::size(s_caps));
	states = 0;
	valid = VulkanPipelineRecord_Read(data, [&](const VulkanPipelineRecordEntry& e) {
		states++;
		for (size_t c = 0; c < std::size(s_caps); c++)
			keys[c].insert(VulkanPipelineRecord_Key(e, s_caps[c].flags));
	});
	std::vector<size_t> counts;
	for (auto& k : keys)
		counts.push_back(k.size());
	return counts;
}

// A frame's draws from a seed: what changes between them is what a title
// changes - the shaders, the render pass, a material's depth, cull and
// blend state, the stencil reference, polygon offset on decals, depth-only
// draws with rasterization killed, strips and fans among the lists, and
// the vertex buffers' strides and offsets. The pipelines they make, counted
// by the renderer's own key, with each set of state the device can leave
// to the draw (none, then each extension the core enables on top).
static std::string s_recordFile;

static void MeasureReplay()
{
	using namespace Latte;
	struct Material
	{
		uint32 depthControl, cull, blendEnable, blend, targetMask;
	};
	// depth test and write (LEQUAL), test only, off; cull back, none,
	// front; opaque, alpha, additive; RGBA, RGB
	const uint32 depthControls[] = {(1 << 1) | (1 << 2) | (3 << 4), (1 << 1) | (3 << 4), 0};
	const uint32 culls[] = {2, 0, 1};
	const uint32 blends[] = {0x00010001, 0x05040504, 0x01010101};
	const uint32 targetMasks[] = {0xF, 0x7};
	std::mt19937 rng(0x7e1a);
	std::vector<Material> materials;
	for (int i = 0; i < 24; i++)
	{
		const uint32 b = rng() % 3;
		materials.push_back({depthControls[rng() % 3], culls[rng() % 3], b != 0, blends[b], targetMasks[rng() % 2]});
	}
	// 40 shader sets, each with its materials; two fetch shaders per vertex
	// layout, differing in attribute offsets (the case the key's note in
	// the renderer describes)
	struct ShaderSet
	{
		uint64 vs, ps;
		std::vector<uint32> materials;
	};
	std::vector<ShaderSet> sets;
	for (uint32 i = 0; i < 40; i++)
	{
		ShaderSet set{0x1000 + i * 0x9E3779B97F4A7C15ull, 0x2000 + i * 0xC2B2AE3D27D4EB4Full, {}};
		for (uint32 m = 0, n = 1 + rng() % 3; m < n; m++)
			set.materials.push_back(rng() % materials.size());
		sets.push_back(set);
	}
	LatteParsedFetchShaderAttribute attrs[2]{};
	// positions and colors from buffer 3, a second set of coordinates from
	// buffer 5 at a fixed stride
	attrs[0] = {3, 0, E_HWFMT::HWFMT_32_32_32_FLOAT, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 0};
	attrs[1] = {3, 1, E_HWFMT::HWFMT_8_8_8_8, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 12};
	LatteParsedFetchShaderAttribute uv{5, 2, E_HWFMT::HWFMT_16_16_FLOAT, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 0};
	LatteParsedFetchShaderBufferGroup group{}, group2{};
	group.attributeBufferIndex = 3;
	group.attribCount = 2;
	group.attrib = attrs;
	group2.attributeBufferIndex = 5;
	group2.attribCount = 1;
	group2.attrib = &uv;
	LatteFetchShader fetch[2];
	for (int f = 0; f < 2; f++)
	{
		fetch[f].bufferGroups = {group, group2};
		fetch[f].vkPipelineHashFragment = 0x51ED + f * 0x1000193;
	}
	const uint32 strides[] = {16, 20, 24, 28, 32};
	const uint64 renderPasses[] = {0xA11CE, 0xB0B, 0xC0FFEE}; // scene, shadow, post

	struct Draw
	{
		uint32 set, material, pass, primitive, stride, fetch, stencilRef;
		bool decal, depthOnly;
	};
	std::vector<Draw> draws;
	for (int i = 0; i < 2000; i++)
	{
		Draw d{};
		d.set = rng() % sets.size();
		d.material = sets[d.set].materials[rng() % sets[d.set].materials.size()];
		const uint32 kind = rng() % 20;
		d.pass = kind < 15 ? 0 : kind < 18 ? 1 : 2;
		const uint32 prim = rng() % 10;
		d.primitive = prim < 6 ? 4 : prim < 9 ? 6 : 5; // lists, strips, fans
		d.stride = strides[rng() % std::size(strides)];
		d.fetch = rng() % 2;
		d.stencilRef = rng() & 0xFF;
		d.decal = d.pass == 0 && rng() % 10 == 0;
		d.depthOnly = d.pass == 1 && rng() % 4 == 0;
		draws.push_back(d);
	}

	auto lcr = std::make_unique<LatteContextRegister>();
	uint64 previous = ~0ull;
	std::vector<size_t> direct;
	// and recorded as a PSO_TRACE build records a title's draws: kept
	// whole, and written out after each state as the core does
	VulkanPipelineRecorder recorder, streamed;
	std::vector<uint8> streamedOut;
	for (const DynamicCaps& c : s_caps)
	{
		std::set<uint64> pipelines;
		for (const Draw& d : draws)
		{
			const Material& m = materials[d.material];
			std::memset(lcr.get(), 0, sizeof(LatteContextRegister));
			uint32* r = lcr->GetRawView();
			r[REGADDR::DB_DEPTH_CONTROL] = d.depthOnly ? ((1 << 1) | (1 << 2) | (3 << 4)) : m.depthControl;
			r[REGADDR::DB_STENCILREFMASK] = d.stencilRef | (0xFF << 8) | (0xFF << 16);
			r[REGADDR::PA_SU_SC_MODE_CNTL] = m.cull | (d.decal ? (1u << 11) : 0);
			r[REGADDR::PA_CL_CLIP_CNTL] = d.depthOnly ? (1u << 22) : 0;
			r[REGADDR::PA_CL_VTE_CNTL] = 1u << 1;
			r[REGADDR::CB_COLOR_CONTROL] = 0xCC0000 | (m.blendEnable << 8);
			r[REGADDR::CB_BLEND0_CONTROL] = m.blend;
			r[REGADDR::CB_TARGET_MASK] = d.depthOnly ? 0 : m.targetMask;
			r[mmVGT_PRIMITIVE_TYPE] = d.primitive;
			r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + 3 * 7 + 2] = d.stride << 11;
			r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + 5 * 7 + 2] = 4 << 11;
			pipelines.insert(VulkanPipelineKey_Graphics(&fetch[d.fetch], sets[d.set].vs, 0, sets[d.set].ps,
				renderPasses[d.pass], *lcr, c.flags));
			if (c.flags == 0)
			{
				recorder.Draw(fetch[d.fetch], sets[d.set].vs, 0, sets[d.set].ps, renderPasses[d.pass], *lcr);
				if (streamed.Draw(fetch[d.fetch], sets[d.set].vs, 0, sets[d.set].ps, renderPasses[d.pass], *lcr))
				{
					streamedOut.insert(streamedOut.end(), streamed.Output().begin(), streamed.Output().end());
					streamed.Taken();
				}
			}
		}
		direct.push_back(pipelines.size());
		std::printf("replayed draws: %zu pipelines for %zu draws, %s\n", pipelines.size(), draws.size(), c.name);
		s_metrics[std::string("replayed draws: pipelines, ") + c.name] = pipelines.size();
		// each set leaves more to the draw: never more pipelines
		if (pipelines.size() > previous)
		{
			std::printf("FAIL: %s makes more pipelines than the set before it\n", c.name);
			s_failures++;
		}
		previous = pipelines.size();
	}
	// the recording replayed: the same pipelines with every set, one state
	// for each the finest key tells apart
	size_t states = 0;
	bool valid = false;
	const std::vector<size_t> replayed = CountRecording(recorder.Output(), states, valid);
	std::printf("replayed draws, recorded: %zu states in %zu bytes\n", states, recorder.Output().size());
	if (!valid || states != direct[0] || states != recorder.States() || replayed != direct)
	{
		std::printf("FAIL: the recording replays to other pipelines (%zu states for %zu)\n", states, direct[0]);
		for (size_t c = 0; c < replayed.size(); c++)
			std::printf("  %s: %zu, drawn %zu\n", s_caps[c].name, replayed[c], direct[c]);
		s_failures++;
	}
	if (!std::equal(recorder.Output().begin(), recorder.Output().end(), streamedOut.begin(), streamedOut.end()) ||
		!streamed.Output().empty())
	{
		std::printf("FAIL: written out after each state, the recording is not the same\n");
		s_failures++;
	}
	// cut short: what was whole still read, the rest refused
	const std::span<const uint8> recording = recorder.Output();
	const std::span<const uint8> cut = recording.first(recording.size() - 5);
	size_t cutStates = 0;
	bool cutValid = true;
	CountRecording(cut, cutStates, cutValid);
	if (cutValid || cutStates != states - 1)
	{
		std::printf("FAIL: a recording cut short reads as whole (%zu states)\n", cutStates);
		s_failures++;
	}
	// two sessions appended: each state still counted once
	std::vector<uint8> twice(recording.begin(), recording.end());
	twice.insert(twice.end(), recording.begin(), recording.end());
	size_t twiceStates = 0;
	bool twiceValid = false;
	if (CountRecording(twice, twiceStates, twiceValid) != direct || !twiceValid || twiceStates != 2 * states)
	{
		std::printf("FAIL: two sessions in one recording count differently\n");
		s_failures++;
	}
	if (!s_recordFile.empty())
	{
		std::ofstream out(s_recordFile, std::ios::binary);
		out.write((const char*)recording.data(), recording.size());
	}
	for (int f = 0; f < 2; f++)
		fetch[f].bufferGroups.clear();
}

// a title's recorded draws (PSO_TRACE=1 build, PSO_RECORD=FILE)
static int Replay(const char* file)
{
	// read where the file is mapped, for as long as it is
	const MappedFile data{fs::path(file)};
	size_t states = 0;
	bool valid = false;
	const std::vector<size_t> counts = CountRecording({data.data(), data.size()}, states, valid);
	std::printf("%s: %zu draw states%s\n", file, states, valid ? "" : " (then not a recording, or cut short)");
	for (size_t c = 0; c < counts.size(); c++)
		std::printf("  pipelines, %s: %zu\n", s_caps[c].name, counts[c]);
	return valid ? 0 : 1;
}

static void ReportTimes()
{
	if (s_compileMicros.empty())
		return;
	std::vector<double> t = s_compileMicros;
	std::sort(t.begin(), t.end());
	auto at = [&](double q) { return t[std::min(t.size() - 1, (size_t)(q * (t.size() - 1) + 0.5))]; };
	double total = 0;
	for (double v : t)
		total += v;
	std::printf("rslc per shader (not gated): median %.1f us, p95 %.1f us, p99 %.1f us, max %.1f us, total %.1f ms over %zu compiles\n",
		at(0.5), at(0.95), at(0.99), t.back(), total / 1000.0, t.size());
}

static std::map<std::string, uint64_t> ReadMetrics(const std::string& file)
{
	std::map<std::string, uint64_t> m;
	std::ifstream in(file);
	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		size_t sp = line.rfind(' ');
		if (sp != std::string::npos)
			m[line.substr(0, sp)] = std::strtoull(line.c_str() + sp + 1, nullptr, 10);
	}
	return m;
}

static int Compare(const std::map<std::string, uint64_t>& base)
{
	int worse = 0, better = 0, missing = 0, fresh = 0;
	for (auto& [name, was] : base)
	{
		auto it = s_metrics.find(name);
		if (it == s_metrics.end())
		{
			// a platform's own numbers are compared on that platform only
			if (name.rfind("filecache.", 0) == 0)
				continue;
			std::printf("GONE    %s (was %llu)\n", name.c_str(), (unsigned long long)was);
			missing++;
		}
		else if (it->second > was)
		{
			std::printf("WORSE   %s: %llu -> %llu\n", name.c_str(), (unsigned long long)was, (unsigned long long)it->second);
			worse++;
		}
		else if (it->second < was)
		{
			std::printf("BETTER  %s: %llu -> %llu\n", name.c_str(), (unsigned long long)was, (unsigned long long)it->second);
			better++;
		}
	}
	for (auto& [name, now] : s_metrics)
		if (!base.count(name) && name.rfind("filecache.", 0) != 0)
		{
			std::printf("NEW     %s: %llu\n", name.c_str(), (unsigned long long)now);
			fresh++;
		}
	std::printf("pso gate: %d worse, %d better, %d gone, %d new\n", worse, better, missing, fresh);
	if (worse)
		std::printf("pso gate: FAIL - shader compilation got worse (AGENTS.md: only the developer accepts that, in the commit message)\n");
	else if (better || missing || fresh)
		std::printf("pso gate: FAIL - update the baseline in this commit: make -C test/pso baseline\n");
	return worse || better || missing || fresh;
}

int main(int argc, char** argv)
{
	if (argc == 3 && !std::strcmp(argv[1], "--replay"))
		return Replay(argv[2]);
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: pso_gate <cemu_graphic_packs> [--baseline FILE] [--write FILE] [--repeat N] [--record FILE]\n"
			"       pso_gate --replay FILE\n");
		return 2;
	}
	std::string baseline, write;
	int repeat = 1;
	for (int i = 2; i + 1 < argc; i += 2)
	{
		if (!std::strcmp(argv[i], "--baseline"))
			baseline = argv[i + 1];
		else if (!std::strcmp(argv[i], "--write"))
			write = argv[i + 1];
		else if (!std::strcmp(argv[i], "--record"))
			s_recordFile = argv[i + 1];
		else if (!std::strcmp(argv[i], "--repeat"))
			repeat = std::max(1, std::atoi(argv[i + 1]));
	}
	s_onModule = OnModule;
	MeasureCorpus(argv[1], repeat);
	MeasureFileCache();
	MeasureRegisterRebuild();
	MeasureReplay();
	if (s_failures)
	{
		std::printf("pso gate: %d failure(s) measuring\n", s_failures);
		return 1;
	}
	std::printf("corpus: %llu modules, %llu words, %llu instructions\n",
		(unsigned long long)s_metrics["corpus modules"], (unsigned long long)s_metrics["corpus words"],
		(unsigned long long)s_metrics["corpus instructions"]);
	for (auto& [name, v] : s_metrics)
		if (name.rfind("filecache.", 0) == 0)
			std::printf("%s: %llu\n", name.c_str(), (unsigned long long)v);
	ReportTimes();
	if (!write.empty())
	{
		std::ofstream out(write, std::ios::binary);
		out << "# The shader/PSO gate's baseline (test/pso/pso_gate.cpp): what the shader\n"
			   "# path costs at this commit. Lower is better; nothing may go up.\n"
			   "# Regenerate with: make -C test/pso baseline\n";
		for (auto& [name, v] : s_metrics)
			out << name << ' ' << v << '\n';
	}
	if (!baseline.empty())
		return Compare(ReadMetrics(baseline));
	return 0;
}
