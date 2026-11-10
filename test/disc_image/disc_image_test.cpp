// The core's disc image and title content readers, against images made here:
//  - a .wud reads back whole and at random places, and so does a .wux - its
//    sectors stored out of order, one stored once for two places, the image
//    not a whole number of sectors - mapped and not;
//  - wud_lend lends a range from the mapping where it lies in one piece, and
//    nothing where it crosses into a .wux sector stored elsewhere, runs past
//    the end, or the file is not mapped;
//  - FSTDataSource decrypts an unhashed block (CBC, the IV given, the next
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
//    whole and at random places, mapped and not; mapped, with no read of
//    the stream once open; a chain that loops ends the lookup.
// FileStream is the one below, over libretro-common's filestream as the core's
// is, counting what is read and mapping only when let.
#include "Cafe/Filesystem/WUD/wud.h"
#include "Cafe/Filesystem/FST/FSTDataSource.h"
#include "Common/FileStream.h"
#include "util/crypto/aes128.h"
#include "Cafe/Filesystem/WUHB/WUHBReader.h"
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

// --- the stream ----------------------------------------------------------

static std::atomic<uint32> s_reads{0};
static bool s_noMap = false;

FileStream* FileStream::openFile2(const fs::path& path, bool allowWrite)
{
	RFILE* f = filestream_open(path.string().c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	return f ? new FileStream(f) : nullptr;
}

FileStream* FileStream::openFileMapped(const fs::path& path)
{
	RFILE* f = filestream_open(path.string().c_str(), RETRO_VFS_FILE_ACCESS_READ,
		s_noMap ? RETRO_VFS_FILE_ACCESS_HINT_NONE : RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
	return f ? new FileStream(f) : nullptr;
}

std::span<const uint8> FileStream::MappedSpan()
{
	int64_t len = 0;
	const uint8_t* map = filestream_get_mapped_ptr(m_file, &len);
	return map && len > 0 ? std::span<const uint8>(map, (size_t)len) : std::span<const uint8>();
}

FileStream::~FileStream()
{
	filestream_close(m_file);
}

uint64 FileStream::GetSize()
{
	const int64_t size = filestream_get_size(m_file);
	return size < 0 ? 0 : (uint64)size;
}

void FileStream::SetPosition(uint64 pos)
{
	filestream_seek(m_file, (int64_t)pos, RETRO_VFS_SEEK_POSITION_START);
}

uint32 FileStream::readData(void* data, uint32 length)
{
	s_reads++;
	const int64_t n = filestream_read(m_file, data, length);
	return n < 0 ? 0 : (uint32)n;
}

// --- what WUHBReader takes from the rest of the core --------------------

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

	wuxHeader_t header{};
	header.magic0 = WUX_MAGIC_0;
	header.magic1 = WUX_MAGIC_1;
	header.sectorSize = SECTOR;
	header.uncompressedSize = image.size();
	std::vector<uint8> wux(sizeof(header) + index.size() * 4);
	memcpy(wux.data(), &header, sizeof(header));
	memcpy(wux.data() + sizeof(header), index.data(), index.size() * 4);
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
		s_noMap = !map;
		wud_t* wud = wud_open(wudPath);
		wud_t* wx = wud_open(wuxPath);
		CHECK(wud && wx);
		if (!wud || !wx)
			continue;
		CHECK(!wud_isWUXCompressed(wud) && wud_isWUXCompressed(wx));
		CHECK((wud->mapped != nullptr) == map && (wx->mapped != nullptr) == map);
		const uint32 reads = s_reads;
		CHECK(ReadsBack(wud, image, rng));
		CHECK(ReadsBack(wx, wuxImage, rng));
		// mapped, every octet from the pages
		CHECK(map ? s_reads == reads : s_reads > reads);

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
	s_noMap = false;

	// too short for its own index table
	std::vector<uint8> cut(wux.begin(), wux.begin() + 40);
	WriteFile(wuxPath, cut);
	CHECK(wud_open(wuxPath) == nullptr);
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
	const uint32 B = FSTDataSource::HASHED_BLOCK_SIZE, H = FSTDataSource::HASHED_BLOCK_HASH_SIZE;
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
static bool RawReadsBack(FSTDataSource& ds, uint16 cluster, uint64 clusterOffset, const Encrypted& e, uint32 blockSize, const uint8* iv0)
{
	std::vector<uint8> block(blockSize);
	uint8 iv[16], next[16];
	memcpy(iv, iv0, 16);
	const uint32 blocks = (uint32)(e.plain.size() / blockSize);
	for (uint32 i = 0; i < blocks; i++)
	{
		if (!ds.ReadRawBlock(cluster, clusterOffset, (uint64)i * blockSize, block.data(), blockSize, KEY, iv, next))
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
		const uint8* p = ds.Fetch(cluster, clusterOffset, (uint64)i * blockSize - 16, prev, 16);
		if (!p || !ds.ReadRawBlock(cluster, clusterOffset, (uint64)i * blockSize, block.data(), blockSize, KEY, p, next))
			return false;
		if (memcmp(block.data(), e.plain.data() + (size_t)i * blockSize, blockSize) != 0)
			return false;
	}
	// a block past the cluster's file, or not a whole number of AES blocks
	return !ds.ReadRawBlock(cluster, clusterOffset, e.plain.size() + (cluster ? 0x100000 : 0x10000000), block.data(), blockSize, KEY, iv, next)
		&& !ds.ReadRawBlock(cluster, clusterOffset, 0, block.data(), 24, KEY, iv, next);
}

static bool HashedReadsBack(FSTDataSource& ds, uint16 cluster, uint64 clusterOffset, const Encrypted& e)
{
	const uint32 B = FSTDataSource::HASHED_BLOCK_SIZE;
	std::vector<uint8> block(B);
	for (uint32 i = 0; i < e.plain.size() / B; i++)
	{
		if (!ds.ReadHashedBlock(cluster, clusterOffset, (uint64)i * B, block.data(), KEY, i % 16))
			return false;
		if (memcmp(block.data(), e.plain.data() + (size_t)i * B, B) != 0)
			return false;
	}
	return !ds.ReadHashedBlock(cluster, clusterOffset, 0, block.data(), KEY, 16);
}

// A source through another, counting the octets it had to read rather than
// lend
class Counting : public FSTDataSource
{
public:
	explicit Counting(FSTDataSource& inner) : m_inner(inner) {}
	uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) override
	{
		const uint64 n = m_inner.readData(clusterIndex, clusterOffset, offset, data, size);
		read += n;
		return n;
	}
	const uint8* Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size) override
	{
		return m_inner.Lend(clusterIndex, clusterOffset, offset, size);
	}
	uint64 read = 0;

private:
	FSTDataSource& m_inner;
};

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
		s_noMap = !map;
		for (const fs::path& path : {wudPath, wuxPath})
		{
			std::unique_ptr<FSTDataSourceWUD> ds(FSTDataSourceWUD::Open(path));
			CHECK(ds != nullptr);
			if (!ds)
				continue;
			// the partition's base and the cluster's offset both count
			ds->SetBaseOffset(0x8000);
			Counting c(*ds);
			const uint32 reads = s_reads;
			CHECK(RawReadsBack(c, 1, 0x8000, raw, SECTOR, iv0));
			// mapped, raw blocks lie in one sector: decrypted from the pages
			CHECK(map ? c.read == 0 : c.read > 0);
			CHECK(HashedReadsBack(c, 2, 0x40000, hashed));
			// a .wux's hashed blocks lie across sectors stored apart: their
			// data read, from the pages where it is mapped, their hashes
			// (in one sector) lent
			const bool lent = map && path == wudPath;
			CHECK(lent ? c.read == 0 : c.read > 0);
			if (map && !lent)
				CHECK(c.read == 3 * (FSTDataSource::HASHED_BLOCK_SIZE - FSTDataSource::HASHED_BLOCK_HASH_SIZE));
			CHECK(map ? s_reads == reads : s_reads > reads);
		}
	}
	s_noMap = false;
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
	const FSTDataSourceApp::Content contents[] = {{0, 0x0}, {1, 0xa}, {2, 0x1b}};

	for (bool map : {true, false})
	{
		s_noMap = !map;
		std::unique_ptr<FSTDataSourceApp> ds(FSTDataSourceApp::Open(dir, contents));
		CHECK(ds != nullptr);
		if (!ds)
			continue;
		const uint32 reads = s_reads;
		Counting c(*ds);
		// the cluster's offset is not the file's: each file is its cluster
		CHECK(RawReadsBack(c, 0, 0x123456, fst, SECTOR, iv0));
		CHECK(RawReadsBack(c, 1, 0x8000, raw, SECTOR, iv1));
		CHECK(HashedReadsBack(c, 2, 0x10000, hashed));
		// mapped, every block decrypted from the pages
		CHECK(map ? c.read == 0 && s_reads == reads : c.read > 0 && s_reads > reads);
		// lent where it lies, mapped
		const uint8* p = ds->Lend(1, 0, 0x100, 0x2000);
		CHECK(map ? p && memcmp(p, raw.cipher.data() + 0x100, 0x2000) == 0 : !p);
		CHECK(!ds->Lend(1, 0, raw.cipher.size() - 4, 8));
		CHECK(!ds->Lend(3, 0, 0, 1));
		// read, from the pages or the file, short at the end
		std::vector<uint8> got(0x300);
		CHECK(ds->readData(2, 0, hashed.cipher.size() - 0x100, got.data(), got.size()) == 0x100);
		CHECK(memcmp(got.data(), hashed.cipher.data() + hashed.cipher.size() - 0x100, 0x100) == 0);
		CHECK(ds->readData(2, 0, hashed.cipher.size(), got.data(), 1) == 0);
		CHECK(ds->readData(7, 0, 0, got.data(), 1) == 0);
	}
	s_noMap = false;

	// a file missing, a cluster twice
	const FSTDataSourceApp::Content missing[] = {{0, 0x0}, {1, 0xc}};
	CHECK(FSTDataSourceApp::Open(dir, missing) == nullptr);
	const FSTDataSourceApp::Content twice[] = {{0, 0x0}, {0, 0xa}};
	CHECK(FSTDataSourceApp::Open(dir, twice) == nullptr);
	fs::remove_all(dir);
}

