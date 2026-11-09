#pragma once

// A file's bytes, read-only, for as long as the object lives: lent from the
// file's mapping where libretro-common can map it (FileStream::openFileMapped),
// read into memory only where it cannot (a file only the frontend's VFS
// reaches). Empty for a missing or empty file. For a blob handed to an API
// that copies it anyway (the Vulkan pipeline cache), and for readers that
// must not share a stream's position (FileCache): no read buffer, no copy.
class MappedFile
{
public:
	explicit MappedFile(const fs::path& path);
	~MappedFile();
	MappedFile(const MappedFile&) = delete;
	MappedFile& operator=(const MappedFile&) = delete;

	const uint8* data() const { return m_data; }
	size_t size() const { return m_size; }
	// whether the bytes are the file's pages rather than a copy
	bool mapped() const { return m_mapped; }

private:
	class FileStream* m_stream{};
	const uint8* m_data{};
	size_t m_size{};
	bool m_mapped{};
	std::vector<uint8> m_copy;
};

// Puts data in place of path's contents through a file beside it that then
// takes its name, so that neither a reader nor a crash ever finds it half
// written. False, and path as it was, if that fails.
bool ReplaceFileContents(const fs::path& path, const void* data, size_t size);
