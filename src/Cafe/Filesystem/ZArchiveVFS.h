#pragma once
#include "Common/FileStream.h"
#include <formats/zar.h>

// A Wii U archive (.wua, and Miiverse's OfflineDB.zar) read with
// libretro-common's zar, opened the way the rest of Cemu opens a game image:
// through FileStream, so an archive the host application hands over - an
// Android content:// URI behind SAF, say - reads like any other file.
//
// The archive is mapped where it can be, once, for as long as it is open:
// lookups and listings read the index where it lies, and a file's octets go
// from the mapping straight to where the read wants them - a block kept as
// it is copied once, a compressed one decoded from the pages - with no
// stream, no seek and nothing shared but the pages. Where it cannot be
// mapped (a file only the frontend's VFS reaches, or one too big for the
// address space) the index - everything past the blocks: records, names,
// tree and footer - is read into memory once on opening, and each
// ZArchiveFile reads its blocks through a stream of its own. Either way
// nothing changes after opening, so any number of threads look up, list
// and read at once, each with a zar_t of its own.
class ZArchive
{
public:
	using Node = zar_file_t;

	struct DirEntry
	{
		std::string name;
		Node node;
	};

	// nullptr if it cannot be read as an archive, and then in @whyNot, if
	// given, what was found instead
	static ZArchive* Open(const fs::path& path, std::string* whyNot = nullptr);

	// A file or a directory; the empty path is the root
	std::optional<Node> LookUp(std::string_view path) const;
	// A directory's entries, in name order
	bool ListDir(std::string_view path, std::vector<DirEntry>& entries) const;

	const fs::path& GetPath() const { return m_path; }
	uint64 GetSize() const { return m_size; }

	// whether the octets are lent from the archive's mapping, so that a
	// file reads with no stream
	bool IsMapped() const { return m_mapped != nullptr; }

	// What a reader asked for: from the mapping, from the index in memory,
	// or from @stream
	size_t Supply(const zar_request_t& req, FileStream* stream) const;

private:
	ZArchive() = default;
	zar_t* NewReader() const;

	fs::path m_path;
	uint64 m_size{};
	uint64 m_indexOffset{};
	std::vector<uint8> m_index;            // unmapped only
	std::unique_ptr<FileStream> m_mapping;
	const uint8* m_mapped{};               // m_size octets while m_mapping lives
};

class ZArchiveFile
{
public:
	ZArchiveFile(const ZArchive& archive, const ZArchive::Node& file) : m_archive(archive), m_file(file) {}
	~ZArchiveFile();
	ZArchiveFile(const ZArchiveFile&) = delete;
	ZArchiveFile& operator=(const ZArchiveFile&) = delete;

	uint64 GetSize() const { return m_file.size; }
	// Octets read: fewer than @size only at the file's end or on an error
	uint64 Read(uint64 offset, void* buffer, uint64 size);

private:
	const ZArchive& m_archive;
	ZArchive::Node m_file;
	zar_t* m_reader{};
	std::unique_ptr<FileStream> m_stream;  // unmapped only
};
