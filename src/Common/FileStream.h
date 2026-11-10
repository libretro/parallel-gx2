#pragma once
#include "Common/precompiled.h"

// A file, through libretro-common's filestream, on every platform: through
// libretro-common's own implementation for a path it can open, mapped where
// asked and where the OS can; through the frontend's VFS for a path only the
// frontend can reach (vfs_hybrid, set up in retro_set_environment).
class FileStream
{
public:
	static FileStream* openFile(std::string_view path);
	static FileStream* openFile(const wchar_t* path, bool allowWrite = false);
	static FileStream* openFile2(const fs::path& path, bool allowWrite = false);
	// Read-only, mapped where it can be: MappedSpan and View lend from the
	// mapping instead of reading
	static FileStream* openFileMapped(const fs::path& path);

	static FileStream* createFile(const wchar_t* path);
	static FileStream* createFile(std::string_view path);
	static FileStream* createFile2(const fs::path& path);

	// The file, read into memory that is the caller's
	static std::optional<std::vector<uint8>> LoadIntoMemory(const fs::path& path);
	// data as the whole of the file, created or emptied first; in one write
	static bool WriteFile(const fs::path& path, const void* data, size_t size);

	// Through the same VFS as the files
	static bool IsRegularFile(const fs::path& path);
	static bool IsDirectory(const fs::path& path);
	static bool Exists(const fs::path& path);
	// nothing when it cannot be told
	static std::optional<bool> IsReadOnly(const fs::path& path);
	static bool Remove(const fs::path& path);
	static bool Rename(const fs::path& from, const fs::path& to);
	// the directory and those above it that are missing; true if it is there
	static bool CreateDirectories(const fs::path& path);
	// a file's size; nothing for what is not there
	static std::optional<uint64> FileSize(const fs::path& path);
	// a file, or a directory with all it holds; true if nothing is left
	static bool RemoveAll(const fs::path& path);

	// A directory's entries, '.' and '..' left out, in no order, through the
	// same VFS as the files (retro_dirent): for (Dir d(p); d.Next();) ...
	class Dir
	{
	public:
		explicit Dir(const fs::path& path);
		~Dir();
		Dir(const Dir&) = delete;
		Dir& operator=(const Dir&) = delete;

		bool Opened() const { return m_dir != nullptr; }
		// on to the next entry; false past the last
		bool Next();
		// the entry's name, valid until the next Next
		std::string_view Name() const { return m_name; }
		fs::path Path() const;
		bool IsDirectory() const;
		// a file's size, from what was read with the entry where it can be
		uint64 Size() const;

	private:
		struct RDIR* m_dir{};
		fs::path m_path;
		std::string_view m_name;
	};

	// Every file and directory under path, depth first, a directory before
	// what it holds: f(const fs::path&, bool isDirectory)
	template<typename F>
	static void ForEachInTree(const fs::path& path, F&& f)
	{
		for (Dir d(path); d.Next();)
		{
			const fs::path p = d.Path();
			const bool isDir = d.IsDirectory();
			f(p, isDir);
			if (isDir)
				ForEachInTree(p, f);
		}
	}

	void SetPosition(uint64 pos);
	uint64 GetSize();
	bool SetEndOfFile();
	void extract(std::vector<uint8>& data);
	void Flush();

	// Lent, not copied. The whole file, where it is mapped (openFileMapped),
	// valid until the stream is deleted; empty otherwise
	std::span<const uint8> MappedSpan();
	// Up to length bytes at the position, moved past as a read would: from
	// the mapping or the read-ahead, valid until the next call on the stream;
	// empty when nothing there can be lent, and the caller reads instead
	std::span<const uint8> View(uint32 length);

	// reading (binary)
	uint32 readData(void* data, uint32 length);
	bool readU64(uint64& v);
	bool readU32(uint32& v);
	bool readU16(uint16& v);
	bool readU8(uint8& v);
	bool readLine(std::string& line);

	// writing (binary)
	sint32 writeData(const void* data, sint32 length);
	void writeU64(uint64 v);
	void writeU32(uint32 v);
	void writeU16(uint16 v);
	void writeU8(uint8 v);

	// writing (strings)
	void writeStringFmt(const char* format, ...);
	void writeString(const char* str);
	void writeLine(const char* str);

	~FileStream();
	FileStream(const FileStream&) = delete;
	FileStream& operator=(const FileStream&) = delete;

private:
	explicit FileStream(struct RFILE* file) : m_file(file) {}
	static FileStream* Open(const fs::path& path, unsigned mode, unsigned hints);

	struct RFILE* m_file;
};

// Streams of one file read at many places from many threads at once, where it
// cannot be mapped: each reader is lent a stream of its own for a read and
// gives it back, with no lock; one is opened when none is free.
class FileStreamPool
{
public:
	// first, if given, is the pool's to keep
	explicit FileStreamPool(const fs::path& path, FileStream* first = nullptr);
	~FileStreamPool();
	FileStreamPool(const FileStreamPool&) = delete;
	FileStreamPool& operator=(const FileStreamPool&) = delete;

	// nullptr when the file can no longer be opened
	FileStream* Acquire();
	void Release(FileStream* stream);
	// streams opened, the first included
	uint32 Opened() const { return m_opened.load(std::memory_order_relaxed); }

private:
	static constexpr size_t SLOTS = 4;
	fs::path m_path;
	std::atomic<FileStream*> m_slots[SLOTS]{};
	std::atomic<uint32> m_opened{0};
};
