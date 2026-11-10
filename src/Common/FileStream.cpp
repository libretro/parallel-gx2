#include "Common/FileStream.h"
#include <cstdarg>

#include <streams/file_stream.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <libretro.h>

#ifndef _WIN32
// The path as the file system has it, matched without regard to case where it
// does not exist as given: titles name their files in whatever case, on a
// console whose file system does not care. Through the VFS, like the open.
static fs::path findPathCI(const fs::path& path)
{
	if (path_is_valid(_pathToUtf8(path).c_str()))
		return path;
	const fs::path name = path.filename();
	fs::path parent = path.parent_path();
	if (parent.empty())
		parent = ".";
	else if (!path_is_valid(_pathToUtf8(parent).c_str()))
		parent = findPathCI(parent);
	const std::string wanted = _pathToUtf8(name);
	if (struct RDIR* dir = retro_opendir_include_hidden(_pathToUtf8(parent).c_str(), true))
	{
		while (retro_readdir(dir))
		{
			const char* entry = retro_dirent_get_name(dir);
			if (entry && boost::iequals(std::string_view(entry), wanted))
			{
				fs::path found = parent / _utf8ToPath(entry);
				retro_closedir(dir);
				return found;
			}
		}
		retro_closedir(dir);
	}
	return parent / name;
}
#endif

std::string FileStream::HostPath(const fs::path& path)
{
#ifdef _WIN32
	return _pathToUtf8(path);
#else
	return _pathToUtf8(findPathCI(path));
#endif
}

FileStream* FileStream::Open(const fs::path& path, unsigned mode, unsigned hints)
{
	const std::string p = HostPath(path);
	// a directory is not a file: the host file system device asks this to
	// tell them apart, and some opens succeed on one
	if (path_is_directory(p.c_str()))
		return nullptr;
	RFILE* file = filestream_open(p.c_str(), mode, hints);
	return file ? new FileStream(file) : nullptr;
}

FileStream* FileStream::openFile(std::string_view path)
{
	return openFile2(_utf8ToPath(path), false);
}

FileStream* FileStream::openFile(const wchar_t* path, bool allowWrite)
{
	return openFile2(fs::path(path), allowWrite);
}

