// The core's FST volume reader over a volume made here, in a source that
// makes its octets as they are asked for, so a cluster can be larger than
// anything the test holds:
//  - files in an unhashed cluster (CBC over the whole cluster, each block's
//    IV the last 16 octets of the one before) and in a hashed one (64 KiB
//    blocks, hashes then data, each block's data checked against its H0
//    hash) read back whole, in pieces and at random places, from a source
//    that lends its octets and one that does not;
//  - a file beyond 4 GiB of its cluster reads back: its place is not cut
//    to 32 bits;
//  - a read does not run past its file's end, in either kind of cluster;
//  - once the first read has made the block cache, reads allocate nothing:
//    blocks are decrypted into the cache's slots, or straight into the
//    destination for a read of more than one whole block, which then reads
//    the source once per block where a cached block reads it not at all;
//  - a raw block's IV comes from the source when the block before is not at
//    hand, lent or read;
//  - a block whose data is not its H0 hash's fails the read;
//  - threads read one volume at once, each with a block cache of its own
//    lent from the volume's pool, from a source that lends and one that
//    does not (run under ThreadSanitizer by CI).
#include "Cafe/Filesystem/FST/FST.h"
#include <lrc_hash.h>
#include "test_source.h"
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "Cemu/Logging/CemuLogging.h"
#include "util/crypto/aes128.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <random>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- what FST.cpp takes from the rest of the core -----------------------------

uint64 s_loggingFlagMask = 0;
bool cemuLog_log(LogType type, std::string_view text) { return false; }
bool NCrypto::TMDParser::parse(const uint8* data, size_t size) { return false; }
bool NCrypto::ETicketParser::parse(const uint8* data, size_t size) { return false; }
void NCrypto::ETicketParser::GetTitleKey(AesKey& key) {}
void KeyCache_Prepare(bool reload) {}
bool KeyCache_FindDiscKey(const fs::path& image, const uint8* header48, uint8* keyOut, KeyCacheSearch* search) { return false; }

