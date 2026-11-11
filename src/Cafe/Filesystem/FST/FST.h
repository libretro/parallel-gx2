#pragma once
#include "Cemu/ncrypto/ncrypto.h"
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
	bool HasCorruption() const;

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
	struct fst_volume* m_volume{}; // fst_volume.h: the table, the clusters, the reads
	const fst_table_t* m_table{}; // the volume's

	FSTVolume(struct fst_volume* volume);
};
