#pragma once
#include "Cemu/ncrypto/ncrypto.h"
#include "util/crypto/sha.h"
#include "Cafe/Filesystem/FST/fst_table.h"

struct FSTFileHandle
{
	friend class FSTVolume;
private:
	uint32 m_fstIndex;
};

struct FSTDirectoryIterator
{
	friend class FSTVolume;

	const FSTFileHandle& GetDirHandle() const
	{
		return dirHandle;
	}
private:
	FSTFileHandle dirHandle;
	fst_table_dir_t entries;
};

class FSTVolume
{
public:
	enum class ErrorCode
  	{
		OK = 0,
		UNKNOWN_ERROR = 1,
	  	DISC_KEY_MISSING = 2,
	  	TITLE_TIK_MISSING = 3,
	    BAD_TITLE_TMD = 4,
	    BAD_TITLE_TIK = 5,
  	};

	// imageOpened, when asked for, says whether the image itself could be opened
	// and its header read. It separates "this is not a disc image we can read"
	// from "it is, and no key in the cache decrypts it" - a caller that wants to
	// tell the user which of the two happened cannot get that from the return
	// value alone.
	// search, if given, says where the key was looked for (KeyCache.h)
	static bool FindDiscKey(const fs::path& path, NCrypto::AesKey& discTitleKey, bool* imageOpened = nullptr, struct KeyCacheSearch* search = nullptr);

	static FSTVolume* OpenFromDiscImage(const fs::path& path, NCrypto::AesKey& discTitleKey, ErrorCode* errorCodeOut = nullptr);
	static FSTVolume* OpenFromDiscImage(const fs::path& path, ErrorCode* errorCodeOut = nullptr);
	static FSTVolume* OpenFromContentFolder(fs::path folderPath, ErrorCode* errorCodeOut = nullptr);
	// An FST of fstSize octets at fstOffset of cluster 0, in a source of the
	// caller's, decrypted with key: what the reads see, without a title around.
	// The volume takes the source, and closes it on failure too.
	static FSTVolume* OpenFromDataSource(struct fst_source* dataSource, uint64 fstOffset, uint32 fstSize, const NCrypto::AesKey& key);

	~FSTVolume();

	uint32 GetFileCount() const;
	bool HasCorruption() const { return m_detectedCorruption.load(std::memory_order_relaxed); }

	bool OpenFile(std::string_view path, FSTFileHandle& fileHandleOut, bool openOnlyFiles = false);

	// file and directory functions
	bool IsDirectory(const FSTFileHandle& fileHandle) const;
	bool IsFile(const FSTFileHandle& fileHandle) const;
	bool HasLinkFlag(const FSTFileHandle& fileHandle) const;

	std::string_view GetName(const FSTFileHandle& fileHandle) const;
	std::string GetPath(const FSTFileHandle& fileHandle) const;

	// file functions
	uint32 GetFileSize(const FSTFileHandle& fileHandle) const;
	uint32 ReadFile(FSTFileHandle& fileHandle, uint32 offset, uint32 size, void* dataOut);

	// directory iterator
	bool OpenDirectoryIterator(std::string_view path, FSTDirectoryIterator& directoryIteratorOut);
	bool Next(FSTDirectoryIterator& directoryIterator, FSTFileHandle& fileHandleOut);

	// helper function to read whole file
	std::vector<uint8> ExtractFile(std::string_view path, bool* success = nullptr)
	{
		if (success)
			*success = false;
		std::vector<uint8> fileData;
		FSTFileHandle fileHandle;
		if (!OpenFile(path, fileHandle, true))
			return fileData;
		fileData.resize(GetFileSize(fileHandle));
		ReadFile(fileHandle, 0, (uint32)fileData.size(), fileData.data());
		if (success)
			*success = true;
		return fileData;
	}

private:
	/* FST data (in memory) */
	enum class ClusterHashMode : uint8
	{
		RAW = 0, // raw data + encryption, no hashing?
	  	RAW_STREAM = 1, // raw data + encryption, with hash stored in tmd?
		HASH_INTERLEAVED = 2, // hashes + raw interleaved in 0x10000 blocks (0x400 bytes of hashes at the beginning, followed by 0xFC00 bytes of data)
	};

	// what a read of a cluster wants besides what the table says of it
	struct FSTCluster
	{
		uint32 offset; // in sectors
		ClusterHashMode hashMode;
		// extra data if TMD is available
		bool hasContentHash{};
		uint8 contentHash32[32];
		bool contentHashIsSHA1{}; // if true then it's SHA1 (with extra bytes zeroed out), otherwise it's SHA256
		uint64 contentSize{}; // size of the content (in blocks)
		// hash context for single hash mode (content hash must be available)
		std::unique_ptr<ShaStream> singleHashCtx;
		// the next block to hash, its top bit set while a reader hashes it:
		// one reader hashes each block, the others go on
		std::atomic<uint32> singleHashNumBlocksHashed{0};
	};