// allocations counted, to see a read make none
static std::atomic<uint64> s_allocs{0};
void* operator new(size_t n)
{
	s_allocs++;
	if (void* p = std::malloc(n ? n : 1))
		return p;
	throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { s_allocs++; return std::malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { s_allocs++; return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

// --- the volume -------------------------------------------------------------

static const uint8 KEY[16] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90};
static constexpr uint32 SECTOR = 0x8000;
static constexpr uint32 HBLOCK = 0x10000, HHASH = 0x400, HDATA = 0xFC00;
static constexpr uint32 OFFSET_FACTOR = 0x20;
// clusters' first sectors: the FST, an unhashed one, a hashed one
static constexpr uint32 FST_SECTOR = 0, RAW_SECTOR = 4, HASHED_SECTOR = 16;

static uint64 Mix(uint64 x)
{
	x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
	return x;
}

// an unhashed cluster's octets as stored: made up, the plain text what
// decrypting them gives
static void RawCipher(uint64 at, uint8* out, size_t n)
{
	for (size_t i = 0; i < n; i++)
		out[i] = (uint8)(Mix((at + i) / 8 + 0x1234) >> (8 * ((at + i) % 8)));
}

static void RawPlain(uint64 at, uint8* out, size_t n)
{
	// block by block: CBC with the block before's last 16 octets as the IV
	while (n > 0)
	{
		const uint64 block = at / SECTOR;
		uint8 cipher[SECTOR], plain[SECTOR], iv[16]{};
		RawCipher(block * SECTOR, cipher, SECTOR);
		if (block == 0)
			iv[1] = 1; // the cluster's index, 1
		else
			RawCipher(block * SECTOR - 16, iv, 16);
		AES128_CBC_decrypt(plain, cipher, SECTOR, KEY, iv);
		const size_t off = (size_t)(at % SECTOR), take = std::min<size_t>(n, SECTOR - off);
		memcpy(out, plain + off, take);
		out += take, at += take, n -= take;
	}
}

// a hashed cluster's block: its data made up, its H0 hash of the data, both
// encrypted as the console has them
static void HashedPlainData(uint64 block, uint8* out)
{
	for (uint32 i = 0; i < HDATA; i++)
		out[i] = (uint8)Mix(block * HDATA + i + 99);
}

static void HashedBlockCipher(uint64 block, uint8* out, bool corrupt)
{
	uint8 hashes[HHASH]{}, data[HDATA];
	HashedPlainData(block, data);
	for (uint32 k = 0; k < 16; k++)
		hashes[20 * k] = (uint8)(k + 1);
	SHA1Digest(data, HDATA, hashes + 20 * (block % 16));
	if (corrupt)
		data[100] ^= 1;
	const uint8 zero[16]{};
	uint8 iv[16];
	memcpy(iv, hashes + 20 * (block % 16), 16);
	AES128_CBC_encrypt(out, hashes, HHASH, KEY, zero);
	AES128_CBC_encrypt(out + HHASH, data, HDATA, KEY, iv);
}

// The disc: the FST as a vector, the clusters made as they are read. Lends
// what it holds when lend is set, and counts what it reads.
class MadeSource : public TestSource
{
public:
	std::vector<uint8> fst;
	bool lend = false;
	uint64 corruptBlock = ~0ull;
	uint64 reads = 0;

	void SetCorrupt(uint64 block)
	{
		corruptBlock = block;
		m_madeBlock = ~0ull;
	}

	uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) override
	{
		reads++;
		uint8* out = (uint8*)data;
		const uint64 at = clusterOffset + offset;
		if (clusterIndex == 0)
		{
			if (at >= fst.size())
				return 0;
			const uint64 n = std::min<uint64>(size, fst.size() - at);
			memcpy(out, fst.data() + at, n);
			return n;
		}
		if (clusterIndex == 1)
		{
			RawCipher(offset, out, size);
			return size;
		}
		for (uint64 done = 0; done < size;)
		{
			const uint64 pos = offset + done, block = pos / HBLOCK;
			if (block != m_madeBlock)
			{
				HashedBlockCipher(block, m_block, block == corruptBlock);
				m_madeBlock = block;
			}
			const uint64 take = std::min<uint64>(size - done, HBLOCK - pos % HBLOCK);
			memcpy(out + done, m_block + pos % HBLOCK, take);
			done += take;
		}
		return size;
	}

	const uint8* Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size) override
	{
		// what is lent lives as long as the source: here, until the next lend
		if (!lend)
			return nullptr;
		if (clusterIndex == 1 && size <= sizeof(m_raw))
		{
			RawCipher(offset, m_raw, size);
			return m_raw;
		}
		if (clusterIndex != 2 || offset / HBLOCK != (offset + size - 1) / HBLOCK)
			return nullptr;
		const uint64 block = offset / HBLOCK;
		if (block != m_madeBlock)
		{
			HashedBlockCipher(block, m_block, block == corruptBlock);
			m_madeBlock = block;
		}
		return m_block + offset % HBLOCK;
	}

private:
	uint8 m_block[HBLOCK];
	uint8 m_raw[SECTOR];
	uint64 m_madeBlock = ~0ull;
};

struct FileDef
{
	const char* name;
	uint16 cluster;
	uint64 offset; // in its cluster's octets (hashed: data octets)
	uint32 size;
};

static const uint64 FAR_OFFSET = 0x123456780ull; // past 4 GiB
static const FileDef FILES[] = {
	{"raw/a.bin", 1, 0, 5 * SECTOR + 123},
	{"raw/b.bin", 1, 6 * SECTOR + 0x40, 3 * SECTOR},
	{"raw/far.bin", 1, FAR_OFFSET, 2 * SECTOR + 7},
	{"h/c.bin", 2, 0, 3 * HDATA + 77},
	{"h/d.bin", 2, 5 * HDATA + 0x20, 40000},
	{"h/far.bin", 2, FAR_OFFSET, 2 * HDATA + 5},
};

static void Be32(std::vector<uint8>& v, size_t at, uint32 x)
{
	for (int i = 0; i < 4; i++)
		v[at + i] = (uint8)(x >> (24 - 8 * i));
}

