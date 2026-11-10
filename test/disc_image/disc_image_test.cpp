// The core's disc image and title content readers, against images made here:
//  - a .wud reads back whole and at random places, and so does a .wux - its
//    sectors stored out of order, one stored once for two places, the image
//    not a whole number of sectors - mapped and not; mapped, with no stream
//    opened, and not, through the one it was opened with;
//  - a .wux is refused when it is too short for its header or its sector
//    table, gives a sector size out of range, or an image size of more
//    sectors than its table can count; one whose table sends a sector past
//    the file's end reads short there and lends nothing;
//  - wud_lend lends a range from the mapping where it lies in one piece, and
//    nothing where it crosses into a .wux sector stored elsewhere, runs past
//    the end, or the file is not mapped;
//  - fst_source decrypts an unhashed block (CBC, the IV given, the next
//    block's IV the last 16 encrypted octets) and a hashed one (hashes with
//    no IV, then the data with its H0 hash as the IV) from a disc image and
//    from .app files, the same mapped as not; mapped, a block in one piece
//    is decrypted from the pages with no read at all, and one that is not (a
//    hashed block across two .wux sectors) is read and decrypted in place;
//  - a title's content with a file missing, or a cluster twice, does not
//    open;
//  - unmapped, a .wud, a .wux, .app files and a .wuhb are read from four
//    threads at once, each read through a stream of its own lent from the
//    file's pool, and read back right (run under ThreadSanitizer by CI);
//  - a homebrew bundle (.wuhb, a RomFS image) looks its files and
//    directories up through hash chains that collide, and reads files back
//    whole and at random places, mapped and not; a chain that loops ends
//    the lookup.
// The disc image reader (wud.c) and the file reader under all three
// (shared_file.c) are built as C89, as the C89 lane builds them.
#include "Cafe/Filesystem/WUD/wud.h"
#include "../fst/test_source.h"
#include "Common/FileStream.h"
#include "util/crypto/aes128.h"
#include "Cafe/Filesystem/WUHB/wuhb.h"
#include "romfs_image.h"
#include "Cemu/Logging/CemuLogging.h"

#include <streams/file_stream.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- the path ---------------------------------------------------------------

// as the core's, but for a case-insensitive search the test does not need
std::string FileStream::HostPath(const fs::path& path)
{
	return path.string();
}

// --- what the C++ under test takes from the rest of the core -------------

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

// --- images ------------------------------------------------------------------

static const uint32 SECTOR = 0x8000;

static fs::path TempPath(const char* name)
{
	return fs::temp_directory_path() / name;
}

static void WriteFile(const fs::path& path, const std::vector<uint8>& data)
{
	FILE* f = std::fopen(path.string().c_str(), "wb");
	CHECK(f != nullptr);
	if (!f)
		return;
	CHECK(std::fwrite(data.data(), 1, data.size(), f) == data.size());
	std::fclose(f);
}

// A .wux of image: sectors stored in reverse, and the last full one stored
// once for itself and for sector 1, which the image is made to repeat
static std::vector<uint8> MakeWux(std::vector<uint8>& image)
{
	const uint32 sectors = (uint32)((image.size() + SECTOR - 1) / SECTOR);
	const uint32 shared = (uint32)(image.size() / SECTOR) - 1; // a full one
	memcpy(image.data() + 1 * SECTOR, image.data() + shared * SECTOR, SECTOR);
	std::vector<uint32> stored;   // image sector of each stored one
	std::vector<uint32> index(sectors);
	for (uint32 s = sectors; s-- > 0;)
	{
		if (s == 1)
			continue;
		index[s] = (uint32)stored.size();
		stored.push_back(s);
	}
	for (uint32 i = 0; i < stored.size(); i++)
		if (stored[i] == shared)
			index[1] = i;

	// the header, little-endian: "WUX0", 0x1099d02e, the sector size, four
	// octets of padding, the image's size, flags, padding - 32 octets
	std::vector<uint8> wux(32 + index.size() * 4);
	auto put = [&](size_t at, uint64 v, int n) { for (int i = 0; i < n; i++) wux[at + i] = (uint8)(v >> (8 * i)); };
	memcpy(wux.data(), "WUX0", 4);
	put(4, 0x1099d02e, 4);
	put(8, SECTOR, 4);
	put(16, image.size(), 8);
	for (size_t i = 0; i < index.size(); i++)
		put(32 + i * 4, index[i], 4);
	wux.resize((wux.size() + SECTOR - 1) / SECTOR * SECTOR);
	for (uint32 s : stored)
	{
		const size_t at = wux.size();
		wux.resize(at + SECTOR); // the last one padded
		memcpy(wux.data() + at, image.data() + (size_t)s * SECTOR, std::min<size_t>(SECTOR, image.size() - (size_t)s * SECTOR));
	}
	return wux;
}

