#include "WUHBReader.h"
#include "Common/FileStream.h"
WUHBReader::WUHBReader(FileStream* fileIn, const fs::path& path) : m_fileIn(fileIn)
{
	cemu_assert_debug(fileIn != nullptr);
	m_mapped = m_fileIn->MappedSpan();
	if (m_mapped.size() != m_fileIn->GetSize())
	{
		m_mapped = {};
		m_streams = std::make_unique<FileStreamPool>(path, m_fileIn.release());
	}
}

bool WUHBReader::ReadAt(uint64 offset, void* dst, uint64 len) const
{
	if (!m_mapped.empty())
	{
		if (offset > m_mapped.size() || m_mapped.size() - offset < len)
			return false;
		memcpy(dst, m_mapped.data() + offset, (size_t)len);
		return true;
	}
	if (len > 0xFFFFFFFF)
		return false;
	FileStream* fs = m_streams->Acquire();
	const bool ok = fs && (fs->SetPosition(offset), fs->readData(dst, (uint32)len) == len);
	m_streams->Release(fs);
	return ok;
}

WUHBReader* WUHBReader::FromPath(const fs::path& path)
{
	FileStream* fileIn{FileStream::openFileMapped(path)};
	if (!fileIn)
		return nullptr;

	WUHBReader* ret = new WUHBReader(fileIn, path);
	if (!ret->CheckMagicValue())
	{
		delete ret;
		return nullptr;
	}

	if (!ret->ReadHeader())
	{
		delete ret;
		return nullptr;
	}

	return ret;
}

static const romfs_direntry_t fallbackDirEntry{
	.parent = ROMFS_ENTRY_EMPTY,
	.listNext = ROMFS_ENTRY_EMPTY,
	.dirListHead = ROMFS_ENTRY_EMPTY,
	.fileListHead = ROMFS_ENTRY_EMPTY,
	.hash = ROMFS_ENTRY_EMPTY,
	.name_size = 0,
	.name = ""
};
static const romfs_fentry_t fallbackFileEntry{
	.parent = ROMFS_ENTRY_EMPTY,
	.listNext = ROMFS_ENTRY_EMPTY,
	.offset = 0,
	.size = 0,
	.hash = ROMFS_ENTRY_EMPTY,
	.name_size = 0,
	.name = ""
};
template<bool File>
const WUHBReader::EntryType<File>& WUHBReader::GetFallback()
{
	if constexpr (File)
		return fallbackFileEntry;
	else
		return fallbackDirEntry;
}

template<bool File>
bool WUHBReader::ReadFixed(uint32 offset, void* fixed, uint64& nameAt) const
{
	const char* typeName = File ? "fentry" : "direntry";
	if (offset == ROMFS_ENTRY_EMPTY)
		return false;
	if (offset >= (File ? m_header.file_table_size : m_header.dir_table_size))
	{
		cemuLog_log(LogType::Force, "WUHB {} offset exceeds table size declared in header", typeName);
		return false;
	}
	const uint64 at = (File ? m_header.file_table_ofs : m_header.dir_table_ofs) + offset;
	if (!ReadAt(at, fixed, offsetof(EntryType<File>, name)))
	{
		cemuLog_log(LogType::Force, "failed to read WUHB {} at offset: {}", typeName, offset);
		return false;
	}
	nameAt = at + offsetof(EntryType<File>, name);
	return true;
}

bool WUHBReader::NameIs(uint64 nameAt, uint32 nameSize, std::string_view name) const
{
	if (nameSize != name.size())
		return false;
	if (!m_mapped.empty())
		return nameAt <= m_mapped.size() && m_mapped.size() - nameAt >= nameSize
			&& memcmp(m_mapped.data() + nameAt, name.data(), nameSize) == 0;
	char buf[256];
	for (uint32 done = 0; done < nameSize;)
	{
		const uint32 n = std::min<uint32>(sizeof(buf), nameSize - done);
		if (!ReadAt(nameAt + done, buf, n) || memcmp(buf, name.data() + done, n) != 0)
			return false;
		done += n;
	}
	return true;
}

template<bool File>
WUHBReader::EntryType<File> WUHBReader::GetEntry(uint32 offset) const
{
	auto fallback = GetFallback<File>();
	EntryType<File> ret;
	uint64 nameAt;
	if (!ReadFixed<File>(offset, &ret, nameAt))
		return fallback;
	// the name
	ret.name.resize(ret.name_size);
	if (!ReadAt(nameAt, ret.name.data(), ret.name_size))
	{
		cemuLog_log(LogType::Force, "failed to read WUHB {} name", File ? "fentry" : "direntry");
		return fallback;
	}
	return ret;
}

romfs_direntry_t WUHBReader::GetDirEntry(uint32 offset) const
{
	return GetEntry<false>(offset);
}
romfs_fentry_t WUHBReader::GetFileEntry(uint32 offset) const
{
	return GetEntry<true>(offset);
}

uint64 WUHBReader::GetFileSize(uint32 entryOffset) const
{
	// the fields alone: no name read, nothing allocated
	romfs_fentry_t fixed;
	uint64 nameAt;
	return ReadFixed<true>(entryOffset, &fixed, nameAt) ? (uint64)fixed.size : 0;
}

