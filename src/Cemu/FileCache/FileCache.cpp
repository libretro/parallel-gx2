#include "FileCache.h"
#include "util/helpers/helpers.h"

#include "util/helpers/MpscList.h"
#include "util/helpers/EventWake.h"
#include "util/helpers/Deflate.h"
#include "Common/FileStream.h"

// The shader/PSO gate (test/pso) counts the file table entries a lookup or
// a write looks at; nothing in the core's own build
#ifdef FILECACHE_COUNT_PROBES
std::atomic<uint64> g_fileCacheProbes{0};
#define FILECACHE_PROBE() g_fileCacheProbes.fetch_add(1, std::memory_order_relaxed)
#else
#define FILECACHE_PROBE() ((void)0)
#endif

struct FileCacheAsyncJob
{
	FileCache* fileCache;
	uint64 name1;
	uint64 name2;
	std::vector<uint8> fileData;
};

struct _FileCacheAsyncWriter
{
	// The thread is started by the first write rather than by the constructor.
	// This object is a static, so a constructor-started thread ran from the
	// moment the library was loaded, and the only thing that ever stopped it
	// was the destructor - which in a libretro core runs inside FreeLibrary,
	// under the loader lock, where the join waits for a thread that cannot
	// exit without that same lock. RetroArch then hung for good on closing
	// content (seen with the MinGW build). Stop() is called from retro_deinit
	// instead, before the library goes, and leaves the destructor nothing to do.
	~_FileCacheAsyncWriter()
	{
		Stop();
	}

	// Writes out whatever is queued and ends the thread. A later AddJob starts
	// a new one.
	void Stop()
	{
		m_isRunning.store(false, std::memory_order_seq_cst);
		m_wake.Notify();
		if (m_fileCacheThread.joinable())
			m_fileCacheThread.join();
		m_started.store(false, std::memory_order_release);
	}

	// No lock (a mutex and condition variable before): the jobs go through
	// an MpscList the one writer thread takes them all from at once, and it
	// sleeps on an eventcount the jobs notify
	void AddJob(FileCache* fileCache, const FileCache::FileName& name, const uint8* fileData, sint32 fileSize)
	{
		FileCacheAsyncJob async;
		async.fileCache = fileCache;
		async.name1 = name.name1;
		async.name2 = name.name2;
		async.fileData = { fileData, fileData + fileSize };

		m_pending.fetch_add(1, std::memory_order_relaxed);
		m_writeRequests.Push(std::move(async));
		// the first job since the last Stop starts the thread
		if (!m_started.exchange(true, std::memory_order_acq_rel))
		{
			m_isRunning.store(true, std::memory_order_seq_cst);
			m_fileCacheThread = std::thread(&_FileCacheAsyncWriter::FileCacheThread, this);
		}
		m_wake.Notify();
	}

private:
	void FileCacheThread()
	{
		SetThreadName("fileCache");
		while (true)
		{
			// Asleep until there is a job, or a stop with none left: a stop
			// still writes out everything queued before it
			m_wake.WaitUntil([this] {
				return m_pending.load(std::memory_order_acquire) != 0 || !m_isRunning.load(std::memory_order_seq_cst);
			});
			bool any = false;
			m_writeRequests.TakeAll([&](FileCacheAsyncJob&& entry) {
				any = true;
				m_pending.fetch_sub(1, std::memory_order_relaxed);
				entry.fileCache->AddFile({ entry.name1, entry.name2 }, entry.fileData.data(), (sint32)entry.fileData.size());
			});
			if (!any && !m_isRunning.load(std::memory_order_seq_cst) && m_pending.load(std::memory_order_acquire) == 0)
				return;
		}
	}

	std::thread m_fileCacheThread;
	MpscList<FileCacheAsyncJob> m_writeRequests;
	std::atomic<uint32> m_pending{0};
	EventWake m_wake;
	std::atomic_bool m_isRunning{false};
	std::atomic_bool m_started{false};
}FileCacheAsyncWriter;

void FileCache_StopAsyncWriter()
{
	FileCacheAsyncWriter.Stop();
}

