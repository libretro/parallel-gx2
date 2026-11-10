#pragma once
#include "Common/precompiled.h"

struct wud_t;
class FileStream;
class FileStreamPool;

// Where an FST volume's encrypted octets come from: a disc image (.wud, .wux)
// or a title's content files (.app). Each is mapped where it can be, and then
// a block is decrypted straight from the file's pages into the cache that
// keeps it - no read, no buffer, no copy before the decryption, which has to
// write the octets anyway. Where it cannot be (a file only the frontend's VFS
// reaches), the octets are read into that same destination and decrypted
// there.
//
// Read from any number of threads at once: what is mapped is only read, and
// what is not is read through a stream lent to each reader.
class FSTDataSource
{
public:
	virtual ~FSTDataSource() = default;

	// Up to size octets at offset (in the cluster at clusterOffset), read
	// into data; how many there were
	virtual uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) = 0;
	// The same size octets, lent from the file's mapping for as long as the
	// source lives; nullptr where they cannot all be lent in one piece, and
	// readData reads them instead
	virtual const uint8* Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size) { return nullptr; }

	// size octets, lent where they can be and otherwise read into scratch:
	// where they are, or nullptr when not all of them could be had
	const uint8* Fetch(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* scratch, uint64 size);

	// An unhashed block of size octets (a multiple of 16), decrypted into dst
	// with iv; nextIv gets its last 16 encrypted octets, the next block's IV.
	bool ReadRawBlock(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* dst, uint32 size,
		const uint8* key, const uint8* iv, uint8* nextIv);
	// A hashed block, 0x10000 octets: the 0x400 octets of hashes decrypted
	// with no IV, then the 0xFC00 of data with the first 16 octets of the
	// block's H0 hash (h0Index of the 16) as the IV, into dst
	bool ReadHashedBlock(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* dst,
		const uint8* key, uint32 h0Index)
	{
		return ReadHashedBlock(clusterIndex, clusterOffset, offset, dst, dst + HASHED_BLOCK_HASH_SIZE, key, h0Index);
	}
	// the same, the hashes into hashDst and the data into dataDst, apart
	bool ReadHashedBlock(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* hashDst, uint8* dataDst,
		const uint8* key, uint32 h0Index);

	static constexpr uint32 HASHED_BLOCK_SIZE = 0x10000;
	static constexpr uint32 HASHED_BLOCK_HASH_SIZE = 0x400;

protected:
	FSTDataSource() = default;
};

// A disc image, .wud or .wux (wud.h)
class FSTDataSourceWUD : public FSTDataSource
{
public:
	static FSTDataSourceWUD* Open(const fs::path& path);
	~FSTDataSourceWUD() override;

	void SetBaseOffset(uint64 baseOffset) { m_baseOffset = baseOffset; }
	uint64 GetBaseOffset() const { return m_baseOffset; }

	uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) override;
	const uint8* Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size) override;

private:
	FSTDataSourceWUD() = default;
	wud_t* m_wudFile{};
	uint64 m_baseOffset{};
};

// A title's content: one .app file a cluster, each of them mapped
class FSTDataSourceApp : public FSTDataSource
{
public:
	struct Content
	{
		uint16 index;    // the cluster
		uint32 contentId; // its file: <contentId>.app, in hex
	};
	static FSTDataSourceApp* Open(const fs::path& path, std::span<const Content> contents);
	~FSTDataSourceApp() override;

	uint64 readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size) override;
	const uint8* Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size) override;

private:
	struct Cluster
	{
		FileStream* file{};             // the mapping's, where mapped
		std::span<const uint8> mapped;
		FileStreamPool* streams{};      // where not: one for each reader at once
	};
	FSTDataSourceApp() = default;
	std::vector<Cluster> m_cluster;
};