uint64 WUHBReader::ReadFromFile(uint32 entryOffset, uint64 fileOffset, uint64 length, void* buffer) const
{
	romfs_fentry_t fileEntry;
	uint64 nameAt;
	if (!ReadFixed<true>(entryOffset, &fileEntry, nameAt))
		return 0;
	const uint64 size = fileEntry.size;
	if (fileOffset >= size)
		return 0;
	uint64 readAmount = std::min(length, size - fileOffset);
	const uint64 wuhbOffset = m_header.file_partition_ofs + fileEntry.offset + fileOffset;
	if (!m_mapped.empty())
	{
		// one copy, from the pages; short where the image is
		if (wuhbOffset >= m_mapped.size())
			return 0;
		readAmount = std::min<uint64>(readAmount, m_mapped.size() - wuhbOffset);
		memcpy(buffer, m_mapped.data() + wuhbOffset, (size_t)readAmount);
		return readAmount;
	}
	FileStream* fs = m_streams->Acquire();
	uint64 got = 0;
	if (fs)
	{
		fs->SetPosition(wuhbOffset);
		got = fs->readData(buffer, (uint32)std::min<uint64>(readAmount, 0xFFFFFFFF));
	}
	m_streams->Release(fs);
	return got;
}

uint32 WUHBReader::GetHashTableEntryOffset(uint32 hash, bool isFile) const
{
	const uint64 hash_table_size = (isFile ? m_header.file_hash_table_size : m_header.dir_hash_table_size);
	const uint64 hash_table_ofs = (isFile ? m_header.file_hash_table_ofs : m_header.dir_hash_table_ofs);

	const uint64 hash_table_entry_count = hash_table_size / sizeof(uint32);
	const uint64 hash_table_entry_offset = hash_table_ofs + (hash % hash_table_entry_count) * sizeof(uint32);

	uint32 tableOffset;
	if (!ReadAt(hash_table_entry_offset, &tableOffset, sizeof(tableOffset)))
	{
		cemuLog_log(LogType::Force, "failed to read WUHB hash table entry at file offset: {}", hash_table_entry_offset);
		return ROMFS_ENTRY_EMPTY;
	}

	return uint32be::from_bevalue(tableOffset);
}

template<bool T>
bool WUHBReader::SearchHashList(uint32& entryOffset, std::string_view targetName) const
{
	// a chain broken by a bad offset, or one that loops, ends the search
	for (uint32 steps = 0; steps < 0x100000; steps++)
	{
		if (entryOffset == ROMFS_ENTRY_EMPTY)
			return false;
		EntryType<T> entry;
		uint64 nameAt;
		if (!ReadFixed<T>(entryOffset, &entry, nameAt))
			return false;
		if (NameIs(nameAt, entry.name_size, targetName))
			return true;
		entryOffset = entry.hash;
	}
	return false;
}

uint32 WUHBReader::Lookup(const std::filesystem::path& path, bool isFile) const
{
	uint32 currentEntryOffset = 0;
	auto look = [&](const fs::path& part, bool lookInFileHT) {
		const auto partString = part.string();
		currentEntryOffset = GetHashTableEntryOffset(CalcPathHash(currentEntryOffset, partString.c_str(), 0, partString.size()), lookInFileHT);
		if (lookInFileHT)
			return SearchHashList<true>(currentEntryOffset, partString);
		else
			return SearchHashList<false>(currentEntryOffset, partString);
	};
	// look for the root entry
	if (!look("", false))
		return ROMFS_ENTRY_EMPTY;

	auto it = path.begin();
	while (it != path.end())
	{
		fs::path part = *it;
		++it;
		// no need to recurse after trailing forward slash (e.g. directory/)
		if (part.empty() && !isFile)
			break;
		// skip leading forward slash
		if (part == "/")
			continue;

		// if the lookup target is a file and this is the last iteration, look in the file hash table instead.
		if (!look(part, it == path.end() && isFile))
			return ROMFS_ENTRY_EMPTY;
	}
	return currentEntryOffset;
}
bool WUHBReader::CheckMagicValue() const
{
	uint8 magic[4];
	if (!ReadAt(0, magic, 4))
	{
		cemuLog_log(LogType::Force, "Failed to read WUHB magic numbers");
		return false;
	}
	static_assert(sizeof(magic) == s_headerMagicValue.size());
	return std::memcmp(&magic, s_headerMagicValue.data(), sizeof(magic)) == 0;
}
bool WUHBReader::ReadHeader()
{
	if (!ReadAt(0, &m_header, sizeof(m_header)))
	{
		cemuLog_log(LogType::Force, "Failed to read WUHB header");
		return false;
	}

	if (m_header.dir_hash_table_size < sizeof(uint32) || m_header.file_hash_table_size < sizeof(uint32))
	{
		cemuLog_log(LogType::Force, "Invalid WUHB hash table size");
		return false;
	}

	return true;
}
unsigned char WUHBReader::NormalizeChar(unsigned char c)
{
	if (c >= 'a' && c <= 'z')
	{
		return c + 'A' - 'a';
	}
	else
	{
		return c;
	}
}
uint32 WUHBReader::CalcPathHash(uint32 parent, const char* path, uint32 start, size_t path_len)
{
	cemu_assert(path != nullptr || path_len == 0);
	uint32 hash = parent ^ 123456789;
	for (uint32 i = 0; i < path_len; i++)
	{
		hash = (hash >> 5) | (hash << 27);
		hash ^= NormalizeChar(path[start + i]);
	}

	return hash;
}