#define FILECACHE_MAGIC_V1					0x8371b694 // used prior to Cemu 1.7.4, only supported caches up to 4GB
#define FILECACHE_MAGIC_V2					0x8371b695 // added support for large caches
#define FILECACHE_MAGIC_V3					0x8371b696 // introduced in Cemu 1.16.0 (non-WIP). Adds zlib compression
#define FILECACHE_HEADER_RESV				128 // number of bytes reserved for the header
#define FILECACHE_FILETABLE_NAME1			0xEFEFEFEFEFEFEFEFULL
#define FILECACHE_FILETABLE_NAME2			0xFEFEFEFEFEFEFEFEULL
#define FILECACHE_FILETABLE_FREE_NAME		0ULL

FileCache* FileCache::Create(const fs::path& path, uint32 extraVersion)
{
	FileStream* fs = FileStream::createFile2(path);
	if (!fs)
	{
		cemuLog_log(LogType::Force, "Failed to create cache file \"{}\"", _pathToUtf8(path));
		return nullptr;
	}
	// init file cache
	auto* fileCache = new FileCache();
	fileCache->fileStream = fs;
	fileCache->dataOffset = FILECACHE_HEADER_RESV;
	fileCache->fileTableEntryCount = 32;
	fileCache->fileTableOffset = 0;
	fileCache->fileTableSize = sizeof(FileTableEntry) * fileCache->fileTableEntryCount;
	fileCache->fileTableEntries = (FileTableEntry*)malloc(fileCache->fileTableSize);
	fileCache->extraVersion = extraVersion;
	memset(fileCache->fileTableEntries, 0, fileCache->fileTableSize);
	// file table stores info about itself
	fileCache->fileTableEntries[0].name1 = FILECACHE_FILETABLE_NAME1;
	fileCache->fileTableEntries[0].name2 = FILECACHE_FILETABLE_NAME2;
	fileCache->fileTableEntries[0].fileOffset = fileCache->fileTableOffset;
	fileCache->fileTableEntries[0].fileSize = fileCache->fileTableSize;
	// write header

	fs->writeU32(FILECACHE_MAGIC_V3);
	fs->writeU32(fileCache->extraVersion);
	fs->writeU64(fileCache->dataOffset);
	fs->writeU64(fileCache->fileTableOffset);
	fs->writeU32(fileCache->fileTableSize);
	// write file table
	fs->SetPosition(fileCache->dataOffset+fileCache->fileTableOffset);
	fs->writeData(fileCache->fileTableEntries, fileCache->fileTableSize);
	fileCache->_buildLookup();
	// done
	return fileCache;
}