// --- tests -------------------------------------------------------------------

static bool ReadsBack(wud_t* wud, const std::vector<uint8>& image, std::mt19937& rng)
{
	std::vector<uint8> got(image.size() + 16);
	if (wud_getWUDSize(wud) != (long long)image.size())
		return false;
	if (wud_readData(wud, got.data(), (uint32)got.size(), 0) != image.size() || memcmp(got.data(), image.data(), image.size()) != 0)
		return false;
	for (int i = 0; i < 300; i++)
	{
		const size_t at = rng() % image.size();
		const size_t len = 1 + rng() % (3 * SECTOR);
		const size_t want = std::min(len, image.size() - at);
		if (wud_readData(wud, got.data(), (uint32)len, (long long)at) != want || memcmp(got.data(), image.data() + at, want) != 0)
			return false;
	}
	return wud_readData(wud, got.data(), 1, (long long)image.size()) == 0;
}

static void TestImages()
{
	std::mt19937 rng(5);
	std::vector<uint8> image(9 * SECTOR + 0x1230);
	for (uint8& b : image)
		b = (uint8)rng();
	std::vector<uint8> wuxImage = image;
	const std::vector<uint8> wux = MakeWux(wuxImage);
	const fs::path wudPath = TempPath("parallel_gx2_disc_image_test.wud");
	const fs::path wuxPath = TempPath("parallel_gx2_disc_image_test.wux");
	WriteFile(wudPath, image);
	WriteFile(wuxPath, wux);

	for (bool map : {true, false})
	{
		const unsigned flags = map ? 0 : WUD_OPEN_NO_MAP;
		wud_t* wud = wud_open(wudPath.string().c_str(), flags);
		wud_t* wx = wud_open(wuxPath.string().c_str(), flags);
		CHECK(wud && wx);
		if (!wud || !wx)
			continue;
		CHECK(!wud_isWUXCompressed(wud) && wud_isWUXCompressed(wx));
		CHECK(wud_isMapped(wud) == map && wud_isMapped(wx) == map);
		CHECK(ReadsBack(wud, image, rng));
		CHECK(ReadsBack(wx, wuxImage, rng));
		// mapped, every octet from the pages: no stream; not, the one
		// opened first, and no more for one reader
		CHECK(map ? wud_streamsOpened(wud) == 0 && wud_streamsOpened(wx) == 0
			: wud_streamsOpened(wud) == 1 && wud_streamsOpened(wx) == 1);

		// lent only in one piece, and only mapped
		const unsigned char* p = wud_lend(wud, 3 * SECTOR, SECTOR + 5);
		CHECK(map ? p && memcmp(p, image.data() + SECTOR + 5, 3 * SECTOR) == 0 : !p);
		CHECK(!wud_lend(wud, 2, (long long)image.size() - 1));
		p = wud_lend(wx, SECTOR - 16, 1 * SECTOR + 16); // the shared sector
		CHECK(map ? p && memcmp(p, wuxImage.data() + SECTOR + 16, SECTOR - 16) == 0 : !p);
		p = wud_lend(wx, 0x200, 5 * SECTOR);
		CHECK(map ? p && memcmp(p, wuxImage.data() + 5 * SECTOR, 0x200) == 0 : !p);
		CHECK(!wud_lend(wx, 32, 2 * SECTOR - 16)); // across, stored apart
		CHECK(!wud_lend(wx, 0x1231, 9 * SECTOR)); // past the end
		p = wud_lend(wx, 0x1230, 9 * SECTOR);     // the last, padded one
		CHECK(map ? p && memcmp(p, wuxImage.data() + 9 * SECTOR, 0x1230) == 0 : !p);
		wud_close(wud);
		wud_close(wx);
	}

	// not images: each refused, mapped and not
	for (unsigned flags : {0u, (unsigned)WUD_OPEN_NO_MAP})
	{
		const std::string p = wuxPath.string();
		// too short for its own index table
		WriteFile(wuxPath, std::vector<uint8>(wux.begin(), wux.begin() + 40));
		CHECK(wud_open(p.c_str(), flags) == nullptr);
		// too short for a header
		WriteFile(wuxPath, std::vector<uint8>(wux.begin(), wux.begin() + 20));
		CHECK(wud_open(p.c_str(), flags) == nullptr);
		// an image size of more sectors than the file could say where are,
		// and one past what the index can count
		std::vector<uint8> bad = wux;
		for (uint64 size : {0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0x100000000ull * SECTOR, 0ull})
		{
			for (int i = 0; i < 8; i++)
				bad[16 + i] = (uint8)(size >> (8 * i));
			WriteFile(wuxPath, bad);
			CHECK(wud_open(p.c_str(), flags) == nullptr);
		}
		// a sector size out of range
		bad = wux;
		bad[8] = 0x80; bad[9] = 0; bad[10] = 0; bad[11] = 0;
		WriteFile(wuxPath, bad);
		CHECK(wud_open(p.c_str(), flags) == nullptr);
		// a folder
		CHECK(wud_open(fs::temp_directory_path().string().c_str(), flags) == nullptr);
		CHECK(wud_open(nullptr, flags) == nullptr);
	}
	// a .wux whose table sends a sector past the file's end: a short read
	// and nothing lent, not a read past the mapping
	{
		std::vector<uint8> bad = wux;
		bad[32] = 0xFF; bad[33] = 0xFF; bad[34] = 0; bad[35] = 0; // sector 0 stored as 0xFFFF
		WriteFile(wuxPath, bad);
		for (unsigned flags : {0u, (unsigned)WUD_OPEN_NO_MAP})
		{
			wud_t* wx = wud_open(wuxPath.string().c_str(), flags);
			CHECK(wx != nullptr);
			if (!wx)
				continue;
			std::vector<uint8> got(SECTOR);
			CHECK(wud_readData(wx, got.data(), 16, 0) == 0);
			CHECK(!wud_lend(wx, 16, 0));
			CHECK(wud_readData(wx, got.data(), 16, SECTOR) == 16); // the next one is where it was
			wud_close(wx);
		}
	}
	fs::remove(wudPath);
	fs::remove(wuxPath);
}