FileStream* FileStream::openFile2(const fs::path& path, bool allowWrite)
{
	// UPDATE_EXISTING: opened, not created anew
	return Open(path, allowWrite ? (RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) : RETRO_VFS_FILE_ACCESS_READ,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
}

FileStream* FileStream::openFileMapped(const fs::path& path)
{
	return Open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
}

FileStream* FileStream::createFile(const wchar_t* path)
{
	return createFile2(fs::path(path));
}

FileStream* FileStream::createFile(std::string_view path)
{
	return createFile2(_utf8ToPath(path));
}

FileStream* FileStream::createFile2(const fs::path& path)
{
	return Open(path, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
}

std::optional<std::vector<uint8>> FileStream::LoadIntoMemory(const fs::path& path)
{
	// mapped where it can be, so the one copy is into the caller's memory
	FileStream* fs = openFileMapped(path);
	if (!fs)
		return std::nullopt;
	std::optional<std::vector<uint8>> v;
	const std::span<const uint8> mapped = fs->MappedSpan();
	const uint64 size = fs->GetSize();
	if (!mapped.empty() || size == 0)
		v.emplace(mapped.begin(), mapped.end());
	else if (size <= 0xFFFFFFFFull)
	{
		v.emplace(size);
		if (fs->readData(v->data(), (uint32)size) != (uint32)size)
			v.reset();
	}
	delete fs;
	return v;
}

bool FileStream::WriteFile(const fs::path& path, const void* data, size_t size)
{
	return filestream_write_file(_pathToUtf8(path).c_str(), data, (int64_t)size);
}

bool FileStream::IsRegularFile(const fs::path& path)
{
	const int flags = path_stat(_pathToUtf8(path).c_str());
	return (flags & RETRO_VFS_STAT_IS_VALID) && !(flags & RETRO_VFS_STAT_IS_DIRECTORY);
}

bool FileStream::IsDirectory(const fs::path& path)
{
	return path_is_directory(_pathToUtf8(path).c_str());
}

bool FileStream::Exists(const fs::path& path)
{
	return path_is_valid(_pathToUtf8(path).c_str());
}

std::optional<bool> FileStream::IsReadOnly(const fs::path& path)
{
	const int flags = path_stat(_pathToUtf8(path).c_str());
	if (!(flags & RETRO_VFS_STAT_IS_VALID))
		return std::nullopt;
	return (flags & RETRO_VFS_STAT_IS_READONLY) != 0;
}

bool FileStream::Remove(const fs::path& path)
{
	return filestream_delete(_pathToUtf8(path).c_str()) == 0;
}

bool FileStream::Rename(const fs::path& from, const fs::path& to)
{
	return filestream_rename(_pathToUtf8(from).c_str(), _pathToUtf8(to).c_str()) == 0;
}

bool FileStream::CreateDirectories(const fs::path& path)
{
	return path_mkdir(_pathToUtf8(path).c_str());
}

std::optional<uint64> FileStream::FileSize(const fs::path& path)
{
	const std::string p = _pathToUtf8(path);
	const int flags = path_stat(p.c_str());
	if (!(flags & RETRO_VFS_STAT_IS_VALID) || (flags & RETRO_VFS_STAT_IS_DIRECTORY))
		return std::nullopt;
	const int64_t size = path_get_size(p.c_str());
	return size < 0 ? std::nullopt : std::optional<uint64>((uint64)size);
}

bool FileStream::RemoveAll(const fs::path& path)
{
	const std::string p = _pathToUtf8(path);
	// a file, an empty directory or a link goes at once - a link to a
	// directory without what that holds; only a directory that is not
	// empty has its entries removed first
	if (filestream_delete(p.c_str()) == 0)
		return true;
	if (!(path_stat(p.c_str()) & RETRO_VFS_STAT_IS_VALID))
		return true;
	if (!path_is_directory(p.c_str()))
		return false;
	for (Dir d(path); d.Next();)
		RemoveAll(d.Path());
	return filestream_delete(p.c_str()) == 0;
}

FileStream::Dir::Dir(const fs::path& path) : m_path(path)
{
	m_dir = retro_opendir_include_hidden(_pathToUtf8(path).c_str(), true);
}

FileStream::Dir::~Dir()
{
	if (m_dir)
		retro_closedir(m_dir);
}

bool FileStream::Dir::Next()
{
	if (!m_dir)
		return false;
	while (retro_readdir(m_dir))
	{
		const char* name = retro_dirent_get_name(m_dir);
		if (!name || !strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		m_name = name;
		return true;
	}
	m_name = {};
	return false;
}

fs::path FileStream::Dir::Path() const
{
	return m_path / _utf8ToPath(m_name);
}

bool FileStream::Dir::IsDirectory() const
{
	return m_dir && retro_dirent_is_dir(m_dir, nullptr);
}

uint64 FileStream::Dir::Size() const
{
	int64_t size = 0;
	if (!m_dir || !retro_dirent_stat(m_dir, &size, nullptr) || size < 0)
		return 0;
	return (uint64)size;
}

void FileStream::SetPosition(uint64 pos)
{
	filestream_seek(m_file, (int64_t)pos, RETRO_VFS_SEEK_POSITION_START);
}

uint64 FileStream::GetSize()
{
	const int64_t size = filestream_get_size(m_file);
	return size < 0 ? 0 : (uint64)size;
}

bool FileStream::SetEndOfFile()
{
	const int64_t pos = filestream_tell(m_file);
	return pos >= 0 && filestream_truncate(m_file, pos) == 0;
}

void FileStream::extract(std::vector<uint8>& data)
{
	const std::span<const uint8> mapped = MappedSpan();
	if (!mapped.empty())
	{
		data.assign(mapped.begin(), mapped.end());
		return;
	}
	const uint64 size = GetSize();
	data.resize((size_t)size);
	SetPosition(0);
	data.resize(readData(data.data(), (uint32)size));
}

void FileStream::Flush()
{
	filestream_flush(m_file);
}

std::span<const uint8> FileStream::MappedSpan()
{
	int64_t len = 0;
	const uint8_t* map = filestream_get_mapped_ptr(m_file, &len);
	return map && len > 0 ? std::span<const uint8>(map, (size_t)len) : std::span<const uint8>();
}

std::span<const uint8> FileStream::View(uint32 length)
{
	const uint8_t* ptr = nullptr;
	const int64_t n = filestream_view(m_file, length, &ptr);
	return n > 0 && ptr ? std::span<const uint8>(ptr, (size_t)n) : std::span<const uint8>();
}

uint32 FileStream::readData(void* data, uint32 length)
{
	const int64_t n = filestream_read(m_file, data, length);
	return n < 0 ? 0 : (uint32)n;
}

bool FileStream::readU64(uint64& v)
{
	return readData(&v, sizeof(uint64)) == sizeof(uint64);
}

bool FileStream::readU32(uint32& v)
{
	return readData(&v, sizeof(uint32)) == sizeof(uint32);
}

bool FileStream::readU16(uint16& v)
{
	return readData(&v, sizeof(uint16)) == sizeof(uint16);
}

bool FileStream::readU8(uint8& v)
{
	return readData(&v, sizeof(uint8)) == sizeof(uint8);
}

// A line without its '\r's and '\n'; false at the end of the file. Found by
// filestream_gets, which looks for the end in the mapping or its read-ahead
// rather than a byte at a time
bool FileStream::readLine(std::string& line)
{
	line.clear();
	char buf[512];
	bool any = false;
	while (filestream_gets(m_file, buf, sizeof(buf)))
	{
		any = true;
		size_t n = strlen(buf);
		const bool end = n && buf[n - 1] == '\n';
		if (end)
			n--;
		// the runs between '\r's, each appended whole
		for (const char *p = buf, *e = buf + n; p < e;)
		{
			const char* cr = (const char*)memchr(p, '\r', (size_t)(e - p));
			line.append(p, cr ? cr : e);
			p = cr ? cr + 1 : e;
		}
		if (end)
			break;
	}
	return any;
}

sint32 FileStream::writeData(const void* data, sint32 length)
{
	const int64_t n = filestream_write(m_file, data, length);
	return n < 0 ? 0 : (sint32)n;
}

void FileStream::writeU64(uint64 v)
{
	writeData(&v, sizeof(uint64));
}

void FileStream::writeU32(uint32 v)
{
	writeData(&v, sizeof(uint32));
}

void FileStream::writeU16(uint16 v)
{
	writeData(&v, sizeof(uint16));
}

void FileStream::writeU8(uint8 v)
{
	writeData(&v, sizeof(uint8));
}

void FileStream::writeStringFmt(const char* format, ...)
{
	char buffer[2048];
	va_list args;
	va_start(args, format);
	const int n = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (n > 0)
		writeData(buffer, std::min<sint32>(n, (sint32)sizeof(buffer) - 1));
}

void FileStream::writeString(const char* str)
{
	writeData(str, (sint32)strlen(str));
}

void FileStream::writeLine(const char* str)
{
	writeData(str, (sint32)strlen(str));
	writeData("\r\n", 2);
}

FileStream::~FileStream()
{
	filestream_close(m_file);
}