FileCache* FileCache::_OpenExisting(const fs::path& path, bool compareExtraVersion, uint32 extraVersion)
{
	FileStream* fs = FileStream::openFile2(path, true);
	if (!fs)
		return nullptr;
	// read header
	uint32 headerMagic = 0;
	fs->readU32(headerMagic);
	bool isV2 = false;
	if (headerMagic != FILECACHE_MAGIC_V1 && headerMagic != FILECACHE_MAGIC_V2 && headerMagic != FILECACHE_MAGIC_V3)
	{
		delete fs;
		return nullptr;
	}
	if (headerMagic == FILECACHE_MAGIC_V1)
	{
		// support for V1 file format removed with the addition of V3
		delete fs;
		return nullptr;
	}
	if (headerMagic == FILECACHE_MAGIC_V2)
		isV2 = true;
	uint32 headerExtraVersion = 0xFFFFFFFF;
	fs->readU32(headerExtraVersion);
	if (compareExtraVersion && headerExtraVersion != extraVersion)
	{
		delete fs;
		return nullptr;
	}
	if (!compareExtraVersion)
	{
		extraVersion = headerExtraVersion;
	}

	uint64 headerDataOffset = 0;
	uint64 headerFileTableOffset = 0;
	uint32 headerFileTableSize = 0;
	fs->readU64(headerDataOffset);
	fs->readU64(headerFileTableOffset);
	if (!fs->readU32(headerFileTableSize))
	{
		cemuLog_log(LogType::Force, "\"{}\" is corrupted", _pathToUtf8(path));
		delete fs;
		return nullptr;
	}
	uint32 fileTableEntryCount = 0;
	bool invalidFileTableSize = false;
	// V2 and V3
	fileTableEntryCount = headerFileTableSize / sizeof(FileTableEntry);
	invalidFileTableSize = (headerFileTableSize % sizeof(FileTableEntry)) != 0;
	if (invalidFileTableSize)
	{
		cemuLog_log(LogType::Force, "\"{}\" is corrupted", _pathToUtf8(path));
		delete fs;
		return nullptr;
	}
	// init struct
	auto* fileCache = new FileCache();
	fileCache->fileStream = fs;
	fileCache->extraVersion = extraVersion;
	fileCache->dataOffset = headerDataOffset;
	fileCache->fileTableEntryCount = fileTableEntryCount;
	fileCache->fileTableOffset = headerFileTableOffset;
	fileCache->fileTableSize = fileTableEntryCount * sizeof(FileTableEntry);
	fileCache->fileTableEntries = (FileTableEntry*)malloc(fileTableEntryCount * sizeof(FileTableEntry));
	memset(fileCache->fileTableEntries, 0, fileTableEntryCount * sizeof(FileTableEntry));
	// read file table
	fileCache->fileStream->SetPosition(fileCache->dataOffset + fileCache->fileTableOffset);
	bool incompleteFileTable = false;
	if (isV2)
	{
		// read file table entries in old format
		incompleteFileTable = fileCache->fileStream->readData(fileCache->fileTableEntries, fileCache->fileTableSize) != fileCache->fileTableSize;
		// in V2 the extra field wasn't guaranteed to have well defined values
		for (uint32 i = 0; i < fileTableEntryCount; i++)
		{
			fileCache->fileTableEntries[i].flags = FileTableEntry::FLAGS::FLAG_NONE;
			fileCache->fileTableEntries[i].extraReserved1 = 0;
			fileCache->fileTableEntries[i].extraReserved2 = 0;
			fileCache->fileTableEntries[i].extraReserved3 = 0;
		}
	}
	else
	{
		incompleteFileTable = fileCache->fileStream->readData(fileCache->fileTableEntries, fileCache->fileTableSize) != fileCache->fileTableSize;
	}
	if (incompleteFileTable)
	{
		cemuLog_log(LogType::Force, "\"{}\" is corrupted (incomplete file table)", _pathToUtf8(path));
		delete fileCache;
		return nullptr;
	}
	fileCache->_buildLookup();
	return fileCache;
}

FileCache* FileCache::Open(const fs::path& path, bool allowCreate, uint32 extraVersion)
{
	FileCache* fileCache = _OpenExisting(path, true, extraVersion);
	if (fileCache)
		return fileCache;
	if (!allowCreate)
		return nullptr;
	return Create(path, extraVersion);
}

FileCache* FileCache::Open(const fs::path& path)
{
	return _OpenExisting(path, false, 0);
}

FileCache::~FileCache()
{
	free(this->fileTableEntries);
	delete fileStream;
}

void FileCache::fileCache_updateFiletable(sint32 extraEntriesToAllocate)
{
	// recreate file table with bigger size (optional)
	// Entry 0 keeps the table's name: the write below finds it by that name,
	// puts the new table where nothing is (the old one is still in use until
	// the header points past it) and only then gives the old table's space up
	sint32 newFileTableEntryCount = this->fileTableEntryCount + extraEntriesToAllocate;
	this->fileTableEntries = (FileTableEntry*)realloc(this->fileTableEntries, sizeof(FileTableEntry)*newFileTableEntryCount);
	for (sint32 f = this->fileTableEntryCount; f < newFileTableEntryCount; f++)
	{
		this->fileTableEntries[f].name1 = FILECACHE_FILETABLE_FREE_NAME;
		this->fileTableEntries[f].name2 = FILECACHE_FILETABLE_FREE_NAME;
		this->fileTableEntries[f].fileOffset = 0;
		this->fileTableEntries[f].fileSize = 0;
		this->fileTableEntries[f].flags = FileTableEntry::FLAGS::FLAG_NONE;
		this->fileTableEntries[f].extraReserved1 = 0;
		this->fileTableEntries[f].extraReserved2 = 0;
		this->fileTableEntries[f].extraReserved3 = 0;
	}
	this->fileTableEntryCount = newFileTableEntryCount;
	this->_addFileInternal(FILECACHE_FILETABLE_NAME1, FILECACHE_FILETABLE_NAME2, (uint8*)this->fileTableEntries, sizeof(FileTableEntry)*newFileTableEntryCount, true);
	// update file table info in struct
	if (this->fileTableEntries[0].name1 != FILECACHE_FILETABLE_NAME1 || this->fileTableEntries[0].name2 != FILECACHE_FILETABLE_NAME2)
	{
		cemuLog_log(LogType::Force, "Corruption in cache file detected");
		assert_dbg();
	}
	this->fileTableOffset = this->fileTableEntries[0].fileOffset;
	this->fileTableSize = this->fileTableEntries[0].fileSize;
	// the copy of entry 0 inside the table just written still says where the
	// old table was
	fileStream->SetPosition(this->dataOffset + this->fileTableOffset);
	fileStream->writeData(this->fileTableEntries, sizeof(FileTableEntry));
	// update header
	fileStream->SetPosition(0);
	fileStream->writeU32(FILECACHE_MAGIC_V3);
	fileStream->writeU32(this->extraVersion);
	fileStream->writeU64(this->dataOffset);
	fileStream->writeU64(this->fileTableOffset);
	fileStream->writeU32(this->fileTableSize);
}