	struct fst_source* m_dataSource; // fst_source.h
	bool m_sourceIsOwned{};
	uint32 m_sectorSize{}; // for cluster offsets
	fst_table_t* m_table{}; // fst_table.h: the entries, their names, the clusters
	std::unique_ptr<FSTCluster[]> m_cluster;
	NCrypto::AesKey m_partitionTitlekey;
	std::atomic<bool> m_detectedCorruption{false};

	bool HashIsDisabled() const
	{
		return fst_table_hash_disabled(m_table);
	}

	/* Decrypted blocks kept for the reads that follow: a fixed set of slots,
	   made at the first read, so a read allocates nothing. A read that wants
	   a whole block is decrypted straight to where it goes instead. A cache
	   is one reader's while it reads: lent from a pool the volume keeps, with
	   no lock, so any number of threads read the volume at once. */
	struct CacheSlot
	{
		uint64 key{~0ull}; // CacheKey(); ~0 when empty
		uint64 lastAccess{};
		NCrypto::AesIv ivForNextBlock{}; // a raw block's: the next one's IV
	};
	static constexpr uint32 CACHE_SLOTS = 32;
	static constexpr uint32 CACHE_SLOT_SIZE = 0x10000; // a hashed block, or a raw one
	struct BlockCache
	{
		std::unique_ptr<uint8[]> memory;
		CacheSlot slots[CACHE_SLOTS];
		uint64 accessCounter{};
		// the IV of the raw block after the last one decrypted outside it
		uint64 nextIvKey{~0ull};
		NCrypto::AesIv nextIv{};

		CacheSlot* Find(uint64 key);
		// the least recently used slot, emptied, or nullptr when there is no memory
		CacheSlot* Take();
		uint8* Data(const CacheSlot* slot) { return memory.get() + (size_t)(slot - slots) * CACHE_SLOT_SIZE; }
	};
	static constexpr size_t CACHE_POOL = 4;
	std::atomic<BlockCache*> m_caches[CACHE_POOL]{};
	BlockCache* AcquireCache();
	void ReleaseCache(BlockCache* cache);

	static uint64 CacheKey(bool hashed, uint32 clusterIndex, uint32 blockIndex)
	{
		return ((uint64)hashed << 63) | ((uint64)clusterIndex << 32) | blockIndex;
	}

	bool DetermineUnhashedBlockIV(BlockCache& cache, uint32 clusterIndex, uint32 blockIndex, NCrypto::AesIv& ivOut);
	// a raw block (m_sectorSize octets) decrypted into dst and, where the
	// cluster has a content hash and this is the next block of it, hashed
	bool DecryptRawBlock(BlockCache& cache, uint32 clusterIndex, uint32 blockIndex, uint8* dst, NCrypto::AesIv& nextIvOut);
	// a hashed block's hashes (0x400) and data (0xFC00), its data checked
	// against its H0 hash
	bool DecryptHashedBlock(uint32 clusterIndex, uint32 blockIndex, uint8* hashDst, uint8* dataDst);
	// from the cache, decrypted into it first when not there
	const uint8* GetDecryptedRawBlock(BlockCache& cache, uint32 clusterIndex, uint32 blockIndex);
	const uint8* GetDecryptedHashedBlockData(BlockCache& cache, uint32 clusterIndex, uint32 blockIndex);

	/* File reading */
	uint32 ReadFile_HashModeRaw(BlockCache& cache, uint32 clusterIndex, const fst_table_file_t& file, uint32 readOffset, uint32 readSize, void* dataOut);
	uint32 ReadFile_HashModeHashed(BlockCache& cache, uint32 clusterIndex, const fst_table_file_t& file, uint32 readOffset, uint32 readSize, void* dataOut);

	// takeSource: the volume closes the source, also when it does not open
	static FSTVolume* OpenFST(struct fst_source* dataSource, bool takeSource, uint64 fstOffset, uint32 fstSize, NCrypto::AesKey* partitionTitleKey, ClusterHashMode fstHashMode, NCrypto::TMDParser* optionalTMD);
};

class FSTVerifier
{
public:
	static bool VerifyContentFile(class FileStream* fileContent, const NCrypto::AesKey* key, uint32 contentIndex, uint32 contentSize, uint32 contentSizePadded, bool isSHA1, const uint8* tmdContentHash);
	static bool VerifyHashedContentFile(class FileStream* fileContent, const NCrypto::AesKey* key, uint32 contentIndex, uint32 contentSize, uint32 contentSizePadded, bool isSHA1, const uint8* tmdContentHash);

};