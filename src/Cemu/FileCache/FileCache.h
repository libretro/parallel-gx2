#pragma once

#include <atomic>
#include "util/helpers/MappedFile.h"
#include "util/helpers/MpscList.h"

// A cache file: entries by a two-word name, in the format standalone Cemu
// reads and writes. No lock. Every change to the file - a write, a delete,
// the file table's growth - is made by the one writer thread
// (FileCache_StopAsyncWriter), in the order the changes were asked for; a
// reader looks an entry up in what the writer has published and reads its
// data through a mapping of the file, without waiting for anything.
class FileCache
{
public:
	struct FileName 
	{
		FileName(uint64 name1, uint64 name2) : name1(name1), name2(name2) {};
		FileName(std::string_view filePath)
		{
			// name from string hash
			uint64 h1 = 0xa2cc2c49386a75fdull;
			uint64 h2 = 0x5182d367734c2ce8ull;
			const char* c = filePath.data();
			const char* cEnd = filePath.data() + filePath.size();
			while (c < cEnd)
			{
				uint64 t = (uint64)*c;
				c++;
				h1 = (h1 << 7) | (h1 >> (64 - 7));
				h1 += t;
				h2 = h2 * 7841u + t;
			}
			name1 = h1;
			name2 = h2;
		};

		FileName(const std::string& filePath) : FileName(std::basic_string_view(filePath.data(), filePath.size())) {};

		uint64 name1;
		uint64 name2;
	};

	~FileCache();

	static FileCache* Create(const fs::path& path, uint32 extraVersion = 0);
	static FileCache* Open(const fs::path& path, bool allowCreate, uint32 extraVersion = 0);
	static FileCache* Open(const fs::path& path); // open without extraVersion check

	void UseCompression(bool enable) { enableCompression = enable; };

	// The data is copied and written by the writer thread: found by a read
	// once written, which WaitForWrites waits for. The two are the same.
	void AddFile(const FileName&& name, const uint8* fileData, sint32 fileSize);
	void AddFileAsync(const FileName& name, const uint8* fileData, sint32 fileSize);
	// Whether it was there; gone when this returns (it waits for the writes
	// asked for before it, and for itself: for a load, not a draw)
	bool DeleteFile(const FileName&& name);
	bool GetFile(const FileName&& name, std::vector<uint8>& dataOut);
	bool GetFileByIndex(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut);
	bool HasFile(const FileName&& name);
	// Until every write asked of this cache so far is in the file
	void WaitForWrites();
	// The same: each write leaves nothing in the stream's buffer
	void Flush();

	sint32 GetFileCount();

	sint32 GetMaximumFileIndex();

private:
	struct FileTableEntry
	{
		enum FLAGS : uint8
		{
			FLAG_NONE = 0x00,
			FLAG_COMPRESSED = (1 << 0), // zLib compressed
		};
		uint64 name1;
		uint64 name2;
		uint64 fileOffset;
		uint32 fileSize;
		FLAGS flags;
		uint8 extraReserved1;
		uint8 extraReserved2;
		uint8 extraReserved3;
	};

	static_assert(sizeof(FileTableEntry) == 0x20);

	FileCache() {};
	friend struct _FileCacheAsyncWriter;

	static FileCache* _OpenExisting(const fs::path& path, bool compareExtraVersion, uint32 extraVersion = 0);

	void fileCache_updateFiletable(sint32 extraEntriesToAllocate);
	// the writer thread's
	sint32 _addFileInternal(uint64 name1, uint64 name2, const uint8* fileData, sint32 fileSize, bool noCompression);
	void _deleteFileInternal(uint64 name1, uint64 name2);