// --- a homebrew bundle -------------------------------------------------------

static void PutBE(std::vector<uint8>& v, size_t at, uint64 x, int bytes)
{
	for (int i = 0; i < bytes; i++)
		v[at + i] = (uint8)(x >> (8 * (bytes - 1 - i)));
}

static uint32 RomfsHash(uint32 parent, std::string_view name)
{
	uint32 hash = parent ^ 123456789;
	for (char ch : name)
	{
		unsigned char c = (unsigned char)ch;
		if (c >= 'a' && c <= 'z')
			c = c + 'A' - 'a';
		hash = (hash >> 5) | (hash << 27);
		hash ^= c;
	}
	return hash;
}

struct RomfsFile
{
	std::string dir;  // "" for the root, else "a/b"
	std::string name;
	std::vector<uint8> data;
};

// A RomFS image as a .wuhb holds one: the root, content, content/sub and
// meta; hash tables of three buckets, so that chains collide. With loop, the
// last entry of every file chain points back at the chain's first.
static std::vector<uint8> MakeWuhb(const std::vector<RomfsFile>& files, bool loop = false)
{
	struct Dir { std::string path, name; uint32 parent; uint32 offset; uint32 hash = 0xFFFFFFFF; };
	std::vector<Dir> dirs = {{"", "", 0}, {"content", "content", 0}, {"content/sub", "sub", 1}, {"meta", "meta", 0}};
	const uint32 BUCKETS = 3;
	auto padded = [](size_t n) { return (n + 3) & ~(size_t)3; };
	uint32 at = 0;
	for (Dir& d : dirs)
	{
		d.offset = at;
		at += (uint32)(24 + padded(d.name.size()));
	}
	const uint32 dirTableSize = at;
	auto dirIndex = [&](const std::string& path) {
		for (size_t i = 0; i < dirs.size(); i++)
			if (dirs[i].path == path)
				return (uint32)i;
		return 0u;
	};
	std::vector<uint32> fileOffset(files.size()), fileData(files.size());
	at = 0;
	uint32 dataAt = 0;
	for (size_t i = 0; i < files.size(); i++)
	{
		fileOffset[i] = at;
		at += (uint32)(32 + padded(files[i].name.size()));
		fileData[i] = dataAt;
		dataAt += (uint32)((files[i].data.size() + 31) & ~(size_t)31);
	}
	const uint32 fileTableSize = at;
	// buckets: heads, and each entry's next in its chain
	std::vector<uint32> dirBucket(BUCKETS, 0xFFFFFFFF), fileBucket(BUCKETS, 0xFFFFFFFF);
	std::vector<uint32> fileNext(files.size(), 0xFFFFFFFF);
	for (size_t i = dirs.size(); i-- > 0;)
	{
		const uint32 b = RomfsHash(dirs[dirs[i].parent].offset, dirs[i].name) % BUCKETS;
		dirs[i].hash = dirBucket[b];
		dirBucket[b] = dirs[i].offset;
	}
	for (size_t i = files.size(); i-- > 0;)
	{
		const uint32 b = RomfsHash(dirs[dirIndex(files[i].dir)].offset, files[i].name) % BUCKETS;
		fileNext[i] = fileBucket[b];
		fileBucket[b] = fileOffset[i];
	}
	if (loop)
	{
		for (size_t i = 0; i < files.size(); i++)
			if (fileNext[i] == 0xFFFFFFFF)
			{
				const uint32 b = RomfsHash(dirs[dirIndex(files[i].dir)].offset, files[i].name) % BUCKETS;
				fileNext[i] = fileBucket[b];
			}
	}

	const size_t header = 0x50;
	const size_t dirHash = header, dirTable = dirHash + BUCKETS * 4;
	const size_t fileHash = dirTable + dirTableSize, fileTable = fileHash + BUCKETS * 4;
	const size_t partition = (fileTable + fileTableSize + 63) & ~(size_t)63;
	std::vector<uint8> img(partition + dataAt);
	memcpy(img.data(), "WUHB", 4);
	PutBE(img, 4, header, 4);
	const uint64 fields[] = {dirHash, BUCKETS * 4, dirTable, dirTableSize, fileHash, BUCKETS * 4, fileTable, fileTableSize, partition};
	for (int i = 0; i < 9; i++)
		PutBE(img, 8 + 8 * i, fields[i], 8);
	for (uint32 b = 0; b < BUCKETS; b++)
	{
		PutBE(img, dirHash + 4 * b, dirBucket[b], 4);
		PutBE(img, fileHash + 4 * b, fileBucket[b], 4);
	}
	for (const Dir& d : dirs)
	{
		const size_t e = dirTable + d.offset;
		PutBE(img, e, dirs[d.parent].offset, 4);
		PutBE(img, e + 4, 0xFFFFFFFF, 4); // sibling
		PutBE(img, e + 8, 0xFFFFFFFF, 4); // child
		PutBE(img, e + 12, 0xFFFFFFFF, 4);
		PutBE(img, e + 16, d.hash, 4);
		PutBE(img, e + 20, d.name.size(), 4);
		memcpy(img.data() + e + 24, d.name.data(), d.name.size());
	}
	// each directory's first file, for a listing
	for (size_t i = files.size(); i-- > 0;)
	{
		const size_t e = dirTable + dirs[dirIndex(files[i].dir)].offset;
		const uint32 head = (uint32)img[e + 12] << 24 | (uint32)img[e + 13] << 16 | (uint32)img[e + 14] << 8 | img[e + 15];
		PutBE(img, fileTable + fileOffset[i] + 4, head, 4);
		PutBE(img, e + 12, fileOffset[i], 4);
	}
	for (size_t i = 0; i < files.size(); i++)
	{
		const size_t e = fileTable + fileOffset[i];
		PutBE(img, e, dirs[dirIndex(files[i].dir)].offset, 4);
		PutBE(img, e + 8, fileData[i], 8);
		PutBE(img, e + 16, files[i].data.size(), 8);
		PutBE(img, e + 24, fileNext[i], 4);
		PutBE(img, e + 28, files[i].name.size(), 4);
		memcpy(img.data() + e + 32, files[i].name.data(), files[i].name.size());
		if (!files[i].data.empty())
			memcpy(img.data() + partition + fileData[i], files[i].data.data(), files[i].data.size());
	}
	return img;
}

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
		s_noMap = !map;
		std::unique_ptr<WUHBReader> reader(WUHBReader::FromPath(path));
		CHECK(reader != nullptr);
		if (!reader)
			continue;
		CHECK(reader->IsMapped() == map);
		const uint32 reads = s_reads;
		bool all = true;
		std::vector<uint8> got(80000);
		for (const RomfsFile& f : files)
		{
			const std::string p = f.dir.empty() ? f.name : f.dir + "/" + f.name;
			const uint32 e = reader->Lookup(p, true);
			all = all && e != ROMFS_ENTRY_EMPTY && reader->GetFileSize(e) == f.data.size();
			if (e == ROMFS_ENTRY_EMPTY)
				continue;
			all = all && reader->Lookup("/" + p, true) == e;
			// a name it begins, and one that begins it, are not it
			all = all && reader->Lookup(p + "x", true) == ROMFS_ENTRY_EMPTY
				&& reader->Lookup(p.substr(0, p.size() - 1), true) == ROMFS_ENTRY_EMPTY;
			all = all && reader->ReadFromFile(e, 0, got.size(), got.data()) == f.data.size()
				&& (f.data.empty() || memcmp(got.data(), f.data.data(), f.data.size()) == 0);
			for (int i = 0; i < 10 && !f.data.empty(); i++)
			{
				const uint64 at = rng() % f.data.size(), len = 1 + rng() % 9000;
				const uint64 want = std::min<uint64>(len, f.data.size() - at);
				all = all && reader->ReadFromFile(e, at, len, got.data()) == want
					&& memcmp(got.data(), f.data.data() + at, want) == 0;
			}
			all = all && reader->ReadFromFile(e, f.data.size(), 1, got.data()) == 0;
			all = all && reader->GetFileEntry(e).name == f.name;
		}
		CHECK(all);
		// directories, and what is not there
		const uint32 sub = reader->Lookup("content/sub", false);
		CHECK(sub != ROMFS_ENTRY_EMPTY && reader->GetDirEntry(sub).name == "sub");
		CHECK(sub != ROMFS_ENTRY_EMPTY && reader->Lookup("content/sub/", false) == sub);
		CHECK(reader->Lookup("content/file9.bin", true) == ROMFS_ENTRY_EMPTY);
		CHECK(reader->Lookup("content/sub/file1.bin", true) == ROMFS_ENTRY_EMPTY);
		CHECK(reader->Lookup("nope/file1.bin", true) == ROMFS_ENTRY_EMPTY);
		CHECK(reader->Lookup("content/file1.bi", true) == ROMFS_ENTRY_EMPTY);
		CHECK(reader->GetFileSize(0x7FFFFFF0) == 0);
		CHECK(reader->ReadFromFile(ROMFS_ENTRY_EMPTY, 0, 1, got.data()) == 0);
		// mapped, nothing read once open
		CHECK(map ? s_reads == reads : s_reads > reads);
	}
	s_noMap = false;

	// a chain that comes back round: what is not there is not found
	WriteFile(path, MakeWuhb(files, true));
	std::unique_ptr<WUHBReader> looped(WUHBReader::FromPath(path));
	CHECK(looped && looped->Lookup("content/file5.bin", true) != ROMFS_ENTRY_EMPTY);
	CHECK(looped && looped->Lookup("content/file9.bin", true) == ROMFS_ENTRY_EMPTY);
	looped.reset();
	// not a bundle
	std::vector<uint8> notOne(0x60);
	WriteFile(path, notOne);
	CHECK(WUHBReader::FromPath(path) == nullptr);
	fs::remove(path);
}