static const uint8 KEY[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 1, 2, 3, 4, 5, 6, 7, 8};

struct Encrypted
{
	std::vector<uint8> plain;
	std::vector<uint8> cipher;
};

// Unhashed blocks of blockSize, CBC-chained from iv0 as a cluster's are
static Encrypted RawCluster(uint32 blocks, uint32 blockSize, const uint8* iv0, std::mt19937& rng)
{
	Encrypted e;
	e.plain.resize((size_t)blocks * blockSize);
	for (uint8& b : e.plain)
		b = (uint8)rng();
	// AES128_CBC_encrypt writes over its input
	std::vector<uint8> in = e.plain;
	e.cipher.resize(e.plain.size());
	AES128_CBC_encrypt(e.cipher.data(), in.data(), (uint32)in.size(), KEY, iv0);
	return e;
}

// Hashed blocks: in each, sixteen H0 hashes (random here) and the data
// encrypted with the block's own, blockIndex % 16
static Encrypted HashedCluster(uint32 blocks, std::mt19937& rng)
{
	const uint32 B = FST_HASHED_BLOCK_SIZE, H = FST_HASHED_BLOCK_HASH_SIZE;
	Encrypted e;
	e.plain.resize((size_t)blocks * B);
	for (uint8& b : e.plain)
		b = (uint8)rng();
	std::vector<uint8> in = e.plain; // AES128_CBC_encrypt writes over it
	e.cipher.resize(e.plain.size());
	for (uint32 i = 0; i < blocks; i++)
	{
		uint8* p = in.data() + (size_t)i * B;
		uint8* c = e.cipher.data() + (size_t)i * B;
		const uint8 zero[16]{};
		AES128_CBC_encrypt(c, p, H, KEY, zero);
		AES128_CBC_encrypt(c + H, p + H, B - H, KEY, e.plain.data() + (size_t)i * B + 20 * (i % 16));
	}
	return e;
}

