#include "Common/precompiled.h"
#include "util/helpers/MappedFile.h"
#include "Common/FileStream.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/Filesystem/FST/fst_source.h"
#include "Cafe/Filesystem/FST/fst_volume.h"
#include "util/crypto/aes128.h"
#include "fstUtil.h" // FSTPathUnitTest

#include "FST.h"
#include "KeyCache.h"


#define SET_FST_ERROR(__code) 	if (errorCodeOut) *errorCodeOut = ErrorCode::__code

static_assert(sizeof(NCrypto::AesIv) == 16); // make sure IV is actually 16 bytes

constexpr size_t DISC_SECTOR_SIZE = 0x8000;

struct DiscHeaderA
{
	// header in first sector (0x0)
	uint8 productCode[22]; // ?
};

struct DiscHeaderB
{
	// header at 0x10000
	static constexpr uint32 MAGIC_VALUE = 0xCC549EB9;

	/* +0x00 */ uint32be magic;
};

static_assert(sizeof(DiscHeaderB) == 0x04);

struct DiscPartitionTableHeader
{
	// header at 0x18000, encrypted
	static constexpr uint32 MAGIC_VALUE = 0xCCA6E67B;

	/* +0x00 */ uint32be magic;
	/* +0x04 */ uint32be blockSize; // must be 0x8000?
	/* +0x08 */ uint8 partitionTableHash[20]; // hash of the data range at +0x800 to end of sector (0x8000)
	/* +0x1C */ uint32be numPartitions;
};

static_assert(sizeof(DiscPartitionTableHeader) == 0x20);

struct DiscPartitionTableEntry
{
	/* +0x00 */ uint8be partitionName[31];
	/* +0x1F */ uint8be numAddresses; // ?
	/* +0x20 */ uint32be partitionAddress; // this is an array?
	/* +0x24 */ uint8 padding[0x80 - 0x24];
};

static_assert(sizeof(DiscPartitionTableEntry) == 0x80);

struct DiscPartitionHeader
{
	// header at the beginning of each partition
	static constexpr uint32 MAGIC_VALUE = 0xCC93A4F5;

	/* +0x00 */ uint32be magic;
	/* +0x04 */ uint32be sectorSize; // must match DISC_SECTOR_SIZE for hashed blocks

	/* +0x08 */ uint32be ukn008;
	/* +0x0C */ uint32be ukn00C; // h3 array size?
	/* +0x10 */ uint32be h3HashNum;
	/* +0x14 */ uint32be fstSize; // in bytes
	/* +0x18 */ uint32be fstSector; // relative to partition start
	/* +0x1C */ uint32be ukn01C;
	/* +0x20 */ uint32be ukn020;

	// the hash and encryption mode for the FST cluster
	/* +0x24 */ uint8 fstHashType;
	/* +0x25 */ uint8 fstEncryptionType; // purpose of this isn't really understood. Maybe it controls which key is being used? (1 -> disc key, 2 -> partition key)

	/* +0x26 */ uint8be versionA;
	/* +0x27 */ uint8be ukn027; // also a version field?

	// there is an array at +0x40 ? Related to H3 list. Also related to value at +0x0C and h3HashNum
	/* +0x28 */ uint8be _uknOrPadding028[0x18];
	/* +0x40 */ uint8be h3HashArray[32]; // dynamic size. Only present if fstHashType != 0
};

static_assert(sizeof(DiscPartitionHeader) == 0x40+0x20);

bool FSTVolume::FindDiscKey(const fs::path& path, NCrypto::AesKey& discTitleKey, bool* imageOpened, KeyCacheSearch* search)
{
	if (imageOpened)
		*imageOpened = false;
	cemuLog_log(LogType::Force, "[FST] FindDiscKey begin path='{}'", path.generic_string());
	std::unique_ptr<fst_source_t, decltype(&fst_source_close)> dataSource(fst_source_open_disc(FileStream::HostPath(path).c_str(), 0), fst_source_close);
	if (!dataSource)
	{
		cemuLog_log(LogType::Force, "[FST] FindDiscKey: fst_source_open_disc failed");
		return false;
	}
	cemuLog_log(LogType::Force, "[FST] FindDiscKey: datasource opened");

	// read section of header which should only contain zero bytes if decrypted
	uint8 header[16*3];
	if (fst_source_read(dataSource.get(), 0, 0, 0x18000 + 0x100, header, sizeof(header)) != sizeof(header))
	{
		cemuLog_log(LogType::Force, "[FST] FindDiscKey: failed to read header bytes");
		return false;
	}
	cemuLog_log(LogType::Force, "[FST] FindDiscKey: read header bytes ok (size={})", (sint32)sizeof(header));
	if (imageOpened)
		*imageOpened = true;

	// keys.txt, then the key files beside the image
	return KeyCache_FindDiscKey(path, header, discTitleKey.b, search);
}