static std::vector<uint8> MakeFst()
{
	// root, raw/ with three files, h/ with three
	const size_t numEntries = 9;
	std::vector<uint8> plain(0x20 + 3 * 0x20 + numEntries * 0x10);
	std::string names(1, '\0');
	auto name = [&](const char* n) { const uint32 at = (uint32)names.size(); names += n; names += '\0'; return at; };
	Be32(plain, 0, 0x46535400);
	Be32(plain, 4, OFFSET_FACTOR);
	Be32(plain, 8, 3);
	const uint32 clusterSector[3] = {FST_SECTOR, RAW_SECTOR, HASHED_SECTOR};
	for (int c = 0; c < 3; c++)
	{
		Be32(plain, 0x20 + 0x20 * c, clusterSector[c]);
		plain[0x20 + 0x20 * c + 0x14] = c == 2 ? 2 : 0; // hash mode
	}
	size_t e = 0x20 + 3 * 0x20;
	auto entry = [&](uint8 type, uint32 nameAt, uint32 offset, uint32 size, uint16 cluster) {
		Be32(plain, e, ((uint32)type << 24) | nameAt);
		Be32(plain, e + 4, offset);
		Be32(plain, e + 8, size);
		plain[e + 14] = (uint8)(cluster >> 8);
		plain[e + 15] = (uint8)cluster;
		e += 0x10;
	};
	entry(1, 0, 0, (uint32)numEntries, 0);
	entry(1, name("raw"), 0, 5, 0);
	for (int i = 0; i < 3; i++)
		entry(0, name(strchr(FILES[i].name, '/') + 1), (uint32)(FILES[i].offset / OFFSET_FACTOR), FILES[i].size, FILES[i].cluster);
	entry(1, name("h"), 0, 9, 0);
	for (int i = 3; i < 6; i++)
		entry(0, name(strchr(FILES[i].name, '/') + 1), (uint32)(FILES[i].offset / OFFSET_FACTOR), FILES[i].size, FILES[i].cluster);
	plain.insert(plain.end(), names.begin(), names.end());
	plain.resize((plain.size() + 15) & ~(size_t)15);
	std::vector<uint8> cipher(plain.size());
	const uint8 zero[16]{};
	AES128_CBC_encrypt(cipher.data(), plain.data(), (uint32)plain.size(), KEY, zero);
	return cipher;
}

static void Expected(const FileDef& f, uint64 at, uint8* out, size_t n)
{
	if (f.cluster == 1)
	{
		RawPlain(f.offset + at, out, n);
		return;
	}
	uint8 data[HDATA];
	for (uint64 pos = f.offset + at; n > 0;)
	{
		HashedPlainData(pos / HDATA, data);
		const size_t take = std::min<size_t>(n, HDATA - pos % HDATA);
		memcpy(out, data + pos % HDATA, take);
		out += take, pos += take, n -= take;
	}
}

static std::unique_ptr<FSTVolume> Open(bool lend, MadeSource*& source)
{
	std::unique_ptr<MadeSource> src(new MadeSource());
	src->fst = MakeFst();
	src->lend = lend;
	source = src.get();
	NCrypto::AesKey key;
	memcpy(key.b, KEY, 16);
	const uint32 fstSize = (uint32)source->fst.size();
	return std::unique_ptr<FSTVolume>(FSTVolume::OpenFromDataSource(AsSource(src.release(), true), 0, fstSize, key));
}

// A block in the cache is not read again; whole blocks in a read of more than
// one are read once each and not kept, the pieces at its ends kept
static void TestCache()
{
	MadeSource* source;
	std::unique_ptr<FSTVolume> vol = Open(false, source);
	CHECK(vol != nullptr);
	if (!vol)
		return;
	std::vector<uint8> got(4 * HBLOCK), want(4 * HBLOCK);
	FSTFileHandle b;
	CHECK(vol->OpenFile("raw/b.bin", b, true));
	Expected(FILES[1], 0, want.data(), FILES[1].size);
	// raw/b.bin: 0x40 into block 6, then 7 and 8 whole, then into 9
	CHECK(vol->ReadFile(b, 0, FILES[1].size, got.data()) == FILES[1].size && memcmp(got.data(), want.data(), FILES[1].size) == 0);
	uint64 reads = source->reads;
	CHECK(vol->ReadFile(b, 10, 20, got.data()) == 20 && source->reads == reads);          // block 6: kept
	CHECK(vol->ReadFile(b, FILES[1].size - 30, 20, got.data()) == 20 && source->reads == reads); // block 9: kept
	CHECK(vol->ReadFile(b, SECTOR + 10, 20, got.data()) == 20 && source->reads > reads);  // block 7: not kept
	CHECK(memcmp(got.data(), want.data() + SECTOR + 10, 20) == 0);
	reads = source->reads;
	CHECK(vol->ReadFile(b, SECTOR + 100, 20, got.data()) == 20 && source->reads == reads); // now kept
	// the same in a hashed cluster: h/c.bin from 0, blocks 0..2 whole, then 3
	FSTFileHandle c;
	CHECK(vol->OpenFile("h/c.bin", c, true));
	CHECK(vol->ReadFile(c, 0, FILES[3].size, got.data()) == FILES[3].size);
	reads = source->reads;
	CHECK(vol->ReadFile(c, 3 * HDATA + 1, 20, got.data()) == 20 && source->reads == reads); // block 3: kept
	CHECK(vol->ReadFile(c, HDATA + 1, 20, got.data()) == 20 && source->reads > reads);      // block 1: not kept
}

