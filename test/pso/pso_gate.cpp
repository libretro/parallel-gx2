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
//    back (those on Linux only: the stream is per platform).
//
// Timed, reported, never gated (CI machines are shared; times are taken on
// the developer's machines, docs/SHADER-STUTTER.md): rslc per shader, median,
// p95, p99 and max over the corpus, with --repeat N.
//
// pso_gate <graphic packs> [--baseline FILE] [--write FILE] [--repeat N]
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

extern std::atomic<uint64> g_fileCacheProbes;

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
		g_fileCacheProbes = 0;
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
		s_metrics["filecache table entries looked at to write 1000 entries"] = g_fileCacheProbes.load();
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
	g_fileCacheProbes = 0;
	s_counting = true;
	int read = 0;
	for (int i = 0; i < ENTRIES; i++)
	{
		std::vector<uint8> data;
		if (cache->GetFile({ (uint64)i + 1, 7 }, data) && data.size() == (size_t)(256 + (i * 37) % 4096))
			read++;
	}
	s_counting = false;
	s_metrics["filecache table entries looked at to find 1000 entries"] = g_fileCacheProbes.load();
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
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: pso_gate <cemu_graphic_packs> [--baseline FILE] [--write FILE] [--repeat N]\n");
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
		else if (!std::strcmp(argv[i], "--repeat"))
			repeat = std::max(1, std::atoi(argv[i + 1]));
	}
	s_onModule = OnModule;
	MeasureCorpus(argv[1], repeat);
	MeasureFileCache();
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