uint8* _fileCache_compressFileData(const uint8* fileData, uint32 fileSize, sint32& compressedSize)
{
	// compress data as a zlib stream
	// stores the size of the uncompressed file in the first 4 bytes
	size_t compressedLen = Deflate::CompressBound(fileSize);
	uint8* compressedData = (uint8*)malloc(4 + compressedLen);
	if (!compressedData || !Deflate::Compress(compressedData + 4, compressedLen, fileData, fileSize, 4)) // level 4 has good compression to performance ratio
	{
		free(compressedData);
		return nullptr;
	}
	compressedData[0] = ((uint32)fileSize >> 24) & 0xFF;
	compressedData[1] = ((uint32)fileSize >> 16) & 0xFF;
	compressedData[2] = ((uint32)fileSize >> 8) & 0xFF;
	compressedData[3] = ((uint32)fileSize >> 0) & 0xFF;
	compressedSize = (sint32)(4 + compressedLen);
	return compressedData;
}

bool _uncompressFileData(const uint8* rawData, size_t rawSize, std::vector<uint8>& dataOut)
{
	if (rawSize < 4)
	{
		dataOut.clear();
		return false;
	}
	// get size of uncompressed file
	uint32 fileSize = 0;
	fileSize |= ((uint32)rawData[0] << 24);
	fileSize |= ((uint32)rawData[1] << 16);
	fileSize |= ((uint32)rawData[2] << 8);
	fileSize |= ((uint32)rawData[3] << 0);
	// allocate buffer
	const uint8* compressedInput = rawData + 4;
	size_t compressedLen = rawSize - 4;
	size_t uncompressedLen = fileSize;
	dataOut.resize(fileSize);
	if (Deflate::Uncompress(dataOut.data(), uncompressedLen, compressedInput, compressedLen) != Deflate::Result::Ok)
	{
		dataOut.clear();
		return false;
	}
	if (uncompressedLen != fileSize || compressedLen != (rawSize - 4))
	{
		// uncompressed size does not match stored size
		dataOut.clear();
		return false;
	}
	return true;
}

