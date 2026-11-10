#include "Cafe/Filesystem/FST/FSTDataSource.h"
#include "Cafe/Filesystem/WUD/wud.h"
#include "Common/shared_file.h"
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

bool FSTDataSource::ReadHashedBlock(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint8* hashDst, uint8* dataDst,
	const uint8* key, uint32 h0Index)
{
	if (h0Index >= 16)
		return false;
	// lent in one piece where the source can, each part read where it goes
	// where it cannot
	const uint8* src = Lend(clusterIndex, clusterOffset, offset, HASHED_BLOCK_SIZE);
	const uint8* hashSrc = src;
	const uint8* dataSrc = src ? src + HASHED_BLOCK_HASH_SIZE : nullptr;
	if (!src)
	{
		hashSrc = Fetch(clusterIndex, clusterOffset, offset, hashDst, HASHED_BLOCK_HASH_SIZE);
		dataSrc = Fetch(clusterIndex, clusterOffset, offset + HASHED_BLOCK_HASH_SIZE, dataDst, HASHED_BLOCK_SIZE - HASHED_BLOCK_HASH_SIZE);
		if (!hashSrc || !dataSrc)
			return false;
	}
	AES128_CBC_decrypt(hashDst, hashSrc, HASHED_BLOCK_HASH_SIZE, key, nullptr);
	// the IV is the decrypted H0 hash: in hashDst now, apart from the source
	uint8 iv[16];
	memcpy(iv, hashDst + 20 * h0Index, 16);
	AES128_CBC_decrypt(dataDst, dataSrc, HASHED_BLOCK_SIZE - HASHED_BLOCK_HASH_SIZE, key, iv);
	return true;
}

// --- a disc image -----------------------------------------------------------

FSTDataSourceWUD* FSTDataSourceWUD::Open(const fs::path& path, bool map)
{
	wud_t* wudFile = wud_open(FileStream::HostPath(path).c_str(), map ? 0 : WUD_OPEN_NO_MAP);
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

FSTDataSourceApp* FSTDataSourceApp::Open(const fs::path& path, std::span<const Content> contents, bool map)
{
	uint32 maxIndex = 0;
	for (const Content& c : contents)
		maxIndex = std::max<uint32>(maxIndex, c.index);
	std::unique_ptr<FSTDataSourceApp> ds(new FSTDataSourceApp());
	ds->m_cluster.resize(maxIndex + 1);
	for (const Content& c : contents)
	{
		shared_file_t*& file = ds->m_cluster[c.index];
		if (file)
			return nullptr; // a cluster twice
		const fs::path appPath = path / fmt::format("{:08x}.app", c.contentId);
		file = shared_file_open(FileStream::HostPath(appPath).c_str(), map ? 0 : SHARED_FILE_NO_MAP);
		if (!file)
			return nullptr;
	}
	return ds.release();
}

FSTDataSourceApp::~FSTDataSourceApp()
{
	for (shared_file_t* file : m_cluster)
		shared_file_close(file);
}

uint64 FSTDataSourceApp::readData(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* data, uint64 size)
{
	// clusterOffset is not used: each file starts at its cluster's start
	if (clusterIndex >= m_cluster.size() || !m_cluster[clusterIndex] || size > 0xFFFFFFFF)
		return 0;
	const int64_t got = shared_file_read_at(m_cluster[clusterIndex], data, (int64_t)size, offset);
	return got > 0 ? (uint64)got : 0;
}

const uint8* FSTDataSourceApp::Lend(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, uint64 size)
{
	if (clusterIndex >= m_cluster.size() || !m_cluster[clusterIndex])
		return nullptr;
	shared_file_t* file = m_cluster[clusterIndex];
	const uint8* m = shared_file_mapped(file);
	const uint64 fileSize = shared_file_size(file);
	if (!m || offset > fileSize || fileSize - offset < size)
		return nullptr;
	return m + offset;
}