// A raw block's IV, the end of the block before, read or lent from the source
// when that block is not at hand
static void TestIvFromSource(bool lend)
{
	MadeSource* source;
	std::unique_ptr<FSTVolume> vol = Open(lend, source);
	CHECK(vol != nullptr);
	if (!vol)
		return;
	FSTFileHandle a;
	CHECK(vol->OpenFile("raw/a.bin", a, true));
	uint8 got[100], want[100];
	Expected(FILES[0], 2 * SECTOR, want, sizeof(want));
	CHECK(vol->ReadFile(a, 2 * SECTOR, sizeof(got), got) == sizeof(got) && memcmp(got, want, sizeof(got)) == 0);
}

static void TestVolume(bool lend)
{
	MadeSource* source;
	std::unique_ptr<FSTVolume> vol = Open(lend, source);
	CHECK(vol != nullptr);
	if (!vol)
		return;
	std::mt19937 rng(lend ? 5 : 6);
	std::vector<uint8> got(4 * HBLOCK), want(4 * HBLOCK);
	bool all = true;
	for (const FileDef& f : FILES)
	{
		FSTFileHandle h;
		if (!vol->OpenFile(f.name, h, true))
		{
			std::fprintf(stderr, "%s not found\n", f.name);
			all = false;
			continue;
		}
		all = all && vol->GetFileSize(h) == f.size;
		// whole, then past the end, then in pieces and at random places
		Expected(f, 0, want.data(), f.size);
		all = all && vol->ReadFile(h, 0, (uint32)got.size(), got.data()) == f.size && memcmp(got.data(), want.data(), f.size) == 0;
		all = all && vol->ReadFile(h, f.size - 3, 100, got.data()) == 3 && memcmp(got.data(), want.data() + f.size - 3, 3) == 0;
		all = all && vol->ReadFile(h, f.size, 1, got.data()) == 0;
		for (uint32 at = 0; at < f.size; at += 4099)
		{
			const uint32 n = std::min<uint32>(4099, f.size - at);
			all = all && vol->ReadFile(h, at, n, got.data()) == n && memcmp(got.data(), want.data() + at, n) == 0;
		}
		for (int i = 0; i < 40; i++)
		{
			const uint32 at = rng() % f.size, n = std::min<uint32>(1 + rng() % (3 * HBLOCK), f.size - at);
			all = all && vol->ReadFile(h, at, n, got.data()) == n && memcmp(got.data(), want.data() + at, n) == 0;
		}
	}
	CHECK(all);

	// entries through the table: paths, listings, what is not there
	{
		FSTFileHandle h;
		CHECK(vol->GetFileCount() == 6);
		CHECK(vol->OpenFile("H\\D.BIN", h, true) && vol->GetPath(h) == "h/d.bin" && vol->GetName(h) == "d.bin");
		CHECK(!vol->OpenFile("h", h, true) && vol->OpenFile("/h/", h, false) && vol->IsDirectory(h) && !vol->IsFile(h));
		CHECK(!vol->OpenFile("raw/c.bin", h, false) && !vol->OpenFile("raw/a.bin/x", h, false));
		FSTDirectoryIterator it;
		int listed = 0;
		CHECK(vol->OpenDirectoryIterator("raw", it));
		while (vol->Next(it, h))
			listed += vol->IsFile(h) && !vol->HasLinkFlag(h) ? 1 : 100;
		CHECK(listed == 3);
		CHECK(!vol->OpenDirectoryIterator("raw/a.bin", it));
		// a lookup and a listing allocate nothing, however long the path
		const uint64 before = s_allocs;
		bool found = vol->OpenFile("./raw/./././././././././././././././a.bin", h, true)
			&& !vol->OpenFile("raw/a-name-longer-than-any-path-kept-without-allocating-would-be.bin", h, false)
			&& !vol->OpenFile("h/1/2/3/4/5/6/7/8/9/10/11/12", h, false);
		CHECK(vol->OpenDirectoryIterator("h", it));
		while (vol->Next(it, h))
			found = found && !vol->GetName(h).empty();
		CHECK(found && s_allocs == before);
	}

	// warm: no allocation for any read, cached or straight through
	FSTFileHandle a, c;
	CHECK(vol->OpenFile("raw/a.bin", a, true) && vol->OpenFile("h/c.bin", c, true));
	const uint64 allocs = s_allocs;
	for (int i = 0; i < 50; i++)
	{
		vol->ReadFile(a, (uint32)(rng() % 5000), 1 + rng() % 70000, got.data());
		vol->ReadFile(c, (uint32)(rng() % 5000), 1 + rng() % 70000, got.data());
	}
	CHECK(s_allocs == allocs);

}