// Every block of a raw cluster at clusterOffset decrypts, each with the IV
// the one before gave, in order and then out of it (an IV from the file)
static bool RawReadsBack(fst_source_t* ds, uint16 cluster, uint64 clusterOffset, const Encrypted& e, uint32 blockSize, const uint8* iv0)
{
	std::vector<uint8> block(blockSize);
	uint8 iv[16], next[16];
	memcpy(iv, iv0, 16);
	const uint32 blocks = (uint32)(e.plain.size() / blockSize);
	for (uint32 i = 0; i < blocks; i++)
	{
		if (!fst_source_read_raw_block(ds, cluster, clusterOffset, (uint64)i * blockSize, block.data(), blockSize, KEY, iv, next))
			return false;
		if (memcmp(block.data(), e.plain.data() + (size_t)i * blockSize, blockSize) != 0)
			return false;
		if (memcmp(next, e.cipher.data() + (size_t)(i + 1) * blockSize - 16, 16) != 0)
			return false;
		memcpy(iv, next, 16);
	}
	for (uint32 i = blocks; i-- > 1;)
	{
		uint8 prev[16];
		const uint8* p = fst_source_fetch(ds, cluster, clusterOffset, (uint64)i * blockSize - 16, prev, 16);
		if (!p || !fst_source_read_raw_block(ds, cluster, clusterOffset, (uint64)i * blockSize, block.data(), blockSize, KEY, p, next))
			return false;
		if (memcmp(block.data(), e.plain.data() + (size_t)i * blockSize, blockSize) != 0)
			return false;
	}
	// a block past the cluster's file, or not a whole number of AES blocks
	return !fst_source_read_raw_block(ds, cluster, clusterOffset, e.plain.size() + (cluster ? 0x100000 : 0x10000000), block.data(), blockSize, KEY, iv, next)
		&& !fst_source_read_raw_block(ds, cluster, clusterOffset, 0, block.data(), 24, KEY, iv, next);
}

static bool HashedReadsBack(fst_source_t* ds, uint16 cluster, uint64 clusterOffset, const Encrypted& e)
{
	const uint32 B = FST_HASHED_BLOCK_SIZE;
	std::vector<uint8> block(B);
	for (uint32 i = 0; i < e.plain.size() / B; i++)
	{
		if (!fst_source_read_hashed_block(ds, cluster, clusterOffset, (uint64)i * B, block.data(), block.data() + FST_HASHED_BLOCK_HASH_SIZE, KEY, i % 16))
			return false;
		if (memcmp(block.data(), e.plain.data() + (size_t)i * B, B) != 0)
			return false;
	}
	return !fst_source_read_hashed_block(ds, cluster, clusterOffset, 0, block.data(), block.data() + FST_HASHED_BLOCK_HASH_SIZE, KEY, 16);
}

// A source through another, counting the octets it had to read rather than
// lend
class Counting : public TestSource
{
public:
	explicit Counting(fst_source_t* inner) : m_inner(inner), m_source(AsSource(this, false)) {}
	~Counting() override { fst_source_close(m_source); }
	uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) override
	{
		const uint64 n = fst_source_read(m_inner, clusterIndex, clusterOffset, offset, data, size);
		read += n;
		return n;
	}
	const uint8* Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size) override
	{
		return fst_source_lend(m_inner, clusterIndex, clusterOffset, offset, size);
	}
	// the source over this
	fst_source_t* Source() const { return m_source; }
	uint64 read = 0;

private:
	fst_source_t* m_inner;
	fst_source_t* m_source;
};

struct SourceCloser
{
	void operator()(fst_source_t* s) const { fst_source_close(s); }
};
using SourcePtr = std::unique_ptr<fst_source_t, SourceCloser>;

