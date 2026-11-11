#include "Common/precompiled.h"
#include "util/helpers/MappedFile.h"
#include "Common/FileStream.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/Filesystem/FST/fst_source.h"
#include "Cafe/Filesystem/FST/fst_volume.h"
#include "Cafe/Filesystem/FST/fst_open.h"
#include "fstUtil.h" // FSTPathUnitTest

#include "FST.h"
#include "KeyCache.h"


#define SET_FST_ERROR(__code) 	if (errorCodeOut) *errorCodeOut = ErrorCode::__code

static_assert(sizeof(NCrypto::AesIv) == 16); // make sure IV is actually 16 bytes

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
	uint8 header[FST_DISC_KEY_PROBE_SIZE];
	if (fst_source_read(dataSource.get(), 0, 0, FST_DISC_KEY_PROBE_OFFSET, header, sizeof(header)) != sizeof(header))
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

// open WUD image: its game partition (fst_open.h)
FSTVolume* FSTVolume::OpenFromDiscImage(const fs::path& path, NCrypto::AesKey& discTitleKey, ErrorCode* errorCodeOut)
{
	SET_FST_ERROR(UNKNOWN_ERROR);
	int why = FST_DISC_OK;
	uint32 detail = 0;
	fst_volume_t* volume = fst_disc_open(FileStream::HostPath(path).c_str(), discTitleKey.b, &why, &detail);
	switch (why)
	{
	case FST_DISC_OK:
		break;
	case FST_DISC_WRONG_KEY:
		cemuLog_log(LogType::Force, "Disc image rejected because decryption failed");
		break;
	case FST_DISC_BAD_TABLE:
		cemuLog_log(LogType::Force, "Disc image rejected: its partition table is not one that can be read ({})", detail);
		break;
	case FST_DISC_NO_PARTITION:
		cemuLog_log(LogType::Force, "Disc image has no SI or GM partition, or more than one SI partition. Cannot read game data");
		break;
	case FST_DISC_BAD_PARTITION:
		cemuLog_log(LogType::Force, "Disc image SI or GM partition header is invalid");
		break;
	case FST_DISC_BAD_TICKET:
		cemuLog_log(LogType::Force, "Disc image ticket file is invalid");
		break;
	case FST_DISC_SI_FST:
	case FST_DISC_GM_FST:
		cemuLog_log(LogType::Force, "Disc image {} partition FST is not valid", why == FST_DISC_SI_FST ? "SI" : "GM");
		break;
	default:
		break;
	}
	if (!volume)
		return nullptr;
	SET_FST_ERROR(OK);
	return new FSTVolume(volume);
}

FSTVolume* FSTVolume::OpenFromContentFolder(fs::path folderPath, ErrorCode* errorCodeOut)
{
	SET_FST_ERROR(UNKNOWN_ERROR);
	// its TMD and ticket, then its contents (fst_open.h)
	MappedFile tmdData(folderPath / "title.tmd");
	if (!tmdData.opened())
		return nullptr;
	fst_tmd_t tmd;
	if (!fst_tmd_parse(tmdData.data(), tmdData.size(), &tmd))
	{
		cemuLog_log(LogType::Force, "TMD of {} is not valid", _pathToUtf8(folderPath));
		SET_FST_ERROR(BAD_TITLE_TMD);
		return nullptr;
	}
	MappedFile ticketData(folderPath / "title.tik");
	NCrypto::AesKey titleKey;
	if (!ticketData.opened() || !fst_ticket_title_key(ticketData.data(), ticketData.size(), titleKey.b))
	{
		if (ticketData.opened())
			SET_FST_ERROR(BAD_TITLE_TIK);
		else
			SET_FST_ERROR(TITLE_TIK_MISSING);
		fst_tmd_free(&tmd);
		return nullptr;
	}
	// each content's file as the host has it: <contentId>.app, in hex
	std::vector<std::string> paths(tmd.count);
	std::vector<const char*> pathPtrs(tmd.count);
	for (uint16 i = 0; i < tmd.count; i++)
	{
		paths[i] = FileStream::HostPath(folderPath / fmt::format("{:08x}.app", tmd.contents[i].id));
		pathPtrs[i] = paths[i].c_str();
	}
	int why = FST_TITLE_OK;
	fst_volume_t* volume = fst_title_open(&tmd, titleKey.b, pathPtrs.data(), &why);
	fst_tmd_free(&tmd);
	if (!volume)
	{
		if (why == FST_TITLE_NO_FST)
			cemuLog_log(LogType::Force, "TMD of {} lists no content for the FST", _pathToUtf8(folderPath));
		else if (why == FST_TITLE_BAD_FST)
			cemuLog_log(LogType::Force, "FST of {} is not valid", _pathToUtf8(folderPath));
		return nullptr;
	}
	SET_FST_ERROR(OK);
	return new FSTVolume(volume);
}

FSTVolume::FSTVolume(fst_volume_t* volume) : m_volume(volume), m_table(fst_volume_table(volume)) {}

FSTVolume* FSTVolume::OpenFromDataSource(fst_source_t* dataSource, uint64 fstOffset, uint32 fstSize, const NCrypto::AesKey& key)
{
	int why = FST_VOLUME_OPEN_OK;
	fst_volume_t* volume = fst_volume_open(dataSource, true, fstOffset, fstSize, key.b, nullptr, 0, &why);
	if (!volume)
	{
		if (why == FST_VOLUME_OPEN_INVALID)
			cemuLog_log(LogType::Force, "FST is not valid");
		return nullptr;
	}
	return new FSTVolume(volume);
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