// A block whose data is not its H0 hash's fails the read; the one before it
// does not
static void TestCorrupt()
{
	MadeSource* source;
	std::unique_ptr<FSTVolume> vol = Open(false, source);
	CHECK(vol != nullptr);
	if (!vol)
		return;
	source->SetCorrupt(1); // h/c.bin's second block
	FSTFileHandle c;
	uint8 got[64];
	CHECK(vol->OpenFile("h/c.bin", c, true));
	CHECK(vol->ReadFile(c, 5, 10, got) == 10);
	CHECK(vol->ReadFile(c, HDATA + 5, 10, got) == 0);
}

// The source's octets made as they are read, safe from more than one thread:
// a block made for each read, not kept between them
class SharedMadeSource : public TestSource
{
public:
	std::vector<uint8> fst;

	uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) override
	{
		uint8* out = (uint8*)data;
		const uint64 at = clusterOffset + offset;
		if (clusterIndex == 0)
		{
			if (at >= fst.size())
				return 0;
			const uint64 n = std::min<uint64>(size, fst.size() - at);
			memcpy(out, fst.data() + at, n);
			return n;
		}
		if (clusterIndex == 1)
		{
			RawCipher(offset, out, size);
			return size;
		}
		std::unique_ptr<uint8[]> block(new uint8[HBLOCK]);
		for (uint64 done = 0; done < size;)
		{
			const uint64 pos = offset + done;
			HashedBlockCipher(pos / HBLOCK, block.get(), false);
			const uint64 take = std::min<uint64>(size - done, HBLOCK - pos % HBLOCK);
			memcpy(out + done, block.get() + pos % HBLOCK, take);
			done += take;
		}
		return size;
	}
};

static void TestThreads()
{
	std::unique_ptr<SharedMadeSource> src(new SharedMadeSource());
	src->fst = MakeFst();
	NCrypto::AesKey key;
	memcpy(key.b, KEY, 16);
	const uint32 fstSize = (uint32)src->fst.size();
	std::unique_ptr<FSTVolume> vol(FSTVolume::OpenFromDataSource(AsSource(src.release(), true), 0, fstSize, key));
	CHECK(vol != nullptr);
	if (!vol)
		return;
	std::atomic<int> bad{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++)
	{
		threads.emplace_back([&, t]() {
			std::mt19937 rng(t + 40);
			std::vector<uint8> got(3 * HBLOCK), want(3 * HBLOCK);
			for (int i = 0; i < 60; i++)
			{
				const FileDef& f = FILES[rng() % std::size(FILES)];
				FSTFileHandle h;
				if (!vol->OpenFile(f.name, h, true))
				{
					bad++;
					continue;
				}
				const uint32 at = rng() % f.size, n = std::min<uint32>(1 + rng() % (2 * HBLOCK + 9), f.size - at);
				Expected(f, at, want.data(), n);
				if (vol->ReadFile(h, at, n, got.data()) != n || memcmp(got.data(), want.data(), n) != 0)
					bad++;
			}
		});
	}
	for (std::thread& t : threads)
		t.join();
	CHECK(bad == 0);
	CHECK(!vol->HasCorruption());
}

int main()
{
	AES128_init();
	TestCache();
	TestIvFromSource(false);
	TestIvFromSource(true);
	TestVolume(false);
	TestVolume(true);
	TestCorrupt();
	TestThreads();
	if (s_failures)
	{
		std::printf("fst_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("fst_test: all passed\n");
	return 0;
}
