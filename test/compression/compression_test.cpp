// zlib and Zstandard through libretro-common, in place of the zlib and zstd
// libraries the core used to build:
//  - Deflate (util/helpers/Deflate.cpp) reads the streams zlib writes, its
//    streams read back, and it tells a short output from a bad stream;
//  - the console's zlib125 (Cafe/OS/libs/zlib125), on the host side of a
//    guest z_stream: inflate and deflate a byte at a time, the containers,
//    the flushes, reset, adler, totals and the return codes games check;
//  - the shader and pipeline caches' entries standalone Cemu writes decode,
//    and the ones written here decode, both through the dictionaries;
//  - the cache file's async writer (Cemu/FileCache) has every entry four
//    threads queued written out once it is stopped, starts again after,
//    and stops at once when given nothing;
//  - the cache file's layout: entries written, rewritten and deleted read
//    back after it is closed and opened again, writes after the reopen
//    land on nothing still in use (the file table included), and the file
//    reuses the space rewritten and deleted entries leave; and random
//    writes, rewrites and deletes read back as written;
//  - the Vulkan pipeline cache's file: MappedFile gives a file's bytes (none
//    for a missing or empty one), and ReplaceFileContents puts new contents
//    in its place whole, leaving nothing beside it, or leaves it as it was;
//  - the graphic packs compiled in (tools/pack_graphic_packs.c, built here
//    as the core's build does) unpack to the files they were made from.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "util/helpers/Deflate.h"
#include "Cafe/OS/libs/zlib125/zlib125_stream.h"
#include "Cafe/HW/Latte/Common/ShaderSerializer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"

#include "Cemu/FileCache/FileCache.h"
#include "util/helpers/MappedFile.h"

#include "golden_deflate.h"
#include "golden_entries.h"

static int s_failures = 0;
#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			s_failures++; \
		} \
	} while (0)

// what golden_deflate.h's streams were made of
static std::vector<uint8_t> TestText(size_t n)
{
	static const char* words[] = {"the", "vertex", "shader", "reads", "the", "texture", "through", "a", "sampler", "and", "writes", "the", "export"};
	std::vector<uint8_t> out;
	uint32_t s = 12345;
	while (out.size() < n)
	{
		s = s * 1103515245u + 12345u;
		const char* w = words[(s >> 8) % 13];
		out.insert(out.end(), w, w + std::strlen(w));
		out.push_back(' ');
		if ((s >> 20) % 7 == 0)
			out.push_back((uint8_t)(s >> 3));
	}
	out.resize(n);
	return out;
}

static std::vector<uint8_t> Noise(size_t n, uint32_t seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(n);
	for (auto& b : v)
		b = (uint8_t)rng();
	return v;
}