// open WUD image using key cache
// if no matching key is found then keyFound will return false
FSTVolume* FSTVolume::OpenFromDiscImage(const fs::path& path, ErrorCode* errorCodeOut)
{
	SET_FST_ERROR(UNKNOWN_ERROR);
	KeyCache_Prepare();
	NCrypto::AesKey discTitleKey;
	if (!FindDiscKey(path, discTitleKey))
	{
		SET_FST_ERROR(DISC_KEY_MISSING);
		return nullptr;
	}
	return OpenFromDiscImage(path, discTitleKey, errorCodeOut);
}

// open WUD image
FSTVolume* FSTVolume::OpenFromDiscImage(const fs::path& path, NCrypto::AesKey& discTitleKey, ErrorCode* errorCodeOut)

{
	// WUD images support multiple partitions, each with their own key and FST
	// the process for loading game data FSTVolume from a WUD image is as follows:
	// 1) parse WUD headers and verify
	// 2) read SI partition FST
	// 3) find main GM partition
	// 4) use SI information to get titleKey for GM partition
	// 5) Load FST for GM
	SET_FST_ERROR(UNKNOWN_ERROR);
	std::unique_ptr<fst_source_t, decltype(&fst_source_close)> dataSource(fst_source_open_disc(FileStream::HostPath(path).c_str(), 0), fst_source_close);
	if (!dataSource)
		return nullptr;
	// check HeaderA (only contains product code?)
	DiscHeaderA headerA{};
	if (fst_source_read(dataSource.get(), 0, 0, 0, &headerA, sizeof(headerA)) != sizeof(headerA))
		return nullptr;
	// check HeaderB
	DiscHeaderB headerB{};
	if (fst_source_read(dataSource.get(), 0, 0, DISC_SECTOR_SIZE * 2, &headerB, sizeof(headerB)) != sizeof(headerB))
		return nullptr;
	if (headerB.magic != headerB.MAGIC_VALUE)
		return nullptr;

	// read, decrypt and parse partition table
	uint8 partitionSector[DISC_SECTOR_SIZE];
	if (fst_source_read(dataSource.get(), 0, 0, DISC_SECTOR_SIZE * 3, partitionSector, DISC_SECTOR_SIZE) != DISC_SECTOR_SIZE)
		return nullptr;
	uint8 iv[16]{};
	AES128_CBC_decrypt(partitionSector, partitionSector, DISC_SECTOR_SIZE, discTitleKey.b, iv);
	// parse partition info
	DiscPartitionTableHeader* partitionHeader = (DiscPartitionTableHeader*)partitionSector;
	if (partitionHeader->magic != DiscPartitionTableHeader::MAGIC_VALUE)
	{
		cemuLog_log(LogType::Force, "Disc image rejected because decryption failed");
		return nullptr;
	}
	if (partitionHeader->blockSize != DISC_SECTOR_SIZE)
	{
		cemuLog_log(LogType::Force, "Disc image rejected because partition sector size is invalid");
		return nullptr;
	}
	uint32 numPartitions = partitionHeader->numPartitions;
	if (numPartitions > 30) // there is space for up to 240 partitions but we use a more reasonable limit
	{
		cemuLog_log(LogType::Force, "Disc image rejected due to exceeding the partition limit (has {} partitions)", numPartitions);
		return nullptr;
	}
	DiscPartitionTableEntry* partitionArray = (DiscPartitionTableEntry*)(partitionSector + 0x800);
	// validate partitions and find SI partition
	uint32 siPartitionIndex = std::numeric_limits<uint32>::max();
	uint32 gmPartitionIndex = std::numeric_limits<uint32>::max();
	for (uint32 i = 0; i < numPartitions; i++)
	{
		if (partitionArray[i].numAddresses != 1)
		{
			cemuLog_log(LogType::Force, "Disc image has unsupported partition with {} addresses", (uint32)partitionArray[i].numAddresses);
			return nullptr;
		}
		auto& name = partitionArray[i].partitionName;
		if (name[0] == 'S' && name[1] == 'I')
		{
			if (siPartitionIndex != std::numeric_limits<uint32>::max())
			{
				cemuLog_log(LogType::Force, "Disc image has multiple SI partitions. Not supported");
				return nullptr;
			}
			siPartitionIndex = i;
		}
		if (name[0] == 'G' && name[1] == 'M')
		{
			if (gmPartitionIndex == std::numeric_limits<uint32>::max())
				gmPartitionIndex = i; // we use the first GM partition we find. This is likely not correct but it seems to work for practically all disc images
		}
	}
	if (siPartitionIndex == std::numeric_limits<uint32>::max() || gmPartitionIndex == std::numeric_limits<uint32>::max())
	{
		cemuLog_log(LogType::Force, "Disc image has no SI or GM partition. Cannot read game data");
		return nullptr;
	}

	// read and verify partition headers for SI and GM
	auto readPartitionHeader = [&](DiscPartitionHeader& partitionHeader, uint32 partitionIndex) -> bool
	{
		cemu_assert_debug(fst_source_get_base_offset(dataSource.get()) == 0);
		if (fst_source_read(dataSource.get(), 0, 0, partitionArray[partitionIndex].partitionAddress * DISC_SECTOR_SIZE, &partitionHeader, sizeof(DiscPartitionHeader)) != sizeof(DiscPartitionHeader))
			return false;
		if (partitionHeader.magic != partitionHeader.MAGIC_VALUE && partitionHeader.sectorSize != DISC_SECTOR_SIZE)
			return false;
		return true;
	};

	// SI partition
	DiscPartitionHeader partitionHeaderSI{};
	if (!readPartitionHeader(partitionHeaderSI, siPartitionIndex))
	{
		cemuLog_log(LogType::Force, "Disc image SI partition header is invalid");
		return nullptr;
	}

	cemu_assert_debug(partitionHeaderSI.fstHashType == 0);
	cemu_assert_debug(partitionHeaderSI.fstEncryptionType == 1);
	// todo - check other fields?

	if(partitionHeaderSI.fstHashType == 0 && partitionHeaderSI.h3HashNum != 0)
		cemuLog_log(LogType::Force, "FST: Partition uses unhashed blocks but stores a non-zero amount of H3 hashes");

	// GM partition
	DiscPartitionHeader partitionHeaderGM{};
	if (!readPartitionHeader(partitionHeaderGM, gmPartitionIndex))
	{
		cemuLog_log(LogType::Force, "Disc image GM partition header is invalid");
		return nullptr;
	}
	cemu_assert_debug(partitionHeaderGM.fstHashType == 1);
	cemu_assert_debug(partitionHeaderGM.fstEncryptionType == 2);

	// if decryption is necessary
	// load SI FST
	fst_source_set_base_offset(dataSource.get(), (uint64)partitionArray[siPartitionIndex].partitionAddress * DISC_SECTOR_SIZE);
	auto siFST = OpenFST(dataSource.get(), false, (uint64)partitionHeaderSI.fstSector * DISC_SECTOR_SIZE, partitionHeaderSI.fstSize, discTitleKey, nullptr);
	if (!siFST)
		return nullptr;
	cemu_assert_debug(!(fst_table_hash_disabled(siFST->m_table) && partitionHeaderSI.h3HashNum != 0)); // if hash is disabled, no H3 data may be present
	// load ticket file for partition that we want to decrypt
	NCrypto::ETicketParser ticketParser;
	std::vector<uint8> ticketData = siFST->ExtractFile(fmt::format("{:02x}/title.tik", gmPartitionIndex));
	if (ticketData.empty() || !ticketParser.parse(ticketData.data(), ticketData.size()))
	{
		cemuLog_log(LogType::Force, "Disc image ticket file is invalid");
		return nullptr;
	}
#if 0
	// each SI partition seems to contain a title.tmd that we could parse and which should have information about the associated GM partition
	// but the console seems to ignore this file for disc images, at least when mounting, so we shouldn't rely on it either
	std::vector<uint8> tmdData = siFST->ExtractFile(fmt::format("{:02x}/title.tmd", gmPartitionIndex));
	if (tmdData.empty())
	{
		cemuLog_log(LogType::Force, "Disc image TMD file is missing");
		return nullptr;
	}
	// parse TMD
	NCrypto::TMDParser tmdParser;
	if (!tmdParser.parse(tmdData.data(), tmdData.size()))
	{
		cemuLog_log(LogType::Force, "Disc image TMD file is invalid");
		return nullptr;
	}
#endif
	delete siFST;
	NCrypto::AesKey gmTitleKey;
	ticketParser.GetTitleKey(gmTitleKey);
	// load GM partition
	fst_source_set_base_offset(dataSource.get(), (uint64)partitionArray[gmPartitionIndex].partitionAddress * DISC_SECTOR_SIZE);
	FSTVolume* r = OpenFST(dataSource.release(), true, (uint64)partitionHeaderGM.fstSector * DISC_SECTOR_SIZE, partitionHeaderGM.fstSize, gmTitleKey, nullptr);
	if (r)
		SET_FST_ERROR(OK);
	cemu_assert_debug(!(r && fst_table_hash_disabled(r->m_table) && partitionHeaderGM.h3HashNum != 0)); // if hash is disabled, no H3 data may be present
	return r;
}