	// what the file table holds, kept beside it so that nothing has to scan
	// the table: the entries by name, where the scan for a free entry starts,
	// and the space between the entries' data. Built when the file is opened
	// and kept up to date by every write and delete: the writer thread's.
	void _buildLookup();
	sint32 _findEntry(uint64 name1, uint64 name2) const;
	void _indexInsert(sint32 entryIndex);
	void _indexErase(uint64 name1, uint64 name2);
	sint32 _takeFreeEntry();
	uint64 _allocateSpace(uint64 size);
	void _releaseSpace(uint64 offset, uint64 size);

	struct SpaceGap
	{
		uint64 offset;
		uint64 size;
	};
	std::vector<sint32> m_index; // open addressing, linear probing; -1 is empty
	sint32 m_indexUsed{};
	sint32 m_freeEntryHint{}; // every entry before it is in use
	std::vector<SpaceGap> m_gaps; // sorted by offset, none touching
	uint64 m_dataEnd{}; // where the last entry's data ends

	class FileStream* fileStream{};
	uint64 dataOffset{};
	uint32 extraVersion{};
	// file table
	FileTableEntry* fileTableEntries{};
	sint32 fileTableEntryCount{};
	// file table (as stored in file)
	uint64 fileTableOffset{};
	uint32 fileTableSize{};
	// options
	bool enableCompression{true};

	// What the readers see. An entry of the table, as a record that is never
	// changed: the writer puts a new one in its place, and keeps the old one
	// until the cache goes, as a reader may still be reading it. The data a
	// record points at is not written over before the file is next opened
	// (_releaseSpace): the readers' pointers are into the file itself.
	struct Record
	{
		uint64 name1;
		uint64 name2;
		uint64 offset;
		uint32 size;
		bool compressed;
		uint64 seq; // the write it is from (m_writeSeq)
	};
	// by table index, in chunks that never move
	static constexpr size_t kSlotChunk = 4096;
	static constexpr size_t kSlotChunks = 4096;
	std::atomic<std::atomic<const Record*>*> m_slots[kSlotChunks]{};
	std::atomic<sint32> m_publishedCount{0}; // the table entries readers may look at
	std::atomic<sint32> m_liveCount{0};
	// the table index by name: a name's slot is written once and its index
	// published after it, so a reader finds a name or an empty slot. Grown by
	// publishing a copy twice the size; the old ones are kept until the cache
	// goes
	struct LookupSlot
	{
		std::atomic<uint64> name1{0};
		std::atomic<uint64> name2{0};
		std::atomic<sint32> index{-1};
	};
	struct LookupTable
	{
		size_t mask;
		size_t used;
		std::unique_ptr<LookupSlot[]> slots;
	};
	std::atomic<LookupTable*> m_lookup{nullptr};
	std::vector<LookupTable*> m_retiredLookups; // the writer's
	std::vector<const Record*> m_retiredRecords; // the writer's
	// the file's bytes; a new mapping when a record is past the end of this
	// one, or newer than its copy where the file could not be mapped
	struct Mapping
	{
		Mapping(const fs::path& path, uint64 seq) : file(path), seq(seq) {}
		MappedFile file;
		uint64 seq; // every write up to it is in it
	};
	std::atomic<Mapping*> m_mapping{nullptr};
	MpscList<Mapping*> m_retiredMappings;
	std::atomic<uint64> m_writeSeq{0};
	std::atomic<uint32> m_pendingWrites{0};
	fs::path m_path;

	void _publishInit(); // every entry the file was opened with
	void _publishCount();
	void _publishEntry(sint32 entryIndex, bool named);
	void _unpublishEntry(sint32 entryIndex);
	void _lookupSet(uint64 name1, uint64 name2, sint32 entryIndex);
	const Record* _slot(sint32 entryIndex) const;
	const Record* _findPublished(uint64 name1, uint64 name2) const;
	bool _readRecord(const Record* record, std::vector<uint8>& dataOut);
};

// Writes out queued cache entries and ends the writer thread (it restarts on
// the next write). For a libretro core to call before it is unloaded.
void FileCache_StopAsyncWriter();
