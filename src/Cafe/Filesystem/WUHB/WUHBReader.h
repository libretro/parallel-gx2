#pragma once
#include <Common/FileStream.h>
#include "RomFSStructs.h"

// A homebrew bundle (.wuhb): a RomFS image. Mapped where it can be, once:
// then an entry is read where it lies, a name compared there, and a file's
// octets copied once from the pages to where the read wants them, with no
// stream, no seek and nothing allocated. Where it cannot be mapped, through a
// stream lent to each reader (shared_file.h). Either way read from any number
// of threads at once.
class WUHBReader
{
  public:
	// map: false to read through streams even where it could be mapped
	static WUHBReader* FromPath(const fs::path& path, bool map = true);
	~WUHBReader();
	WUHBReader(const WUHBReader&) = delete;
	WUHBReader& operator=(const WUHBReader&) = delete;

	romfs_direntry_t GetDirEntry(uint32 offset) const;
	romfs_fentry_t GetFileEntry(uint32 offset) const;

	uint64 GetFileSize(uint32 entryOffset) const;
	// whether the image's octets are lent from its mapping
	bool IsMapped() const { return !m_mapped.empty(); }

	uint64 ReadFromFile(uint32 entryOffset, uint64 fileOffset, uint64 length, void* buffer) const;

	uint32 Lookup(const std::filesystem::path& path, bool isFile) const;

  private:
	explicit WUHBReader(struct shared_file* file);
	WUHBReader() = delete;

	romfs_header_t m_header;
	struct shared_file* m_file; // shared_file.h
	std::span<const uint8> m_mapped; // its octets, where mapped
	// len octets at offset into dst: from the mapping, or the stream
	bool ReadAt(uint64 offset, void* dst, uint64 len) const;
	// an entry's fields before its name, and where its name lies
	template<bool File>
	bool ReadFixed(uint32 offset, void* fixed, uint64& nameAt) const;
	// whether an entry's name is name, compared where it lies when mapped
	bool NameIs(uint64 nameAt, uint32 nameSize, std::string_view name) const;
	constexpr static std::string_view s_headerMagicValue = "WUHB";
	bool ReadHeader();
	bool CheckMagicValue() const;

	static inline unsigned char NormalizeChar(unsigned char c);
	static uint32 CalcPathHash(uint32 parent, const char* path, uint32 start, size_t path_len);

	template<bool File>
	using EntryType = std::conditional_t<File, romfs_fentry_t, romfs_direntry_t>;
	template<bool File>
	static const EntryType<File>& GetFallback();
	template<bool File>
	EntryType<File> GetEntry(uint32 offset) const;

	template<bool T>
	bool SearchHashList(uint32& entryOffset, std::string_view targetName) const;
	uint32 GetHashTableEntryOffset(uint32 hash, bool isFile) const;
};