FSTVolume* FSTVolume::OpenFromContentFolder(fs::path folderPath, ErrorCode* errorCodeOut)
{
	SET_FST_ERROR(UNKNOWN_ERROR);
	// load TMD
	MappedFile tmdData(folderPath / "title.tmd");
	if (!tmdData.opened())
		return nullptr;
	NCrypto::TMDParser tmdParser;
	if (!tmdParser.parse(tmdData.data(), tmdData.size()))
	{
		SET_FST_ERROR(BAD_TITLE_TMD);
		return nullptr;
	}
	// load ticket
	MappedFile ticketData(folderPath / "title.tik");
	if (!ticketData.opened())
	{
		SET_FST_ERROR(TITLE_TIK_MISSING);
		return nullptr;
	}
	NCrypto::ETicketParser ticketParser;
	if (!ticketParser.parse(ticketData.data(), ticketData.size()))
	{
		SET_FST_ERROR(BAD_TITLE_TIK);
		return nullptr;
	}
	NCrypto::AesKey titleKey;
	ticketParser.GetTitleKey(titleKey);
	// open data source
	// each content's file as the host has it: <contentId>.app, in hex
	std::vector<std::string> paths;
	std::vector<const char*> pathPtrs;
	std::vector<uint16> clusters;
	for (const auto& itr : tmdParser.GetContentList())
	{
		paths.push_back(FileStream::HostPath(folderPath / fmt::format("{:08x}.app", itr.contentId)));
		clusters.push_back(itr.index);
	}
	for (const std::string& p : paths)
		pathPtrs.push_back(p.c_str());
	fst_source_t* dataSource = fst_source_open_app(pathPtrs.data(), clusters.data(), clusters.size(), 0);
	if (!dataSource)
		return nullptr;
	// the FST is all of the first content (cluster 0)
	uint32 fstSize = 0;
	for (auto& itr : tmdParser.GetContentList())
	{
		if (itr.index == 0)
		{
			cemu_assert_debug(itr.size <= 0xFFFFFFFF);
			fstSize = (uint32)itr.size;
		}
	}
	FSTVolume* fstVolume = FSTVolume::OpenFST(dataSource, true, 0, fstSize, titleKey, &tmdParser);
	if (fstVolume)
		SET_FST_ERROR(OK);
	return fstVolume;
}