static void TestDeflate()
{
	const std::vector<uint8_t> plain = TestText(s_plainSize);

	CHECK(Deflate::Adler32(1, (const uint8_t*)"Wikipedia", 9) == 0x11E60398);
	CHECK(Deflate::Crc32(0, (const uint8_t*)"123456789", 9) == 0xCBF43926);
	CHECK(Deflate::Adler32(1, plain.data(), plain.size()) == s_plainAdler);
	CHECK(Deflate::Crc32(0, plain.data(), plain.size()) == s_plainCrc);

	// zlib's streams
	struct
	{
		const uint8_t* data;
		size_t size;
		int windowBits;
	} golden[] = {
		{s_zlib9, sizeof(s_zlib9), 15},
		{s_gzip, sizeof(s_gzip), 31},
		{s_gzip, sizeof(s_gzip), 47},
		{s_zlib9, sizeof(s_zlib9), 47},
		{s_raw, sizeof(s_raw), -15},
		{s_window9, sizeof(s_window9), 15},
		{s_flushed, sizeof(s_flushed), 15},
	};
	for (auto& g : golden)
	{
		std::vector<uint8_t> out(plain.size());
		size_t outLen = out.size(), inLen = g.size;
		CHECK(Deflate::Uncompress(out.data(), outLen, g.data, inLen, g.windowBits) == Deflate::Result::Ok);
		CHECK(outLen == plain.size() && inLen == g.size && out == plain);
		// a byte short of room, and a stream cut short
		outLen = out.size() - 1;
		inLen = g.size;
		CHECK(Deflate::Uncompress(out.data(), outLen, g.data, inLen, g.windowBits) == Deflate::Result::BufferTooSmall);
		outLen = out.size() + 10;
		out.resize(outLen);
		inLen = g.size - 5;
		CHECK(Deflate::Uncompress(out.data(), outLen, g.data, inLen, g.windowBits) == Deflate::Result::DataError);
	}
	{
		// a flipped bit in the trailer
		std::vector<uint8_t> bad(s_zlib9, s_zlib9 + sizeof(s_zlib9));
		bad.back() ^= 1;
		std::vector<uint8_t> out(plain.size());
		size_t outLen = out.size(), inLen = bad.size();
		CHECK(Deflate::Uncompress(out.data(), outLen, bad.data(), inLen) == Deflate::Result::DataError);
	}

	// its own, of text and of noise, at every level, which zlib's bound holds
	for (size_t n : {(size_t)0, (size_t)1, (size_t)100, (size_t)5000, (size_t)70000, (size_t)300000})
	{
		for (int pass = 0; pass < 2; pass++)
		{
			std::vector<uint8_t> src = pass ? Noise(n, (uint32_t)n) : TestText(n);
			for (int level = 0; level <= 9; level += 3)
			{
				std::vector<uint8_t> z(Deflate::CompressBound(n));
				size_t zLen = z.size();
				CHECK(Deflate::Compress(z.data(), zLen, src.data(), n, level));
				CHECK(zLen <= Deflate::ZlibCompressBound(n));
				CHECK(zLen >= 6 && z[0] == 0x78 && ((z[0] << 8) | z[1]) % 31 == 0);
				uint32_t adler = Deflate::Adler32(1, src.data(), n);
				CHECK(z[zLen - 4] == (uint8_t)(adler >> 24) && z[zLen - 1] == (uint8_t)adler);
				std::vector<uint8_t> out(n + 1);
				size_t outLen = out.size(), inLen = zLen;
				CHECK(Deflate::Uncompress(out.data(), outLen, z.data(), inLen) == Deflate::Result::Ok);
				CHECK(outLen == n && inLen == zLen && (!n || std::memcmp(out.data(), src.data(), n) == 0));
				// too little room to write it in
				if (n > 100)
				{
					zLen = n / 50;
					CHECK(!Deflate::Compress(z.data(), zLen, src.data(), n, level) || pass == 0);
				}
			}
		}
	}
}

// a guest z_stream's fields, moved a byte at a time
static int Pump(zlib125::HostStream* s, zlib125::StreamFields& f, const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, size_t inStep, size_t outStep, int flush)
{
	size_t inPos = 0;
	uint8_t buf[64];
	for (int guard = 0; guard < 10000000; guard++)
	{
		size_t give = std::min(inStep, inLen - inPos);
		f.nextIn = in + inPos;
		f.availIn = (uint32_t)give;
		f.nextOut = buf;
		f.availOut = (uint32_t)std::min(outStep, sizeof(buf));
		int lastFlush = (flush == zlib125::Z_FINISH && inPos + give == inLen) ? zlib125::Z_FINISH : zlib125::Z_NO_FLUSH;
		int r = s->IsDeflate() ? s->Deflate(f, lastFlush) : s->Inflate(f, lastFlush);
		inPos += give - f.availIn;
		out.insert(out.end(), buf, (uint8_t*)f.nextOut);
		if (r == zlib125::Z_STREAM_END || r < 0 && r != zlib125::Z_BUF_ERROR)
			return r;
		if (r == zlib125::Z_BUF_ERROR && inPos == inLen && f.nextOut == buf)
			return r; // nothing more will come
	}
	return -100;
}