static void TestDiscSource()
{
	std::mt19937 rng(17);
	const uint8 iv0[16] = {0, 3};
	// a disc: a raw cluster of 0x8000 blocks at 0x10000, a hashed one at
	// 0x48000 - its blocks across two sectors, which a .wux stores apart
	const Encrypted raw = RawCluster(6, SECTOR, iv0, rng);
	const Encrypted hashed = HashedCluster(3, rng);
	std::vector<uint8> image(0x48000 + hashed.cipher.size() + SECTOR);
	memcpy(image.data() + 0x10000, raw.cipher.data(), raw.cipher.size());
	memcpy(image.data() + 0x48000, hashed.cipher.data(), hashed.cipher.size());
	std::vector<uint8> wuxImage = image;
	const std::vector<uint8> wux = MakeWux(wuxImage);
	CHECK(memcmp(wuxImage.data() + 0x10000, image.data() + 0x10000, image.size() - 0x10000) == 0);
	const fs::path wudPath = TempPath("parallel_gx2_disc_source_test.wud");
	const fs::path wuxPath = TempPath("parallel_gx2_disc_source_test.wux");
	WriteFile(wudPath, image);
	WriteFile(wuxPath, wux);

	for (bool map : {true, false})
	{
		for (const fs::path& path : {wudPath, wuxPath})
		{
			SourcePtr ds(fst_source_open_disc(path.string().c_str(), map ? 0 : FST_SOURCE_NO_MAP));
			CHECK(ds != nullptr);
			if (!ds)
				continue;
			// the partition's base and the cluster's offset both count
			fst_source_set_base_offset(ds.get(), 0x8000);
			CHECK(fst_source_get_base_offset(ds.get()) == 0x8000);
			Counting c(ds.get());
			CHECK(RawReadsBack(c.Source(), 1, 0x8000, raw, SECTOR, iv0));
			// mapped, raw blocks lie in one sector: decrypted from the pages
			CHECK(map ? c.read == 0 : c.read > 0);
			CHECK(HashedReadsBack(c.Source(), 2, 0x40000, hashed));
			// a .wux's hashed blocks lie across sectors stored apart: their
			// data read, from the pages where it is mapped, their hashes
			// (in one sector) lent
			const bool lent = map && path == wudPath;
			CHECK(lent ? c.read == 0 : c.read > 0);
			if (map && !lent)
				CHECK(c.read == 3 * (FST_HASHED_BLOCK_SIZE - FST_HASHED_BLOCK_HASH_SIZE));
		}
	}
	fs::remove(wudPath);
	fs::remove(wuxPath);
}

static void TestAppSource()
{
	std::mt19937 rng(23);
	const uint8 iv0[16] = {0, 0};
	const uint8 iv1[16] = {0, 1};
	const Encrypted fst = RawCluster(1, SECTOR, iv0, rng);
	const Encrypted raw = RawCluster(5, SECTOR, iv1, rng);
	const Encrypted hashed = HashedCluster(4, rng);
	const fs::path dir = TempPath("parallel_gx2_app_source_test");
	fs::create_directories(dir);
	WriteFile(dir / "00000000.app", fst.cipher);
	WriteFile(dir / "0000000a.app", raw.cipher);
	WriteFile(dir / "0000001b.app", hashed.cipher);
	auto Open = [&](std::initializer_list<std::pair<uint16, const char*>> contents, bool map) {
		std::vector<std::string> paths;
		std::vector<const char*> ptrs;
		std::vector<uint16> clusters;
		for (const auto& c : contents)
		{
			paths.push_back((dir / c.second).string());
			clusters.push_back(c.first);
		}
		for (const std::string& p : paths)
			ptrs.push_back(p.c_str());
		return SourcePtr(fst_source_open_app(ptrs.data(), clusters.data(), clusters.size(), map ? 0 : FST_SOURCE_NO_MAP));
	};

	for (bool map : {true, false})
	{
		SourcePtr ds = Open({{0, "00000000.app"}, {1, "0000000a.app"}, {2, "0000001b.app"}}, map);
		CHECK(ds != nullptr);
		if (!ds)
			continue;
		Counting c(ds.get());
		// the cluster's offset is not the file's: each file is its cluster
		CHECK(RawReadsBack(c.Source(), 0, 0x123456, fst, SECTOR, iv0));
		CHECK(RawReadsBack(c.Source(), 1, 0x8000, raw, SECTOR, iv1));
		CHECK(HashedReadsBack(c.Source(), 2, 0x10000, hashed));
		// mapped, every block decrypted from the pages
		CHECK(map ? c.read == 0 : c.read > 0);
		// lent where it lies, mapped
		const uint8* p = fst_source_lend(ds.get(), 1, 0, 0x100, 0x2000);
		CHECK(map ? p && memcmp(p, raw.cipher.data() + 0x100, 0x2000) == 0 : !p);
		CHECK(!fst_source_lend(ds.get(), 1, 0, raw.cipher.size() - 4, 8));
		CHECK(!fst_source_lend(ds.get(), 3, 0, 0, 1));
		// read, from the pages or the file, short at the end
		std::vector<uint8> got(0x300);
		CHECK(fst_source_read(ds.get(), 2, 0, hashed.cipher.size() - 0x100, got.data(), got.size()) == 0x100);
		CHECK(memcmp(got.data(), hashed.cipher.data() + hashed.cipher.size() - 0x100, 0x100) == 0);
		CHECK(fst_source_read(ds.get(), 2, 0, hashed.cipher.size(), got.data(), 1) == 0);
		CHECK(fst_source_read(ds.get(), 7, 0, 0, got.data(), 1) == 0);
		// clusters with no file between them read nothing
		SourcePtr sparse = Open({{0, "00000000.app"}, {5, "0000000a.app"}}, map);
		CHECK(sparse && fst_source_read(sparse.get(), 3, 0, 0, got.data(), 1) == 0
			&& !fst_source_lend(sparse.get(), 3, 0, 0, 1)
			&& fst_source_read(sparse.get(), 5, 0, 0, got.data(), 16) == 16
			&& memcmp(got.data(), raw.cipher.data(), 16) == 0);
	}

	// a file missing, a cluster twice
	CHECK(Open({{0, "00000000.app"}, {1, "0000000c.app"}}, true) == nullptr);
	CHECK(Open({{0, "00000000.app"}, {0, "0000000a.app"}}, true) == nullptr);
	CHECK(Open({}, true) != nullptr);
	fs::remove_all(dir);
}