void FileCache::_addFileInternal(uint64 name1, uint64 name2, const uint8* fileData, sint32 fileSize, bool noCompression)
{
	if (fileSize < 0)
		return;
	if (!enableCompression)
		noCompression = true;
	// compress data
	sint32 rawSize = 0;
	uint8* rawData = nullptr;
	bool isCompressed = false;
	if (noCompression)
	{
		rawData = (uint8*)fileData;
		rawSize = fileSize;
	}
	else
	{
		// compress file
		rawData = _fileCache_compressFileData(fileData, fileSize, rawSize);
		if (rawData)
		{
			isCompressed = true;
		}
		else
		{
			rawData = (uint8*)fileData;
			rawSize = fileSize;
		}
	}
	std::unique_lock lock(this->mutex);
	// the entry already under this name, else a free one
	sint32 entryIndex = _findEntry(name1, name2);
	const bool replacing = entryIndex >= 0;
	if (!replacing)
	{
		entryIndex = _takeFreeEntry();
		if (entryIndex < 0)
		{
			if (name1 == FILECACHE_FILETABLE_NAME1 && name2 == FILECACHE_FILETABLE_NAME2)
			{
				cemuLog_log(LogType::Force, "Error in cache file");
				cemu_assert_debug(false);
			}
			// no free entry: a table twice as big (64 entries more at a
			// time made the writes of a large cache quadratic, and left a
			// trail of old tables behind in the file)
			fileCache_updateFiletable(std::max<sint32>(64, this->fileTableEntryCount));
			entryIndex = _takeFreeEntry();
		}
	}
	// the data goes where nothing is: what it replaces stays in use until
	// the entry points away from it
	const uint64 replacedOffset = this->fileTableEntries[entryIndex].fileOffset;
	const uint64 replacedSize = replacing ? this->fileTableEntries[entryIndex].fileSize : 0;
	const uint64 currentStartOffset = _allocateSpace((uint64)rawSize);
	// update file table entry
	this->fileTableEntries[entryIndex].name1 = name1;
	this->fileTableEntries[entryIndex].name2 = name2;
	this->fileTableEntries[entryIndex].fileOffset = currentStartOffset;
	this->fileTableEntries[entryIndex].fileSize = rawSize;
	this->fileTableEntries[entryIndex].flags = isCompressed ? FileTableEntry::FLAGS::FLAG_COMPRESSED : FileTableEntry::FLAGS::FLAG_NONE;
	this->fileTableEntries[entryIndex].extraReserved1 = 0;
	this->fileTableEntries[entryIndex].extraReserved2 = 0;
	this->fileTableEntries[entryIndex].extraReserved3 = 0;
	if (!replacing)
		_indexInsert(entryIndex);
	// write file data
	fileStream->SetPosition(this->dataOffset + currentStartOffset);
	fileStream->writeData(rawData, rawSize);
#ifdef __APPLE__
    fileStream->Flush();
#endif
	// write file table entry
	fileStream->SetPosition(this->dataOffset + this->fileTableOffset + (uint64)(sizeof(FileTableEntry)*entryIndex));
	fileStream->writeData(this->fileTableEntries + entryIndex, sizeof(FileTableEntry));
#ifdef __APPLE__
    fileStream->Flush();
#endif
	_releaseSpace(replacedOffset, replacedSize);
	if (isCompressed)
		free(rawData);
}

void FileCache::AddFile(const FileName&& name, const uint8* fileData, sint32 fileSize)
{
	this->_addFileInternal(name.name1, name.name2, fileData, fileSize, false);
}

void FileCache::Flush()
{
#ifndef _WIN32
	// Windows' FileStream writes with WriteFile, which buffers nothing in the
	// process, and has no Flush
	std::unique_lock lock(this->mutex);
	if (fileStream)
		fileStream->Flush();
#endif
}

bool FileCache::DeleteFile(const FileName&& name)
{
	if( name.name1 == FILECACHE_FILETABLE_NAME1 && name.name2 == FILECACHE_FILETABLE_NAME2 )
		return false; // prevent filetable from being deleted
	std::unique_lock lock(this->mutex);
	sint32 entryIndex = _findEntry(name.name1, name.name2);
	if (entryIndex < 0)
		return false;
	FileTableEntry* entry = this->fileTableEntries + entryIndex;
	const uint64 offset = entry->fileOffset;
	const uint64 size = entry->fileSize;
	_indexErase(name.name1, name.name2);
	entry->name1 = FILECACHE_FILETABLE_FREE_NAME;
	entry->name2 = FILECACHE_FILETABLE_FREE_NAME;
	entry->fileOffset = 0;
	entry->fileSize = 0;
	// store updated entry to file cache
	fileStream->SetPosition(this->dataOffset+this->fileTableOffset+(uint64)(sizeof(FileTableEntry)*entryIndex));
	fileStream->writeData(this->fileTableEntries+entryIndex, sizeof(FileTableEntry));
	_releaseSpace(offset, size);
	m_freeEntryHint = std::min(m_freeEntryHint, entryIndex);
	return true;
}

void FileCache::AddFileAsync(const FileName& name, const uint8* fileData, sint32 fileSize)
{
	FileCacheAsyncWriter.AddJob(this, name, fileData, fileSize);
}