static void TestZlib125()
{
	using namespace zlib125;
	const std::vector<uint8_t> plain = TestText(s_plainSize);
	int r;

	CHECK(!HostStream::CreateInflate(7, &r) && r == Z_STREAM_ERROR);
	CHECK(!HostStream::CreateInflate(-16, &r) && r == Z_STREAM_ERROR);
	CHECK(!HostStream::CreateDeflate(10, Z_DEFLATED, 15, &r) && r == Z_STREAM_ERROR);
	CHECK(!HostStream::CreateDeflate(6, 9, 15, &r) && r == Z_STREAM_ERROR);
	CHECK(!HostStream::CreateDeflate(6, Z_DEFLATED, 47, &r) && r == Z_STREAM_ERROR);

	// inflate, a byte in and a byte out at a time, every container
	struct
	{
		const uint8_t* data;
		size_t size;
		int windowBits;
		uint32_t adler;
	} golden[] = {
		{s_zlib9, sizeof(s_zlib9), 15, s_plainAdler},
		{s_zlib9, sizeof(s_zlib9), 0, s_plainAdler},
		{s_gzip, sizeof(s_gzip), 31, s_plainCrc},
		{s_gzip, sizeof(s_gzip), 47, s_plainCrc},
		{s_raw, sizeof(s_raw), -15, 1},
		{s_window9, sizeof(s_window9), 9, s_plainAdler},
		{s_flushed, sizeof(s_flushed), 15, s_plainAdler},
	};
	for (auto& g : golden)
	{
		for (size_t step : {(size_t)1, (size_t)7, (size_t)4096})
		{
			HostStream* s = HostStream::CreateInflate(g.windowBits, &r);
			CHECK(s && r == Z_OK);
			if (!s)
				continue;
			StreamFields f = {};
			s->Reset(f);
			CHECK(f.adler == 1 && f.totalIn == 0 && f.totalOut == 0);
			std::vector<uint8_t> out;
			CHECK(Pump(s, f, g.data, g.size, out, step, step, Z_NO_FLUSH) == Z_STREAM_END);
			CHECK(out == plain);
			CHECK(f.totalIn == g.size && f.totalOut == plain.size());
			CHECK(f.adler == g.adler);
			// once ended, it says so again
			f.availIn = 0;
			uint8_t b;
			f.nextOut = &b;
			f.availOut = 1;
			CHECK(s->Inflate(f, Z_NO_FLUSH) == Z_STREAM_END);
			// and a reset reads the stream again
			CHECK(s->InflateReset(f) == Z_OK && f.totalIn == 0 && f.totalOut == 0);
			out.clear();
			CHECK(Pump(s, f, g.data, g.size, out, 4096, 64, Z_NO_FLUSH) == Z_STREAM_END && out == plain);
			delete s;
		}
	}
	{
		// no progress, Z_FINISH short of the end, and a bad stream
		HostStream* s = HostStream::CreateInflate(15, &r);
		StreamFields f = {};
		s->Reset(f);
		uint8_t out[16];
		f.nextIn = s_zlib9;
		f.availIn = 0;
		f.nextOut = out;
		f.availOut = sizeof(out);
		CHECK(s->Inflate(f, Z_NO_FLUSH) == Z_BUF_ERROR);
		f.availIn = sizeof(s_zlib9);
		CHECK(s->Inflate(f, Z_FINISH) == Z_BUF_ERROR);
		CHECK(f.totalOut == sizeof(out));
		f.nextOut = nullptr;
		CHECK(s->Inflate(f, Z_NO_FLUSH) == Z_STREAM_ERROR);
		CHECK(s->InflateReset(f, 15) == Z_OK);
		std::vector<uint8_t> bad(s_zlib9, s_zlib9 + sizeof(s_zlib9));
		bad[0] = 0x79;
		std::vector<uint8_t> o;
		CHECK(Pump(s, f, bad.data(), bad.size(), o, 64, 64, Z_NO_FLUSH) == Z_DATA_ERROR);
		CHECK(s->InflateReset(f, 99) == Z_STREAM_ERROR);
		delete s;
	}

	// deflate, a byte at a time and in one go, back through inflate
	for (int windowBits : {15, 31, -15, 9})
	{
		for (size_t step : {(size_t)1, (size_t)13, (size_t)100000})
		{
			for (size_t n : {(size_t)0, (size_t)1, (size_t)5000, s_plainSize})
			{
				const std::vector<uint8_t> src = n == s_plainSize ? plain : TestText(n);
				HostStream* d = HostStream::CreateDeflate(Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, &r);
				CHECK(d && r == Z_OK);
				if (!d)
					continue;
				StreamFields f = {};
				d->Reset(f);
				CHECK(f.adler == (windowBits == 31 ? 0u : 1u));
				std::vector<uint8_t> z;
				CHECK(Pump(d, f, src.data(), src.size(), z, step, step, Z_FINISH) == Z_STREAM_END);
				CHECK(f.totalIn == n && f.totalOut == z.size());
				CHECK(z.size() <= d->DeflateBound((uint32_t)n));
				if (windowBits == 15)
					CHECK(f.adler == Deflate::Adler32(1, src.data(), n));
				if (windowBits == 31)
					CHECK(f.adler == Deflate::Crc32(0, src.data(), n));
				uint8_t b;
				f.nextIn = nullptr;
				f.availIn = 0;
				f.nextOut = &b;
				f.availOut = 1;
				CHECK(d->Deflate(f, Z_FINISH) == Z_STREAM_END);
				delete d;

				HostStream* i = HostStream::CreateInflate(windowBits, &r);
				StreamFields g = {};
				i->Reset(g);
				std::vector<uint8_t> back;
				CHECK(Pump(i, g, z.data(), z.size(), back, 4096, 64, Z_NO_FLUSH) == Z_STREAM_END);
				CHECK(back == src);
				delete i;
			}
		}
	}
	{
		// the flushes zlib would have emitted at are taken as none
		HostStream* d = HostStream::CreateDeflate(6, Z_DEFLATED, 15, &r);
		StreamFields f = {};
		d->Reset(f);
		std::vector<uint8_t> z(Deflate::ZlibCompressBound(plain.size()));
		f.nextIn = plain.data();
		f.availIn = 3000;
		f.nextOut = z.data();
		f.availOut = (uint32_t)z.size();
		CHECK(d->Deflate(f, Z_SYNC_FLUSH) == Z_OK && f.availIn == 0);
		f.availIn = (uint32_t)plain.size() - 3000;
		CHECK(d->Deflate(f, Z_FULL_FLUSH) == Z_OK);
		CHECK(d->Deflate(f, Z_FINISH) == Z_STREAM_END);
		CHECK(d->Deflate(f, Z_NO_FLUSH) == Z_STREAM_ERROR);
		std::vector<uint8_t> back(plain.size());
		size_t outLen = back.size(), inLen = f.totalOut;
		CHECK(Deflate::Uncompress(back.data(), outLen, z.data(), inLen) == Deflate::Result::Ok && back == plain);
		f.availOut = 0;
		delete d;
	}

	// the one-call functions
	{
		std::vector<uint8_t> z(CompressBound((uint32_t)plain.size()));
		uint32_t zLen = (uint32_t)z.size();
		CHECK(Compress(z.data(), &zLen, plain.data(), (uint32_t)plain.size(), Z_DEFAULT_COMPRESSION) == Z_OK);
		std::vector<uint8_t> back(plain.size());
		uint32_t outLen = (uint32_t)back.size();
		CHECK(Uncompress(back.data(), &outLen, z.data(), zLen) == Z_OK && outLen == plain.size() && back == plain);
		outLen = 100;
		CHECK(Uncompress(back.data(), &outLen, z.data(), zLen) == Z_BUF_ERROR && outLen == 100);
		// cut short: zlib's answer turns on whether the output had room left
		outLen = (uint32_t)back.size();
		CHECK(Uncompress(back.data(), &outLen, z.data(), zLen - 3) == Z_BUF_ERROR);
		back.resize(plain.size() + 1);
		outLen = (uint32_t)back.size();
		CHECK(Uncompress(back.data(), &outLen, z.data(), zLen - 3) == Z_DATA_ERROR);
		zLen = 10;
		CHECK(Compress(z.data(), &zLen, plain.data(), (uint32_t)plain.size(), 6) == Z_BUF_ERROR);
		CHECK(CompressBound(1000) == 1000 + 13);
		CHECK(Crc32(1234, nullptr, 10) == 0);
		CHECK(Crc32(0, plain.data(), (uint32_t)plain.size()) == s_plainCrc);
	}
}