FSTVolume::FSTVolume(fst_volume_t* volume) : m_volume(volume), m_table(fst_volume_table(volume)) {}

FSTVolume* FSTVolume::OpenFST(fst_source_t* dataSource, bool takeSource, uint64 fstOffset, uint32 fstSize, const NCrypto::AesKey& partitionTitleKey, NCrypto::TMDParser* optionalTMD)
{
	// each content in the TMD is a cluster: what a raw one is hashed to
	std::vector<fst_volume_content_t> contents;
	if (optionalTMD)
	{
		auto& contentList = optionalTMD->GetContentList();
		contents.resize(contentList.size());
		for (size_t i = 0; i < contentList.size(); i++)
		{
			contents[i].size = contentList[i].size;
			contents[i].sha1 = HAS_FLAG(contentList[i].contentFlags, NCrypto::TMDParser::TMDContentFlags::FLAG_SHA1);
			static_assert(sizeof(contentList[i].hash32) == sizeof(contents[i].hash));
			memcpy(contents[i].hash, contentList[i].hash32, sizeof(contents[i].hash));
		}
	}
	int why = FST_VOLUME_OPEN_OK;
	fst_volume_t* volume = fst_volume_open(dataSource, takeSource, fstOffset, fstSize, partitionTitleKey.b,
		optionalTMD ? contents.data() : nullptr, (uint32)contents.size(), &why);
	if (!volume)
	{
		if (why == FST_VOLUME_OPEN_INVALID)
			cemuLog_log(LogType::Force, "FST is not valid");
		else if (why == FST_VOLUME_OPEN_CONTENTS)
			cemuLog_log(LogType::Force, "FST: Number of clusters does not match TMD content list");
		return nullptr;
	}
	return new FSTVolume(volume);
}