// --- a homebrew bundle -------------------------------------------------------


static void TestWuhb()
{
	std::mt19937 rng(31);
	std::vector<RomfsFile> files;
	files.push_back({"", "root.txt", {}});
	files.push_back({"meta", "meta.xml", {}});
	files.push_back({"content", "empty.bin", {}});
	for (int i = 0; i < 9; i++)
		files.push_back({"content", fmt::format("file{}.bin", i), {}});
	for (int i = 0; i < 6; i++)
		files.push_back({"content/sub", fmt::format("Deep_{}.dat", i), {}});
	for (size_t f = 0; f < files.size(); f++)
		if (files[f].name != "empty.bin")
		{
			files[f].data.resize(1 + rng() % 70000);
			for (uint8& b : files[f].data)
				b = (uint8)rng();
		}
	const fs::path path = TempPath("parallel_gx2_wuhb_test.wuhb");
	WriteFile(path, MakeWuhb(files));

	for (bool map : {true, false})
	{
		wuhb_t* reader = wuhb_open(path.string().c_str(), map ? 0 : WUHB_OPEN_NO_MAP);
		CHECK(reader != nullptr);
		if (!reader)
			continue;
		CHECK(wuhb_is_mapped(reader) == map);
		auto Lookup = [&](const std::string& p, bool isFile) { return wuhb_lookup(reader, p.data(), p.size(), isFile); };
		auto FileName = [&](uint32 e) { wuhb_file_t f; char n[256]; return wuhb_file_entry(reader, e, &f, n, sizeof(n)) ? std::string(n) : std::string("?"); };
		auto DirName = [&](uint32 e) { wuhb_dir_t d; char n[256]; return wuhb_dir_entry(reader, e, &d, n, sizeof(n)) ? std::string(n) : std::string("?"); };
		bool all = true;
		std::vector<uint8> got(80000);
		for (const RomfsFile& f : files)
		{
			const std::string p = f.dir.empty() ? f.name : f.dir + "/" + f.name;
			const uint32 e = Lookup(p, true);
			all = all && e != WUHB_ENTRY_EMPTY && wuhb_file_size(reader, e) == f.data.size();
			if (e == WUHB_ENTRY_EMPTY)
				continue;
			all = all && Lookup("/" + p, true) == e;
			// a name it begins, and one that begins it, are not it; nor is
			// it with '/' after it, or in another case
			all = all && Lookup(p + "x", true) == WUHB_ENTRY_EMPTY
				&& Lookup(p.substr(0, p.size() - 1), true) == WUHB_ENTRY_EMPTY
				&& Lookup(p + "/", true) == WUHB_ENTRY_EMPTY;
			std::string upper = p;
			for (char& c : upper)
				c = (char)toupper((unsigned char)c);
			all = all && (upper == p || Lookup(upper, true) == WUHB_ENTRY_EMPTY);
			// doubled '/' between names count as one
			if (!f.dir.empty())
				all = all && Lookup(f.dir + "//" + f.name, true) == e;
			all = all && wuhb_read_file(reader, e, 0, got.size(), got.data()) == f.data.size()
				&& (f.data.empty() || memcmp(got.data(), f.data.data(), f.data.size()) == 0);
			for (int i = 0; i < 10 && !f.data.empty(); i++)
			{
				const uint64 at = rng() % f.data.size(), len = 1 + rng() % 9000;
				const uint64 want = std::min<uint64>(len, f.data.size() - at);
				all = all && wuhb_read_file(reader, e, at, len, got.data()) == want
					&& memcmp(got.data(), f.data.data() + at, want) == 0;
			}
			all = all && wuhb_read_file(reader, e, f.data.size(), 1, got.data()) == 0;
			all = all && FileName(e) == f.name;
			// cut to fit, and ended
			{
				wuhb_file_t fe;
				char cut[5];
				all = all && wuhb_file_entry(reader, e, &fe, cut, sizeof(cut)) && fe.name_size == f.name.size()
					&& std::string(cut) == f.name.substr(0, 4);
			}
		}
		CHECK(all);
		// directories, and what is not there
		const uint32 sub = Lookup("content/sub", false);
		CHECK(sub != WUHB_ENTRY_EMPTY && DirName(sub) == "sub");
		CHECK(sub != WUHB_ENTRY_EMPTY && Lookup("content/sub/", false) == sub);
		// a directory is no file, with '/' after it or not
		CHECK(Lookup("content/sub/", true) == WUHB_ENTRY_EMPTY && Lookup("content/sub", true) == WUHB_ENTRY_EMPTY);
		CHECK(Lookup("content/file9.bin", true) == WUHB_ENTRY_EMPTY);
		CHECK(Lookup("content/sub/file1.bin", true) == WUHB_ENTRY_EMPTY);
		CHECK(Lookup("nope/file1.bin", true) == WUHB_ENTRY_EMPTY);
		CHECK(Lookup("content/file1.bi", true) == WUHB_ENTRY_EMPTY);
		CHECK(wuhb_file_size(reader, 0x7FFFFFF0) == 0);
		CHECK(wuhb_read_file(reader, WUHB_ENTRY_EMPTY, 0, 1, got.data()) == 0);
		// the root, as "", "/" and "//"
		const uint32 root = Lookup("", false);
		CHECK(root != WUHB_ENTRY_EMPTY && Lookup("/", false) == root && Lookup("//", false) == root);
		// a directory lists what it holds: its directories, then its files
		{
			const uint32 content = Lookup("content", false);
			wuhb_dir_t d;
			CHECK(content != WUHB_ENTRY_EMPTY && wuhb_dir_entry(reader, content, &d, nullptr, 0));
			std::vector<std::string> dirs, names;
			for (uint32 e = d.dir_list_head, n = 0; e != WUHB_ENTRY_EMPTY && n < 100; n++)
			{
				wuhb_dir_t c;
				char name[256];
				if (!wuhb_dir_entry(reader, e, &c, name, sizeof(name)))
					break;
				dirs.push_back(name);
				e = c.list_next;
			}
			for (uint32 e = d.file_list_head, n = 0; e != WUHB_ENTRY_EMPTY && n < 100; n++)
			{
				wuhb_file_t c;
				char name[256];
				if (!wuhb_file_entry(reader, e, &c, name, sizeof(name)))
					break;
				names.push_back(name);
				e = c.list_next;
			}
			CHECK(dirs.size() == 1 && dirs[0] == "sub");
			CHECK(names.size() == 10);
		}
		wuhb_close(reader);
	}

	// a chain that comes back round: what is not there is not found
	WriteFile(path, MakeWuhb(files, true));
	wuhb_t* looped = wuhb_open(path.string().c_str(), 0);
	CHECK(looped && wuhb_lookup(looped, "content/file5.bin", 17, true) != WUHB_ENTRY_EMPTY);
	CHECK(looped && wuhb_lookup(looped, "content/file9.bin", 17, true) == WUHB_ENTRY_EMPTY);
	wuhb_close(looped);
	// not a bundle
	std::vector<uint8> notOne(0x60);
	WriteFile(path, notOne);
	CHECK(wuhb_open(path.string().c_str(), 0) == nullptr);
	// a header that says its hash tables hold nothing
	{
		std::vector<uint8> bad = MakeWuhb(files);
		for (int i = 0; i < 8; i++)
			bad[0x10 + i] = 0;
		WriteFile(path, bad);
		CHECK(wuhb_open(path.string().c_str(), 0) == nullptr);
	}
	// too short for its header
	WriteFile(path, std::vector<uint8>{'W', 'U', 'H', 'B'});
	CHECK(wuhb_open(path.string().c_str(), 0) == nullptr);
	CHECK(wuhb_open(fs::temp_directory_path().string().c_str(), 0) == nullptr);
	fs::remove(path);
}