// golden_entries.h's shader program and register state
static std::vector<uint8_t> TestProgram()
{
	std::vector<uint8_t> p(2048);
	uint32_t s = 0x1234;
	for (size_t i = 0; i < p.size(); i += 4)
	{
		s = s * 1103515245u + 12345u;
		uint32_t w = (i % 32 < 16) ? (uint32_t)(i * 0x01000193u) : (s >> 8) & 0x00FF00FF;
		p[i] = (uint8_t)(w >> 24);
		p[i + 1] = (uint8_t)(w >> 16);
		p[i + 2] = (uint8_t)(w >> 8);
		p[i + 3] = (uint8_t)w;
	}
	return p;
}

static uint32_t TestRegister(int i)
{
	return ((0x1037778e >> ((i / 3) & 31)) & 1) ? (uint32_t)(i + i * 17 + i * 23) : 0;
}

static void TestCacheEntries()
{
	// standalone Cemu's
	{
		std::vector<uint8_t> prog;
		MemStreamReader reader(s_shaderEntryV1, sizeof(s_shaderEntryV1));
		CHECK(Latte::DeserializeShaderProgram(prog, reader));
		CHECK(prog == TestProgram());
	}
	{
		static Latte::GPUCompactedRegisterState state;
		MemStreamReader reader(s_registerEntryV1, sizeof(s_registerEntryV1));
		CHECK(Latte::DeserializeRegisterState(state, reader));
		bool same = true;
		for (int i = 0; i < Latte::GPUCompactedRegisterState::NUM_REGS; i++)
			same &= state.rawArray[i] == TestRegister(i);
		CHECK(same);
	}
	// a corrupted entry is refused, not decoded to something
	{
		std::vector<uint8_t> bad(s_shaderEntryV1, s_shaderEntryV1 + sizeof(s_shaderEntryV1));
		bad[bad.size() / 2] ^= 0x55;
		std::vector<uint8_t> prog;
		MemStreamReader reader(bad.data(), (sint32)bad.size());
		CHECK(!Latte::DeserializeShaderProgram(prog, reader) || prog != TestProgram());
	}
	// the core's own, and back; smaller than without the dictionary
	for (size_t n : {(size_t)4, (size_t)64, (size_t)2048, (size_t)30000})
	{
		std::vector<uint8_t> prog = n == 2048 ? TestProgram() : Noise(n, 9);
		MemStreamWriter writer(0);
		Latte::SerializeShaderProgram(prog.data(), (uint32)prog.size(), writer);
		auto bytes = writer.getResult();
		CHECK(bytes[0] == 1);
		std::vector<uint8_t> back;
		MemStreamReader reader(bytes.data(), (sint32)bytes.size());
		CHECK(Latte::DeserializeShaderProgram(back, reader) && back == prog);
		if (n == 2048)
			CHECK(bytes.size() < sizeof(s_shaderEntryV1) * 5 / 4);
	}
	{
		static Latte::GPUCompactedRegisterState in, out;
		for (int i = 0; i < Latte::GPUCompactedRegisterState::NUM_REGS; i++)
			in.rawArray[i] = (i % 5 == 0) ? (uint32_t)i * 0x9E3779B9u : 0;
		MemStreamWriter writer(0);
		Latte::SerializeRegisterState(in, writer);
		auto bytes = writer.getResult();
		MemStreamReader reader(bytes.data(), (sint32)bytes.size());
		CHECK(Latte::DeserializeRegisterState(out, reader));
		CHECK(std::memcmp(&in, &out, sizeof(in)) == 0);
	}
}

