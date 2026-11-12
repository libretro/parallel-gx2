#pragma once

#include "Cemu/FileCache/file_cache.h"
#include "Common/FileStream.h"

// A cache file: entries by a two-word name, in the format standalone Cemu
// reads and writes, over the C (file_cache.h). No lock: every change to the
// file is made by the one writer thread (FileCache_StopAsyncWriter), in the
// order the changes were asked for; a reader looks an entry up in what the
// writer has published and reads its data through a mapping of the file,
// without waiting for anything.
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

	~FileCache() { file_cache_close(m_fc); }
	FileCache(const FileCache&) = delete;
	FileCache& operator=(const FileCache&) = delete;

	static FileCache* Create(const fs::path& path, uint32 extraVersion = 0)
	{
		file_cache_t* fc = Ready() ? file_cache_create(FileStream::HostPath(path).c_str(), extraVersion) : nullptr;
		if (!fc)
			cemuLog_log(LogType::Force, "Failed to create cache file \"{}\"", _pathToUtf8(path));
		return fc ? new FileCache(fc) : nullptr;
	}

	static FileCache* Open(const fs::path& path, bool allowCreate, uint32 extraVersion = 0)
	{
		bool corrupt = false;
		file_cache_t* fc = Ready() ? file_cache_open(FileStream::HostPath(path).c_str(), allowCreate, extraVersion, &corrupt) : nullptr;
		if (corrupt)
			cemuLog_log(LogType::Force, "\"{}\" is corrupted", _pathToUtf8(path));
		return fc ? new FileCache(fc) : nullptr;
	}

	// open without the extra version checked
	static FileCache* Open(const fs::path& path)
	{
		bool corrupt = false;
		file_cache_t* fc = Ready() ? file_cache_open_any(FileStream::HostPath(path).c_str(), &corrupt) : nullptr;
		if (corrupt)
			cemuLog_log(LogType::Force, "\"{}\" is corrupted", _pathToUtf8(path));
		return fc ? new FileCache(fc) : nullptr;
	}

	void UseCompression(bool enable) { file_cache_use_compression(m_fc, enable); }

	// The data is copied and written by the writer thread: found by a read
	// once written, which WaitForWrites waits for. The two are the same.
	void AddFile(const FileName&& name, const uint8* fileData, sint32 fileSize) { AddFileAsync(name, fileData, fileSize); }
	void AddFileAsync(const FileName& name, const uint8* fileData, sint32 fileSize)
	{
		if (fileSize >= 0)
			file_cache_add(m_fc, name.name1, name.name2, fileData, (size_t)fileSize);
	}
	// Whether it was there; gone when this returns (it waits for the writes
	// asked for before it, and for itself: for a load, not a draw)
	bool DeleteFile(const FileName&& name) { return file_cache_delete(m_fc, name.name1, name.name2); }
	bool GetFile(const FileName&& name, std::vector<uint8>& dataOut)
	{
		if (file_cache_get(m_fc, name.name1, name.name2, &Resize, &dataOut))
			return true;
		dataOut.clear();
		return false;
	}
	bool GetFileByIndex(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut)
	{
		if (file_cache_get_index(m_fc, index, name1, name2, &Resize, &dataOut))
			return true;
		dataOut.clear();
		return false;
	}
	bool HasFile(const FileName&& name) { return file_cache_has(m_fc, name.name1, name.name2); }
	// Until every write asked of this cache so far is in the file
	void WaitForWrites() { file_cache_wait(m_fc); }
	// The same: each write leaves nothing in the stream's buffer
	void Flush() { file_cache_wait(m_fc); }

	sint32 GetFileCount() { return file_cache_count(m_fc); }
	sint32 GetMaximumFileIndex() { return file_cache_max_index(m_fc); }

private:
	explicit FileCache(file_cache_t* fc) : m_fc(fc) {}

	static bool Ready()
	{
		static const bool ready = file_cache_global_init();
		return ready;
	}

	// the entry's room, in the caller's vector; nullptr when it cannot grow
	static void* Resize(void* ctx, size_t size)
	{
		try
		{
			auto& v = *static_cast<std::vector<uint8>*>(ctx);
			v.resize(size);
			return v.data();
		}
		catch (...)
		{
			return nullptr;
		}
	}

	file_cache_t* m_fc;
};

// Writes out queued cache entries and ends the writer thread (it restarts on
// the next write). For a libretro core to call before it is unloaded.
inline void FileCache_StopAsyncWriter()
{
	file_cache_stop_writer();
}
