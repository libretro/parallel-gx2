#include "Cafe/Filesystem/ZArchiveVFS.h"

// What the last 144 octets of a file say, against what an archive's footer
// has there: its magic, its version, and the archive's size, which has to be
// the file's (as the stream reports it)
static std::string DescribeFooter(VFSFileStream* stream, uint64 fileSize)
{
	if (fileSize < 144)
		return fmt::format("it is {} bytes long, too short for an archive", fileSize);
	uint8 f[144];
	stream->SetPosition(fileSize - 144);
	if (stream->readData(f, sizeof(f)) != sizeof(f))
		return fmt::format("its last 144 of {} bytes could not be read", fileSize);
	auto be32 = [&](int at) { return (uint32)f[at] << 24 | (uint32)f[at + 1] << 16 | (uint32)f[at + 2] << 8 | f[at + 3]; };
	const uint64 total = (uint64)be32(128) << 32 | be32(132);
	return fmt::format("its footer has magic {:08x} (an archive's: 169f52d6), version {:08x} (61bf3a01), size {} (the file's: {})",
		be32(140), be32(136), total, fileSize);
}

ZArchive* ZArchive::Open(const fs::path& path, std::string* whyNot)
{
	auto fail = [&](std::string why) -> ZArchive* {
		if (whyNot)
			*whyNot = std::move(why);
		return nullptr;
	};
	std::unique_ptr<VFSFileStream> stream(VFSFileStream::openFile2(path));
	if (!stream)
		return fail("it could not be opened");
	std::unique_ptr<ZArchive> archive(new ZArchive());
	archive->m_path = path;
	archive->m_size = stream->GetSize();
	// read the footer from the file, then the whole index after the blocks
	zar_t* z = zar_new(archive->m_size);
	if (!z)
		return fail("out of memory");
	zar_request_t req;
	int err;
	while ((err = zar_open_step(z, &req)) == ZAR_PENDING)
		zar_feed(z, archive->Supply(req, stream.get()));
	const uint64 indexOffset = zar_index_offset(z);
	zar_free(z);
	if (err != ZAR_OK || indexOffset >= archive->m_size)
		return fail(DescribeFooter(stream.get(), archive->m_size));
	archive->m_index.resize(archive->m_size - indexOffset);
	stream->SetPosition(indexOffset);
	if (stream->readData(archive->m_index.data(), (uint32)archive->m_index.size()) != archive->m_index.size())
		return fail(fmt::format("its index, {} bytes from offset {}, could not be read", archive->m_index.size(), indexOffset));
	archive->m_indexOffset = indexOffset;
	return archive.release();
}

size_t ZArchive::Supply(const zar_request_t& req, VFSFileStream* stream) const
{
	size_t length = req.length;
	if (!m_index.empty() && req.offset >= m_indexOffset)
	{
		const uint64 at = req.offset - m_indexOffset;
		if (at >= m_index.size())
			return 0;
		length = (size_t)std::min<uint64>(length, m_index.size() - at);
		memcpy(req.dst, m_index.data() + at, length);
		return length;
	}
	if (!stream)
		return 0;
	// a short supply is asked for again: the index part comes from memory
	if (!m_index.empty() && length > m_indexOffset - req.offset)
		length = (size_t)(m_indexOffset - req.offset);
	length = std::min<size_t>(length, 0x40000000);
	stream->SetPosition(req.offset);
	return stream->readData(req.dst, (uint32)length);
}

zar_t* ZArchive::NewReader() const
{
	zar_t* z = zar_new(m_size);
	if (!z)
		return nullptr;
	zar_request_t req;
	int err;
	while ((err = zar_open_step(z, &req)) == ZAR_PENDING)
		zar_feed(z, Supply(req, nullptr));
	if (err != ZAR_OK)
	{
		zar_free(z);
		return nullptr;
	}
	return z;
}

std::optional<ZArchive::Node> ZArchive::LookUp(std::string_view path) const
{
	zar_t* z = NewReader();
	if (!z)
		return std::nullopt;
	const std::string p(path); // zar wants it NUL-terminated
	Node node{};
	zar_request_t req;
	int err = zar_lookup_begin(z, p.c_str());
	if (err == ZAR_OK)
	{
		while ((err = zar_lookup_step(z, &req, &node)) == ZAR_PENDING)
			zar_feed(z, Supply(req, nullptr));
	}
	zar_free(z);
	if (err != ZAR_OK)
		return std::nullopt;
	return node;
}

bool ZArchive::ListDir(std::string_view path, std::vector<DirEntry>& entries) const
{
	entries.clear();
	zar_t* z = NewReader();
	if (!z)
		return false;
	const std::string p(path);
	zar_dirent_t e;
	zar_request_t req;
	int err = zar_dir_begin(z, p.c_str());
	while (err == ZAR_OK)
	{
		while ((err = zar_dir_step(z, &req, &e)) == ZAR_PENDING)
			zar_feed(z, Supply(req, nullptr));
		if (err == ZAR_OK)
			entries.push_back({std::string(e.name, e.len), e.file});
	}
	zar_free(z);
	return err == ZAR_END;
}

ZArchiveFile::~ZArchiveFile()
{
	zar_free(m_reader);
}

uint64 ZArchiveFile::Read(uint64 offset, void* buffer, uint64 size)
{
	if (m_file.dir || offset >= m_file.size)
		return 0;
	if (!m_reader)
	{
		m_reader = zar_new(m_archive.GetSize());
		if (!m_reader)
			return 0;
		zar_request_t req;
		int err;
		while ((err = zar_open_step(m_reader, &req)) == ZAR_PENDING)
			zar_feed(m_reader, m_archive.Supply(req, nullptr));
		if (err != ZAR_OK)
		{
			zar_free(m_reader);
			m_reader = nullptr;
			return 0;
		}
	}
	if (!m_stream)
	{
		m_stream.reset(VFSFileStream::openFile2(m_archive.GetPath()));
		if (!m_stream)
			return 0;
	}
	size = std::min<uint64>(size, m_file.size - offset);
	zar_request_t req;
	int err = zar_read_begin(m_reader, &m_file, offset, buffer, (size_t)size);
	if (err == ZAR_OK)
	{
		while ((err = zar_read_step(m_reader, &req)) == ZAR_PENDING)
			zar_feed(m_reader, m_archive.Supply(req, m_stream.get()));
	}
	return zar_read_progress(m_reader);
}