static std::vector<uint8_t> EntryData(int thread, int i)
{
	std::vector<uint8_t> d = TestText(40 + (i * 37) % 3000);
	d[0] = (uint8_t)thread;
	d[1] = (uint8_t)i;
	return d;
}

static void TestFileCacheWriter()
{
	const char* path = "filecache_test.bin";
	std::remove(path);
	constexpr int THREADS = 4, PER_THREAD = 150;
	for (int round = 0; round < 2; round++)
	{
		// the second round reopens what the first wrote, and restarts the
		// writer the first stopped
		FileCache* cache = round ? FileCache::Open(path, false, 7) : FileCache::Create(path, 7);
		CHECK(cache);
		if (!cache)
			return;
		std::vector<std::thread> threads;
		for (int t = 0; t < THREADS; t++)
			threads.emplace_back([cache, t, round] {
				for (int i = 0; i < PER_THREAD; i++)
				{
					std::vector<uint8_t> d = EntryData(t, i);
					cache->AddFileAsync({(uint64)(round * 100 + t + 1), (uint64)i + 1}, d.data(), (sint32)d.size());
				}
			});
		for (auto& t : threads)
			t.join();
		// everything queued before the stop is written by the time it returns
		FileCache_StopAsyncWriter();
		bool all = true;
		for (int r = 0; r <= round; r++)
			for (int t = 0; t < THREADS; t++)
				for (int i = 0; i < PER_THREAD; i++)
				{
					std::vector<uint8_t> got;
					all &= cache->GetFile({(uint64)(r * 100 + t + 1), (uint64)i + 1}, got) && got == EntryData(t, i);
				}
		CHECK(all);
		CHECK(cache->GetFileCount() == (round + 1) * THREADS * PER_THREAD);
		delete cache;
	}
	// nothing queued: stops at once, and again
	FileCache_StopAsyncWriter();
	FileCache_StopAsyncWriter();
	std::remove(path);
}