bool FileCache::_getFileDataInternal(const FileTableEntry* entry, std::vector<uint8>& dataOut)
{
	fileStream->SetPosition(this->dataOffset + entry->fileOffset);
	if ((entry->flags&FileTableEntry::FLAG_COMPRESSED) == 0)
	{
		// stored as is: read straight into the caller's buffer
		dataOut.resize(entry->fileSize);
		if (fileStream->readData(dataOut.data(), entry->fileSize) != entry->fileSize)
		{
			dataOut.clear();
			return false;
		}
		return true;
	}
	// The compressed bytes go into a buffer this thread keeps (a new one
	// for every entry was an allocation and a free per read) and inflate
	// from there into the caller's buffer. One that grew for a very large
	// entry is not kept.
	thread_local std::vector<uint8> s_compressed;
	s_compressed.resize(entry->fileSize);
	bool ok = fileStream->readData(s_compressed.data(), entry->fileSize) == entry->fileSize &&
		_uncompressFileData(s_compressed.data(), entry->fileSize, dataOut);
	if (s_compressed.capacity() > 4 * 1024 * 1024)
		std::vector<uint8>().swap(s_compressed);
	if (!ok)
	{
		dataOut.clear();
		return false;
	}
	return true;
}

bool FileCache::GetFile(const FileName&& name, std::vector<uint8>& dataOut)
{
	std::unique_lock lock(this->mutex);
	sint32 entryIndex = _findEntry(name.name1, name.name2);
	if (entryIndex < 0)
	{
		dataOut.clear();
		return false;
	}
	return _getFileDataInternal(this->fileTableEntries + entryIndex, dataOut);
}

bool FileCache::GetFileByIndex(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut)
{
	// under the lock from the start: a write can grow (and move) the table
	std::unique_lock lock(this->mutex);
	if (index < 0 || index >= this->fileTableEntryCount)
		return false;
	FileTableEntry* entry = this->fileTableEntries + index;
	if (this->fileTableEntries == nullptr)
	{
		cemuLog_log(LogType::Force, "GetFileByIndex() fileTable is NULL");
		return false;
	}
	if (entry->name1 == FILECACHE_FILETABLE_FREE_NAME && entry->name2 == FILECACHE_FILETABLE_FREE_NAME)
		return false;
	if (entry->name1 == FILECACHE_FILETABLE_NAME1 && entry->name2 == FILECACHE_FILETABLE_NAME2)
		return false;

	if(name1)
		*name1 = entry->name1;
	if(name2)
		*name2 = entry->name2;
	return _getFileDataInternal(entry, dataOut);
}

bool FileCache::HasFile(const FileName&& name)
{
	std::unique_lock lock(this->mutex);
	return _findEntry(name.name1, name.name2) >= 0;
}

static inline bool _isFreeEntryName(uint64 name1, uint64 name2)
{
	return name1 == FILECACHE_FILETABLE_FREE_NAME && name2 == FILECACHE_FILETABLE_FREE_NAME;
}

