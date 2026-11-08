#pragma once

// A file's bytes, read-only, for as long as the object lives: mapped where
// the OS can map it, read into memory where it cannot. Empty for a missing
// or empty file. For a blob handed to an API that copies it anyway (the
// Vulkan pipeline cache): no read buffer, and no copy of it.
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
	bool mapped() const { return m_view != nullptr; }

private:
	const uint8* m_data{};
	size_t m_size{};
	void* m_view{};
	std::vector<uint8> m_copy;
};

// Puts data in place of path's contents through a file beside it that then
// takes its name, so that neither a reader nor a crash ever finds it half
// written. False, and path as it was, if that fails.
bool ReplaceFileContents(const fs::path& path, const void* data, size_t size);