static void TestThreadsUnmapped()
{
	s_noMap = true;
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

	wud_t* wud = wud_open(wudPath);
	wud_t* wx = wud_open(wuxPath);
	const FSTDataSourceApp::Content contents[] = {{0, 5}};
	std::unique_ptr<FSTDataSourceApp> app(FSTDataSourceApp::Open(appDir, contents));
	std::unique_ptr<WUHBReader> wuhb(WUHBReader::FromPath(wuhbPath));
	CHECK(wud && wx && app && wuhb);
	if (wud && wx && app && wuhb)
	{
		CHECK(!wud->mapped && !wx->mapped && !wuhb->IsMapped());
		std::vector<uint32> entries;
		for (const RomfsFile& f : files)
			entries.push_back(wuhb->Lookup("content/" + f.name, true));
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
					if (app->readData(0, 0, at, got.data(), n) != n || memcmp(got.data(), image.data() + at, n) != 0)
						bad++;
					const size_t k = r() % files.size();
					const std::vector<uint8>& fd = files[k].data;
					const uint64 fat = r() % fd.size(), fn = std::min<uint64>(1 + r() % 5000, fd.size() - fat);
					if (wuhb->ReadFromFile(entries[k], fat, fn, got.data()) != fn || memcmp(got.data(), fd.data() + fat, fn) != 0)
						bad++;
				}
			});
		}
		for (std::thread& t : threads)
			t.join();
		CHECK(bad == 0);
		// a stream each at most, for the four
		CHECK(wud->streams->Opened() <= 4 && wx->streams->Opened() <= 4);
	}
	if (wud)
		wud_close(wud);
	if (wx)
		wud_close(wx);
	app.reset();
	wuhb.reset();
	s_noMap = false;
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