static std::vector<uint8_t> LayoutData(int i, int version)
{
	std::vector<uint8_t> d = Noise(16 + (i * 97 + version * 31) % 2500, (uint32_t)(i * 7 + version));
	return d;
}

static long long FileSizeOf(const char* path)
{
	FILE* f = std::fopen(path, "rb");
	if (!f)
		return -1;
	std::fseek(f, 0, SEEK_END);
	long long n = std::ftell(f);
	std::fclose(f);
	return n;
}

static void TestFileCacheLayout()
{
	const char* path = "filecache_layout.bin";
	std::remove(path);
	constexpr int N = 400;
	// what each name holds: -1 deleted, else the version written last
	std::vector<int> version(2 * N + 1, -1);
	auto verify = [&](FileCache* cache) {
		bool all = true;
		int live = 0;
		for (int i = 1; i <= 2 * N; i++)
		{
			std::vector<uint8_t> got;
			bool has = cache->GetFile({(uint64)i, 3}, got);
			if (version[i] < 0)
				all &= !has && !cache->HasFile({(uint64)i, 3});
			else
			{
				all &= has && got == LayoutData(i, version[i]) && cache->HasFile({(uint64)i, 3});
				live++;
			}
		}
		CHECK(all);
		CHECK(cache->GetFileCount() == live);
	};
	FileCache* cache = FileCache::Create(path, 9);
	CHECK(cache);
	if (!cache)
		return;
	// stored as written, so the sizes below are known
	cache->UseCompression(false);
	for (int i = 1; i <= N; i++)
	{
		std::vector<uint8_t> d = LayoutData(i, 0);
		cache->AddFile({(uint64)i, 3}, d.data(), (sint32)d.size());
		version[i] = 0;
	}
	for (int i = 1; i <= N; i += 3)
	{
		std::vector<uint8_t> d = LayoutData(i, 1);
		cache->AddFile({(uint64)i, 3}, d.data(), (sint32)d.size());
		version[i] = 1;
	}
	for (int i = 2; i <= N; i += 5)
	{
		CHECK(cache->DeleteFile({(uint64)i, 3}));
		version[i] = -1;
	}
	CHECK(!cache->DeleteFile({(uint64)N + 1, 3}));
	verify(cache);
	delete cache;
	for (int round = 0; round < 2; round++)
	{
		cache = FileCache::Open(path, false, 9);
		CHECK(cache);
		if (!cache)
			return;
		cache->UseCompression(false);
		verify(cache);
		if (round == 0)
		{
			// new names after the reopen, and rewrites of old ones: none of
			// it may land on an entry or the file table still in use
			for (int i = N + 1; i <= 2 * N; i++)
			{
				std::vector<uint8_t> d = LayoutData(i, 0);
				cache->AddFile({(uint64)i, 3}, d.data(), (sint32)d.size());
				version[i] = 0;
			}
			for (int i = 5; i <= 2 * N; i += 7)
			{
				std::vector<uint8_t> d = LayoutData(i, 2);
				cache->AddFile({(uint64)i, 3}, d.data(), (sint32)d.size());
				version[i] = 2;
			}
			verify(cache);
		}
		delete cache;
	}
	// the space rewritten and deleted entries left is used again: the file
	// stays within twice what is live in it
	long long liveBytes = 0;
	for (int i = 1; i <= 2 * N; i++)
		if (version[i] >= 0)
			liveBytes += (long long)LayoutData(i, version[i]).size() + 0x20;
	long long fileSize = FileSizeOf(path);
	CHECK(fileSize > liveBytes && fileSize < 2 * liveBytes);
	std::remove(path);
	// random writes, rewrites and deletes over few names (names that collide
	// in the lookup, and gaps that join and split), checked against what was
	// written, through reopens
	std::vector<int> model(64, -1);
	std::mt19937 rng(5);
	cache = FileCache::Create(path, 9);
	CHECK(cache);
	if (!cache)
		return;
	bool all = true;
	for (int op = 0; op < 6000 && cache; op++)
	{
		int i = (int)(rng() % model.size());
		uint64 n1 = (uint64)(i % 8) << 40 | (uint64)(i / 8), n2 = 0x51;
		if (rng() % 3 == 0)
		{
			all &= cache->DeleteFile({n1, n2}) == (model[i] >= 0);
			model[i] = -1;
		}
		else
		{
			std::vector<uint8_t> d = LayoutData(i, op);
			cache->AddFile({n1, n2}, d.data(), (sint32)d.size());
			model[i] = op;
		}
		if (op % 500 == 499)
		{
			delete cache;
			cache = FileCache::Open(path, false, 9);
			CHECK(cache);
		}
		if (cache && op % 97 == 0)
		{
			int live = 0;
			for (int k = 0; k < (int)model.size(); k++)
			{
				std::vector<uint8_t> got;
				uint64 k1 = (uint64)(k % 8) << 40 | (uint64)(k / 8);
				bool has = cache->GetFile({k1, n2}, got);
				all &= model[k] < 0 ? !has : has && got == LayoutData(k, model[k]);
				live += model[k] >= 0;
			}
			all &= cache->GetFileCount() == live;
		}
	}
	CHECK(all);
	delete cache;
	std::remove(path);
}

