#pragma once
#include "Common/FileStream.h"
#include "util/helpers/mapped_file.h"

#include <streams/file_stream.h>

// A file's bytes, read-only, for as long as the object lives (mapped_file.h):
// lent from the file's mapping, read into memory only where it cannot be
// mapped. Empty for a missing or empty file.
class MappedFile
{
public:
	explicit MappedFile(const fs::path& path) { mapped_file_open(&m_file, FileStream::HostPath(path).c_str()); }
	~MappedFile() { mapped_file_close(&m_file); }
	MappedFile(const MappedFile&) = delete;
	MappedFile& operator=(const MappedFile&) = delete;

	const uint8* data() const { return m_file.data; }
	size_t size() const { return m_file.size; }
	std::span<const uint8> span() const { return { m_file.data, m_file.size }; }
	// whether the file was there to open, empty or not
	bool opened() const { return m_file.opened; }
	// whether the bytes are the file's pages rather than a copy
	bool mapped() const { return m_file.mapped; }

private:
	mapped_file_t m_file{};
};

// Puts data in place of path's contents through a file beside it that then
// takes its name, so that neither a reader nor a crash ever finds it half
// written. False, and path as it was, if that fails.
inline bool ReplaceFileContents(const fs::path& path, const void* data, size_t size)
{
	return filestream_write_file_atomic(_pathToUtf8(path).c_str(), data, (int64_t)size);
}