static void TestThreadsUnmapped()
{
	std::mt19937 rng(77);
	std::vector<uint8> image(12 * SECTOR + 0x777);
	for (uint8& b : image)
		b = (uint8)rng();
	std::vector<uint8> wuxImage = image;
	const std::vector<uint8> wux = MakeWux(wuxImage);
	const fs::path wudPath = TempPath("parallel_gx2_threads_test.wud");
	const fs::path wuxPath = TempPath("parallel_gx2_threads_test.wux");
	WriteFile(wudPath, image);
	WriteFile(wuxPath, wux);
	const fs::path appDir = TempPath("parallel_gx2_threads_app");
	fs::create_directories(appDir);
	WriteFile(appDir / "00000005.app", image);
	std::vector<RomfsFile> files;
	for (int i = 0; i < 6; i++)
	{
		files.push_back({"content", fmt::format("f{}.bin", i), std::vector<uint8>(3000 + i * 9000)});
		for (uint8& b : files.back().data)
			b = (uint8)rng();
	}
	const fs::path wuhbPath = TempPath("parallel_gx2_threads_test.wuhb");
	WriteFile(wuhbPath, MakeWuhb(files));

	wud_t* wud = wud_open(wudPath.string().c_str(), WUD_OPEN_NO_MAP);
	wud_t* wx = wud_open(wuxPath.string().c_str(), WUD_OPEN_NO_MAP);
	const std::string appPath = (appDir / "00000005.app").string();
	const char* appPaths[] = {appPath.c_str()};
	const uint16 appClusters[] = {0};
	SourcePtr app(fst_source_open_app(appPaths, appClusters, 1, FST_SOURCE_NO_MAP));
	wuhb_t* wuhb = wuhb_open(wuhbPath.string().c_str(), WUHB_OPEN_NO_MAP);
	CHECK(wud && wx && app && wuhb);
	if (wud && wx && app && wuhb)
	{
		CHECK(!wud_isMapped(wud) && !wud_isMapped(wx) && !wuhb_is_mapped(wuhb));
		std::vector<uint32> entries;
		for (const RomfsFile& f : files)
		{
			const std::string p = "content/" + f.name;
			entries.push_back(wuhb_lookup(wuhb, p.data(), p.size(), true));
		}
		std::atomic<int> bad{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++)
		{
			threads.emplace_back([&, t]() {
				std::mt19937 r(t + 3);
				std::vector<uint8> got(3 * SECTOR);
				for (int i = 0; i < 200; i++)
				{
					const size_t at = r() % image.size();
					const size_t n = std::min<size_t>(1 + r() % got.size(), image.size() - at);
					if (wud_readData(wud, got.data(), (uint32)n, (long long)at) != n || memcmp(got.data(), image.data() + at, n) != 0)
						bad++;
					if (wud_readData(wx, got.data(), (uint32)n, (long long)at) != n || memcmp(got.data(), wuxImage.data() + at, n) != 0)
						bad++;
					if (fst_source_read(app.get(), 0, 0, at, got.data(), n) != n || memcmp(got.data(), image.data() + at, n) != 0)
						bad++;
					const size_t k = r() % files.size();
					const std::vector<uint8>& fd = files[k].data;
					const uint64 fat = r() % fd.size(), fn = std::min<uint64>(1 + r() % 5000, fd.size() - fat);
					if (wuhb_read_file(wuhb, entries[k], fat, fn, got.data()) != fn || memcmp(got.data(), fd.data() + fat, fn) != 0)
						bad++;
				}
			});
		}
		for (std::thread& t : threads)
			t.join();
		CHECK(bad == 0);
		// a stream each at most, for the four
		CHECK(wud_streamsOpened(wud) <= 4 && wx && wud_streamsOpened(wx) <= 4);
	}
	if (wud)
		wud_close(wud);
	if (wx)
		wud_close(wx);
	app.reset();
	wuhb_close(wuhb);
	fs::remove(wudPath);
	fs::remove(wuxPath);
	fs::remove(wuhbPath);
	fs::remove_all(appDir);
}

int main()
{
	AES128_init();
	TestImages();
	TestDiscSource();
	TestAppSource();
	TestWuhb();
	TestThreadsUnmapped();
	if (s_failures)
	{
		std::printf("disc_image_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("disc_image_test: all passed\n");
	return 0;
}