static void TestMappedFile()
{
	const fs::path path = "mapped_test.bin";
	std::error_code ec;
	fs::remove(path, ec);
	{
		MappedFile none(path);
		CHECK(none.size() == 0 && none.data() == nullptr);
	}
	std::vector<uint8_t> a = Noise(300000, 11), b = Noise(1234, 12);
	CHECK(ReplaceFileContents(path, a.data(), a.size()));
	CHECK(!fs::exists(fs::path("mapped_test.bin.tmp")));
	{
		MappedFile f(path);
		CHECK(f.mapped());
		CHECK(f.size() == a.size() && std::memcmp(f.data(), a.data(), a.size()) == 0);
	}
	// shorter contents replace longer ones whole
	CHECK(ReplaceFileContents(path, b.data(), b.size()));
	{
		MappedFile f(path);
		CHECK(f.size() == b.size() && std::memcmp(f.data(), b.data(), b.size()) == 0);
	}
	CHECK(fs::file_size(path, ec) == b.size());
	// an empty file maps to nothing
	CHECK(ReplaceFileContents(path, nullptr, 0));
	{
		MappedFile f(path);
		CHECK(f.size() == 0);
	}
	// where nothing can be written: false, and nothing left behind
	const fs::path bad = fs::path("no_such_dir_for_mapped_test") / "x.bin";
	CHECK(!ReplaceFileContents(bad, a.data(), a.size()));
	CHECK(!fs::exists(bad.parent_path()));
	fs::remove(path, ec);
}

