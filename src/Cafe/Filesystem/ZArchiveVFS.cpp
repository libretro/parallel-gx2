#include "Cafe/Filesystem/ZArchiveVFS.h"

// What the last 144 octets of a file say, against what an archive's footer
// has there: its magic, its version, and the archive's size, which has to be
// the file's (as the stream reports it)
static std::string DescribeFooter(FileStream* stream, uint64 fileSize)
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
	std::unique_ptr<FileStream> stream(FileStream::openFileMapped(path));
	if (!stream)
		return fail("it could not be opened");
	std::unique_ptr<ZArchive> archive(new ZArchive());
	archive->m_path = path;
	archive->m_size = stream->GetSize();
	const std::span<const uint8> mapped = stream->MappedSpan();
	if (!mapped.empty() && mapped.size() == archive->m_size)
		archive->m_mapped = mapped.data();
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
	archive->m_indexOffset = indexOffset;
	if (archive->m_mapped)
	{
		// the index is read where it lies, and so is everything else
		archive->m_mapping = std::move(stream);
		return archive.release();
	}
	archive->m_index.resize(archive->m_size - indexOffset);
	stream->SetPosition(indexOffset);
	if (stream->readData(archive->m_index.data(), (uint32)archive->m_index.size()) != archive->m_index.size())
		return fail(fmt::format("its index, {} bytes from offset {}, could not be read", archive->m_index.size(), indexOffset));
	return archive.release();
}

size_t ZArchive::Supply(const zar_request_t& req, FileStream* stream) const
{
	size_t length = req.length;
	if (m_mapped)
	{
		// one copy, from the pages to where the reader wants them
		if (req.offset >= m_size)
			return 0;
		length = (size_t)std::min<uint64>(length, m_size - req.offset);
		memcpy(req.dst, m_mapped + req.offset, length);
		return length;
	}
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
	m_readersMade.fetch_add(1, std::memory_order_relaxed);
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

ZArchive::~ZArchive()
{
	for (std::atomic<zar_t*>& slot : m_readers)
		zar_free(slot.exchange(nullptr, std::memory_order_acquire));
}

zar_t* ZArchive::AcquireReader() const
{
	for (std::atomic<zar_t*>& slot : m_readers)
	{
		if (slot.load(std::memory_order_relaxed) == nullptr)
			continue;
		if (zar_t* z = slot.exchange(nullptr, std::memory_order_acquire))
			return z;
	}
	return NewReader();
}

void ZArchive::ReleaseReader(zar_t* z) const
{
	if (!z)
		return;
	for (std::atomic<zar_t*>& slot : m_readers)
	{
		zar_t* empty = nullptr;
		if (slot.load(std::memory_order_relaxed) == nullptr
			&& slot.compare_exchange_strong(empty, z, std::memory_order_release, std::memory_order_relaxed))
			return;
	}
	zar_free(z);
}

// A path zar can take, NUL-terminated: on the stack when it is short
class ZArchivePath
{
public:
	explicit ZArchivePath(std::string_view path)
	{
		if (path.size() < sizeof(m_short))
		{
			memcpy(m_short, path.data(), path.size());
			m_short[path.size()] = '\0';
			m_str = m_short;
		}
		else
		{
			m_long.assign(path);
			m_str = m_long.c_str();
		}
	}
	const char* c_str() const { return m_str; }

private:
	char m_short[512];
	std::string m_long;
	const char* m_str;
};

std::optional<ZArchive::Node> ZArchive::LookUp(std::string_view path) const
{
	zar_t* z = AcquireReader();
	if (!z)
		return std::nullopt;
	const ZArchivePath p(path); // zar wants it NUL-terminated
	Node node{};
	zar_request_t req;
	int err = zar_lookup_begin(z, p.c_str());
	if (err == ZAR_OK)
	{
		while ((err = zar_lookup_step(z, &req, &node)) == ZAR_PENDING)
			zar_feed(z, Supply(req, nullptr));
	}
	ReleaseReader(z);
	if (err != ZAR_OK)
		return std::nullopt;
	return node;
}

bool ZArchive::ListDir(std::string_view path, std::vector<DirEntry>& entries) const
{
	entries.clear();
	zar_t* z = AcquireReader();
	if (!z)
		return false;
	const ZArchivePath p(path);
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
	ReleaseReader(z);
	return err == ZAR_END;
}

ZArchiveFile::~ZArchiveFile() = default;

uint64 ZArchiveFile::Read(uint64 offset, void* buffer, uint64 size)
{
	if (m_file.dir || offset >= m_file.size)
		return 0;
	if (!m_stream && !m_archive.IsMapped())
	{
		m_stream.reset(FileStream::openFile2(m_archive.GetPath()));
		if (!m_stream)
			return 0;
	}
	zar_t* reader = m_archive.AcquireReader();
	if (!reader)
		return 0;
	size = std::min<uint64>(size, m_file.size - offset);
	zar_request_t req;
	int err = zar_read_begin(reader, &m_file, offset, buffer, (size_t)size);
	if (err == ZAR_OK)
	{
		while ((err = zar_read_step(reader, &req)) == ZAR_PENDING)
			zar_feed(reader, m_archive.Supply(req, m_stream.get()));
	}
	const uint64 read = zar_read_progress(reader);
	m_archive.ReleaseReader(reader);
	return read;
}
