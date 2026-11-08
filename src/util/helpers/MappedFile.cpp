#include "util/helpers/MappedFile.h"

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

MappedFile::MappedFile(const fs::path& path)
{
#ifdef _WIN32
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && (uint64)size.QuadPart <= (uint64)SIZE_MAX)
	{
		// the view keeps the mapping (and the file) open after the handles go
		HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (mapping)
		{
			m_view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
			CloseHandle(mapping);
		}
		if (m_view)
		{
			m_data = (const uint8*)m_view;
			m_size = (size_t)size.QuadPart;
		}
	}
	CloseHandle(file);
	if (m_view || size.QuadPart <= 0)
		return;
#else
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	struct stat st{};
	if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64)st.st_size <= (uint64)SIZE_MAX)
	{
		// the mapping keeps the file's pages after the descriptor goes, and
		// keeps them as they were if the file is then replaced by a rename
		void* view = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (view != MAP_FAILED)
		{
			m_view = view;
			m_data = (const uint8*)view;
			m_size = (size_t)st.st_size;
		}
	}
	close(fd);
	if (m_view || st.st_size <= 0)
		return;
#endif
	// what cannot be mapped is read
	std::ifstream in(path, std::ios::in | std::ios::binary | std::ios::ate);
	if (!in.is_open())
		return;
	const std::streamoff n = in.tellg();
	if (n <= 0)
		return;
	in.seekg(0, std::ios::beg);
	m_copy.resize((size_t)n);
	if (!in.read((char*)m_copy.data(), n))
	{
		m_copy.clear();
		return;
	}
	m_data = m_copy.data();
	m_size = m_copy.size();
}

MappedFile::~MappedFile()
{
	if (!m_view)
		return;
#ifdef _WIN32
	UnmapViewOfFile(m_view);
#else
	munmap(m_view, m_size);
#endif
}

bool ReplaceFileContents(const fs::path& path, const void* data, size_t size)
{
	fs::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			return false;
		out.write((const char*)data, (std::streamsize)size);
		out.close();
		if (!out)
		{
			std::error_code ec;
			fs::remove(tmp, ec);
			return false;
		}
	}
#ifdef _WIN32
	const bool ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	const bool ok = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
	if (!ok)
	{
		std::error_code ec;
		fs::remove(tmp, ec);
	}
	return ok;
}