// tools/pack_graphic_packs.c writes these
extern const unsigned char g_libretro_graphic_packs[];
extern const size_t g_libretro_graphic_packs_size;
extern const size_t g_libretro_graphic_packs_unpacked_size;

static void TestGraphicPacks(const char* fixture)
{
	std::vector<uint8_t> archive(g_libretro_graphic_packs_unpacked_size);
	size_t archiveLen = archive.size(), packedLen = g_libretro_graphic_packs_size;
	CHECK(Deflate::Uncompress(archive.data(), archiveLen, g_libretro_graphic_packs, packedLen) == Deflate::Result::Ok);
	CHECK(archiveLen == archive.size() && packedLen == g_libretro_graphic_packs_size);
	int files = 0;
	size_t pos = 0;
	while (pos < archiveLen)
	{
		std::string name((const char*)archive.data() + pos);
		pos += name.size() + 1;
		if (pos + 4 > archiveLen)
			break;
		uint32_t size = archive[pos] | (archive[pos + 1] << 8) | (archive[pos + 2] << 16) | ((uint32_t)archive[pos + 3] << 24);
		pos += 4;
		CHECK(size <= archiveLen - pos);
		// the fixture's groups are all named "G<n>" in the test's tree
		std::string path = std::string(fixture) + "/" + (name == "LICENSE.md" ? name : "Enhancements/" + name);
		FILE* f = std::fopen(path.c_str(), "rb");
		CHECK(f);
		if (f)
		{
			std::vector<uint8_t> disk(size + 1);
			size_t got = std::fread(disk.data(), 1, disk.size(), f);
			std::fclose(f);
			CHECK(got == size && std::memcmp(disk.data(), archive.data() + pos, size) == 0);
		}
		pos += size;
		files++;
	}
	CHECK(pos == archiveLen && files == 4);
}

int main(int argc, char** argv)
{
	TestDeflate();
	TestZlib125();
	TestCacheEntries();
	TestFileCacheWriter();
	TestFileCacheLayout();
	TestMappedFile();
	if (argc > 1)
		TestGraphicPacks(argv[1]);
	if (s_failures)
	{
		std::printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	std::printf("ok: zlib and zstd through libretro-common\n");
	return 0;
}
