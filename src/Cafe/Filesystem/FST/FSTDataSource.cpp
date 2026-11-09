#include "Cafe/Filesystem/FST/FSTDataSource.h"
#include "Cafe/Filesystem/WUD/wud.h"
#include "Common/FileStream.h"
#include "util/crypto/aes128.h"

const uint8* FSTDataSource::Fetch(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* scratch, uint64 size)
{
	if (const uint8* lent = Lend(clusterIndex, clusterOffset, offset, size))
		return lent;
	return readData(clusterIndex, clusterOffset, offset, scratch, size) == size ? scratch : nullptr;
}

bool FSTDataSource::ReadRawBlock(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* dst, uint32 size,
	const uint8* key, const uint8* iv, uint8* nextIv)
{
	if (size < 16 || (size % 16) != 0)
		return false;
	const uint8* src = Fetch(clusterIndex, clusterOffset, offset, dst, size);
	if (!src)
		return false;
	// taken before an in-place decryption overwrites it
	memcpy(nextIv, src + size - 16, 16);
	AES128_CBC_decrypt(dst, src, size, key, iv);
	return true;
}

bool FSTDataSource::ReadHashedBlock(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* dst,
	const uint8* key, uint32 h0Index)
{
	if (h0Index >= 16)
		return false;
	const uint8* src = Fetch(clusterIndex, clusterOffset, offset, dst, HASHED_BLOCK_SIZE);
	if (!src)
		return false;
	AES128_CBC_decrypt(dst, src, HASHED_BLOCK_HASH_SIZE, key, nullptr);
	// the IV is the decrypted H0 hash: in dst now, apart from src
	uint8 iv[16];
	memcpy(iv, dst + 20 * h0Index, 16);
	AES128_CBC_decrypt(dst + HASHED_BLOCK_HASH_SIZE, src + HASHED_BLOCK_HASH_SIZE,
		HASHED_BLOCK_SIZE - HASHED_BLOCK_HASH_SIZE, key, iv);
	return true;
}

// --- a disc image -----------------------------------------------------------

FSTDataSourceWUD* FSTDataSourceWUD::Open(const fs::path& path)
{
	wud_t* wudFile = wud_open(path);
	if (!wudFile)
		return nullptr;
	FSTDataSourceWUD* ds = new FSTDataSourceWUD();
	ds->m_wudFile = wudFile;
	return ds;
}

FSTDataSourceWUD::~FSTDataSourceWUD()
{
	if (m_wudFile)
		wud_close(m_wudFile);
}

uint64 FSTDataSourceWUD::readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size)
{
	if (size > 0xFFFFFFFF)
		return 0;
	return wud_readData(m_wudFile, data, (uint32)size, clusterOffset + offset + m_baseOffset);
}

const uint8* FSTDataSourceWUD::Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size)
{
	if (size > 0xFFFFFFFF)
		return nullptr;
	return wud_lend(m_wudFile, (uint32)size, clusterOffset + offset + m_baseOffset);
}

// --- a title's content files ------------------------------------------------

FSTDataSourceApp* FSTDataSourceApp::Open(const fs::path& path, std::span<const Content> contents)
{
	uint32 maxIndex = 0;
	for (const Content& c : contents)
		maxIndex = std::max<uint32>(maxIndex, c.index);
	std::unique_ptr<FSTDataSourceApp> ds(new FSTDataSourceApp());
	ds->m_cluster.resize(maxIndex + 1);
	for (const Content& c : contents)
	{
		Cluster& cluster = ds->m_cluster[c.index];
		if (cluster.file)
			return nullptr; // a cluster twice
		cluster.file = FileStream::openFileMapped(path / fmt::format("{:08x}.app", c.contentId));
		if (!cluster.file)
			return nullptr;
		cluster.mapped = cluster.file->MappedSpan();
		if (cluster.mapped.size() != cluster.file->GetSize())
			cluster.mapped = {};
	}
	return ds.release();
}

FSTDataSourceApp::~FSTDataSourceApp()
{
	for (Cluster& c : m_cluster)
		delete c.file;
}

uint64 FSTDataSourceApp::readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size)
{
	// clusterOffset is not used: each file starts at its cluster's start
	if (clusterIndex >= m_cluster.size() || !m_cluster[clusterIndex].file || size > 0xFFFFFFFF)
		return 0;
	Cluster& c = m_cluster[clusterIndex];
	if (!c.mapped.empty())
	{
		if (offset >= c.mapped.size())
			return 0;
		const size_t n = (size_t)std::min<uint64>(size, c.mapped.size() - offset);
		memcpy(data, c.mapped.data() + offset, n);
		return n;
	}
	c.file->SetPosition(offset);
	return c.file->readData(data, (uint32)size);
}

const uint8* FSTDataSourceApp::Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size)
{
	if (clusterIndex >= m_cluster.size())
		return nullptr;
	const std::span<const uint8> m = m_cluster[clusterIndex].mapped;
	if (offset > m.size() || m.size() - offset < size || m.empty())
		return nullptr;
	return m.data() + offset;
}
