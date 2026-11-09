#pragma once
#include "Common/FileStream.h"
#include <formats/zar.h>

// A Wii U archive (.wua, and Miiverse's OfflineDB.zar) read with
// libretro-common's zar, opened the way the rest of Cemu opens a game image:
// through FileStream, so an archive the host application hands over - an
// Android content:// URI behind SAF, say - reads like any other file.
//
// The archive's index - everything past the compressed blocks: records,
// names, tree and footer - is read into memory once on opening and never
// changes after, so any number of threads look paths up and list
// directories at once, each with a zar_t of its own and no I/O. A file's
// octets are read through a ZArchiveFile, which has its own reader and its
// own stream of the archive: nothing is shared between threads that read.
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

	// What a reader asked for: from the index in memory, or from @stream
	size_t Supply(const zar_request_t& req, FileStream* stream) const;

private:
	ZArchive() = default;
	zar_t* NewReader() const;

	fs::path m_path;
	uint64 m_size{};
	uint64 m_indexOffset{};
	std::vector<uint8> m_index;
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
	std::unique_ptr<FileStream> m_stream;
};
