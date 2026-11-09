#include "util/helpers/MappedFile.h"
#include "Common/FileStream.h"

#include <streams/file_stream.h>

MappedFile::MappedFile(const fs::path& path)
{
	m_stream = FileStream::openFileMapped(path);
	if (!m_stream)
		return;
	const std::span<const uint8> mapped = m_stream->MappedSpan();
	if (!mapped.empty())
	{
		// MAP_SHARED: what is written into the mapped range shows, and a
		// rename over the path leaves these pages as they were
		m_data = mapped.data();
		m_size = mapped.size();
		m_mapped = true;
		return;
	}
	// what cannot be mapped is read, once
	m_stream->extract(m_copy);
	delete m_stream;
	m_stream = nullptr;
	if (m_copy.empty())
		return;
	m_data = m_copy.data();
	m_size = m_copy.size();
}

MappedFile::~MappedFile()
{
	delete m_stream;
}

bool ReplaceFileContents(const fs::path& path, const void* data, size_t size)
{
	// a file beside it, renamed into place, removed on failure
	return filestream_write_file_atomic(_pathToUtf8(path).c_str(), data, (int64_t)size);
}