FSTVolume* FSTVolume::OpenFromDataSource(fst_source_t* dataSource, uint64 fstOffset, uint32 fstSize, const NCrypto::AesKey& key)
{
	return OpenFST(dataSource, true, fstOffset, fstSize, key, nullptr);
}

bool FSTVolume::HasCorruption() const
{
	return fst_volume_corrupted(m_volume);
}

uint32 FSTVolume::GetFileCount() const
{
	return fst_table_file_count(m_table);
}

bool FSTVolume::OpenFile(std::string_view path, FSTFileHandle& fileHandleOut, bool openOnlyFiles)
{
	const uint32 index = fst_table_lookup(m_table, path.data(), path.size(), openOnlyFiles);
	if (index == FST_TABLE_NONE)
		return false;
	fileHandleOut.m_fstIndex = index;
	return true;
}

bool FSTVolume::IsDirectory(const FSTFileHandle& fileHandle) const
{
	return fst_table_is_dir(m_table, fileHandle.m_fstIndex);
}

bool FSTVolume::IsFile(const FSTFileHandle& fileHandle) const
{
	return fst_table_is_file(m_table, fileHandle.m_fstIndex);
}

bool FSTVolume::HasLinkFlag(const FSTFileHandle& fileHandle) const
{
	return fst_table_is_link(m_table, fileHandle.m_fstIndex);
}

std::string_view FSTVolume::GetName(const FSTFileHandle& fileHandle) const
{
	return fst_table_name(m_table, fileHandle.m_fstIndex);
}

std::string FSTVolume::GetPath(const FSTFileHandle& fileHandle) const
{
	std::string path(fst_table_path(m_table, fileHandle.m_fstIndex, nullptr, 0), '\0');
	fst_table_path(m_table, fileHandle.m_fstIndex, path.data(), path.size() + 1);
	return path;
}

uint32 FSTVolume::GetFileSize(const FSTFileHandle& fileHandle) const
{
	fst_table_file_t file;
	if (!fst_table_file(m_table, fileHandle.m_fstIndex, &file))
		return 0;
	return file.size;
}

uint32 FSTVolume::ReadFile(FSTFileHandle& fileHandle, uint32 offset, uint32 size, void* dataOut)
{
	cemu_assert_debug(!fst_table_is_link(m_table, fileHandle.m_fstIndex));
	fst_volume_error_t err;
	const uint32 read = fst_volume_read(m_volume, fileHandle.m_fstIndex, offset, size, dataOut, &err);
	switch (err.kind)
	{
	case FST_VOLUME_READ_OK:
		break;
	case FST_VOLUME_READ_IV:
		cemuLog_log(LogType::Force, "Failed to read IV for raw FST block");
		break;
	case FST_VOLUME_READ_RAW:
		cemuLog_log(LogType::Force, "Failed to read raw FST block");
		break;
	case FST_VOLUME_READ_RAW_HASH:
		cemuLog_log(LogType::Force, "FST: Raw section hash mismatch");
		break;
	case FST_VOLUME_READ_HASHED:
		cemuLog_log(LogType::Force, "Failed to read hashed FST block");
		break;
	case FST_VOLUME_READ_H0:
		cemuLog_log(LogType::Force, "FST: Hash H0 mismatch in hashed block (section {} index {})", err.cluster, err.block);
		break;
	case FST_VOLUME_READ_MODE:
		cemu_assert_debug(false);
		break;
	default:
		cemuLog_log(LogType::Force, "FST: no memory to read with");
		break;
	}
	return read;
}

bool FSTVolume::OpenDirectoryIterator(std::string_view path, FSTDirectoryIterator& directoryIteratorOut)
{
	FSTFileHandle fileHandle;
	if (!OpenFile(path, fileHandle, false))
		return false;
	if (!fst_table_dir_open(m_table, fileHandle.m_fstIndex, &directoryIteratorOut.entries))
		return false;
	directoryIteratorOut.dirHandle = fileHandle;
	return true;
}

bool FSTVolume::Next(FSTDirectoryIterator& directoryIterator, FSTFileHandle& fileHandleOut)
{
	const uint32 index = fst_table_dir_next(m_table, &directoryIterator.entries);
	if (index == FST_TABLE_NONE)
		return false;
	fileHandleOut.m_fstIndex = index;
	return true;
}

FSTVolume::~FSTVolume()
{
	fst_volume_free(m_volume);
}

void FSTVolumeTest()
{
	FSTPathUnitTest();
}