static inline uint64 _entryNameHash(uint64 name1, uint64 name2)
{
	// the names are hashes already, but of paths, and some callers number
	// them instead: mixed so that neither ends up in a run of slots
	uint64 h = name1 * 0x9E3779B97F4A7C15ull ^ (name2 + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
	return h ^ (h >> 29);
}

void FileCache::_buildLookup()
{
	m_index.assign(64, -1);
	m_indexUsed = 0;
	m_freeEntryHint = 0;
	m_gaps.clear();
	m_dataEnd = 0;
	// the space in use: every entry's data, and the table itself, where the
	// header says it is (whatever entry 0 says)
	std::vector<SpaceGap> used;
	used.reserve(this->fileTableEntryCount + 1);
	used.push_back({ this->fileTableOffset, this->fileTableSize });
	bool hint = false;
	for (sint32 i = 0; i < this->fileTableEntryCount; i++)
	{
		const FileTableEntry& e = this->fileTableEntries[i];
		if (_isFreeEntryName(e.name1, e.name2))
		{
			if (!hint)
				m_freeEntryHint = i;
			hint = true;
			continue;
		}
		// the first of two entries under one name is the one a scan found
		if (_findEntry(e.name1, e.name2) < 0)
			_indexInsert(i);
		used.push_back({ e.fileOffset, e.fileSize });
	}
	if (!hint)
		m_freeEntryHint = this->fileTableEntryCount;
	std::sort(used.begin(), used.end(), [](const SpaceGap& a, const SpaceGap& b) { return a.offset < b.offset; });
	for (const SpaceGap& u : used)
	{
		if (u.size == 0)
			continue;
		if (u.offset > m_dataEnd)
			m_gaps.push_back({ m_dataEnd, u.offset - m_dataEnd });
		m_dataEnd = std::max(m_dataEnd, u.offset + u.size);
	}
}

sint32 FileCache::_findEntry(uint64 name1, uint64 name2) const
{
	if (_isFreeEntryName(name1, name2) || m_index.empty())
		return -1;
	const size_t mask = m_index.size() - 1;
	for (size_t slot = _entryNameHash(name1, name2) & mask;; slot = (slot + 1) & mask)
	{
		FILECACHE_PROBE();
		sint32 i = m_index[slot];
		if (i < 0)
			return -1;
		if (this->fileTableEntries[i].name1 == name1 && this->fileTableEntries[i].name2 == name2)
			return i;
	}
}

void FileCache::_indexInsert(sint32 entryIndex)
{
	if ((size_t)(m_indexUsed + 1) * 2 > m_index.size())
	{
		// twice the slots, the entries put in again
		std::vector<sint32> old(m_index.size() * 2, -1);
		old.swap(m_index);
		const size_t mask = m_index.size() - 1;
		for (sint32 i : old)
		{
			if (i < 0)
				continue;
			size_t slot = _entryNameHash(this->fileTableEntries[i].name1, this->fileTableEntries[i].name2) & mask;
			while (m_index[slot] >= 0)
				slot = (slot + 1) & mask;
			m_index[slot] = i;
		}
	}
	const FileTableEntry& e = this->fileTableEntries[entryIndex];
	const size_t mask = m_index.size() - 1;
	size_t slot = _entryNameHash(e.name1, e.name2) & mask;
	while (m_index[slot] >= 0)
	{
		FILECACHE_PROBE();
		slot = (slot + 1) & mask;
	}
	m_index[slot] = entryIndex;
	m_indexUsed++;
}

void FileCache::_indexErase(uint64 name1, uint64 name2)
{
	const size_t mask = m_index.size() - 1;
	size_t slot = _entryNameHash(name1, name2) & mask;
	while (true)
	{
		sint32 i = m_index[slot];
		if (i < 0)
			return;
		if (this->fileTableEntries[i].name1 == name1 && this->fileTableEntries[i].name2 == name2)
			break;
		slot = (slot + 1) & mask;
	}
	// moves back the slots after it that would no longer be found (no
	// tombstones to pile up)
	size_t hole = slot;
	for (size_t next = (hole + 1) & mask; m_index[next] >= 0; next = (next + 1) & mask)
	{
		const FileTableEntry& e = this->fileTableEntries[m_index[next]];
		size_t home = _entryNameHash(e.name1, e.name2) & mask;
		// it stays if its home is cyclically within (hole, next]
		bool stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
		if (stays)
			continue;
		m_index[hole] = m_index[next];
		hole = next;
	}
	m_index[hole] = -1;
	m_indexUsed--;
}

sint32 FileCache::_takeFreeEntry()
{
	for (sint32 i = m_freeEntryHint; i < this->fileTableEntryCount; i++)
	{
		FILECACHE_PROBE();
		if (_isFreeEntryName(this->fileTableEntries[i].name1, this->fileTableEntries[i].name2))
		{
			m_freeEntryHint = i + 1;
			return i;
		}
	}
	m_freeEntryHint = this->fileTableEntryCount;
	return -1;
}

uint64 FileCache::_allocateSpace(uint64 size)
{
	if (size == 0)
		return m_dataEnd;
	// the first gap it fits in, else the end
	for (size_t g = 0; g < m_gaps.size(); g++)
	{
		FILECACHE_PROBE();
		if (m_gaps[g].size < size)
			continue;
		uint64 offset = m_gaps[g].offset;
		m_gaps[g].offset += size;
		m_gaps[g].size -= size;
		if (m_gaps[g].size == 0)
			m_gaps.erase(m_gaps.begin() + g);
		return offset;
	}
	uint64 offset = m_dataEnd;
	m_dataEnd += size;
	return offset;
}

void FileCache::_releaseSpace(uint64 offset, uint64 size)
{
	if (size == 0)
		return;
	auto it = std::lower_bound(m_gaps.begin(), m_gaps.end(), offset, [](const SpaceGap& g, uint64 o) { return g.offset < o; });
	it = m_gaps.insert(it, { offset, size });
	// joined with the gaps it touches
	if (it + 1 != m_gaps.end() && it->offset + it->size == (it + 1)->offset)
	{
		it->size += (it + 1)->size;
		m_gaps.erase(it + 1);
	}
	if (it != m_gaps.begin() && (it - 1)->offset + (it - 1)->size == it->offset)
	{
		(it - 1)->size += it->size;
		it = m_gaps.erase(it) - 1;
	}
	// a gap the data ends with is no gap
	if (it->offset + it->size == m_dataEnd)
	{
		m_dataEnd = it->offset;
		m_gaps.erase(it);
	}
}

sint32 FileCache::GetMaximumFileIndex()
{
	return this->fileTableEntryCount;
}

sint32 FileCache::GetFileCount()
{
	std::unique_lock lock(this->mutex);
	sint32 fileCount = 0;
	FileTableEntry* entry = this->fileTableEntries;
	FileTableEntry* entryLast = this->fileTableEntries+this->fileTableEntryCount;
	while( entry < entryLast )
	{
		if( entry->name1 == FILECACHE_FILETABLE_FREE_NAME && entry->name2 == FILECACHE_FILETABLE_FREE_NAME )
		{
			entry++;
			continue;
		}
		if( entry->name1 == FILECACHE_FILETABLE_NAME1 && entry->name2 == FILECACHE_FILETABLE_NAME2 )
		{
			entry++;
			continue;
		}
		fileCount++;
		entry++;
	}

	return fileCount;
}

void fileCache_test()
{
	FileCache* fc = FileCache::Create("testCache.bin", 0);
	uint32 time1 = GetTickCount();

	char* testString1 = (char*)malloc(1024 * 1024 * 8);
	char* testString2 = (char*)malloc(1024 * 1024 * 8);
	char* testString3 = (char*)malloc(1024 * 1024 * 8);
	for (sint32 f = 0; f < 1024 * 1024 * 8; f++)
	{
		testString1[f] = 'a' + (f & 7);
		testString2[f] = 'd' + (f & 3);
		testString3[f] = 'f' + (f & 3);
	}

	for(sint32 i=0; i<2200; i++)
	{
		fc->AddFile({ 0x1000001ULL, (uint64)i }, (uint8*)testString1, 1024 * 1024 * 1);
		fc->AddFile({ 0x1000002ULL, (uint64)i }, (uint8*)testString2, 1024 * 1024 * 1);
		fc->AddFile({ 0x1000003ULL, (uint64)i }, (uint8*)testString3, 1024 * 1024 * 1);
	}
	uint32 time2 = GetTickCount();
	debug_printf("Writing took %dms\n", time2-time1);
	delete fc;
	// verify if all entries are still valid
	FileCache* fcRead = FileCache::Open("testCache.bin", 0);
	uint32 time3 = GetTickCount();
	for(sint32 i=0; i<2200; i++)
	{
		std::vector<uint8> fileData;
		bool r = fcRead->GetFile({ 0x1000001ULL, (uint64)i }, fileData);
		if (!r || fileData.size() != 1024 * 1024 * 1 || memcmp(fileData.data(), testString1, 1024 * 1024 * 1) != 0)
			cemu_assert_debug(false);
		r = fcRead->GetFile({ 0x1000002ULL, (uint64)i }, fileData);
		if( !r || fileData.size() != 1024 * 1024 * 1 || memcmp(fileData.data(), testString2, 1024 * 1024 * 1) != 0 )
			cemu_assert_debug(false);
		r = fcRead->GetFile({ 0x1000003ULL, (uint64)i }, fileData);
		if( !r || fileData.size() != 1024 * 1024 * 1 || memcmp(fileData.data(), testString3, 1024 * 1024 * 1) != 0 )
			cemu_assert_debug(false);
	}
	uint32 time4 = GetTickCount();
	debug_printf("Reading took %dms\n", time4-time3);
	delete fcRead;
	cemu_assert_debug(false);
	exit(0);
}
