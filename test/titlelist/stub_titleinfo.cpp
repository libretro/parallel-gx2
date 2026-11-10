// TitleInfo as the title list test has it: a title is a .wud file whose text
// is "<16 hex digits of title id> <version>", found where the core would
// find a disc image. Its location (the UID) is its path. Everything the list
// asks of a title, and nothing that would read a real image.
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/Filesystem/ZArchiveVFS.h"

#include <fstream>

static bool ReadTitleFile(const fs::path& path, uint64& titleId, uint16& version)
{
	std::ifstream f(path);
	std::string id;
	uint32 v = 0;
	if (!(f >> id >> v))
		return false;
	return TitleIdParser::ParseFromStr(id, titleId) && (version = (uint16)v, true);
}

TitleInfo::TitleInfo(const fs::path& path)
{
	uint64 titleId;
	uint16 version;
	m_fullPath = path;
	m_titleFormat = TitleDataFormat::WUD;
	if (!ReadTitleFile(path, titleId, version))
		return;
	m_parsedAppXml = new ParsedAppXml{titleId, version, 0, 0, 0};
	m_uid = std::hash<std::string>()(_pathToUtf8(path));
	m_isValid = true;
}

TitleInfo::TitleInfo(const fs::path& path, std::string_view subPath) : TitleInfo(path)
{
	m_subPath = subPath;
}

TitleInfo::TitleInfo(const CachedInfo& cachedInfo)
{
	m_cachedInfo = new CachedInfo(cachedInfo);
	m_fullPath = cachedInfo.path;
	m_titleFormat = cachedInfo.titleDataFormat;
	m_uid = std::hash<std::string>()(_pathToUtf8(cachedInfo.path));
	m_isValid = true;
}

TitleInfo::~TitleInfo()
{
	cemu_assert(m_mountpoints.empty());
	delete m_parsedMetaXml;
	delete m_parsedAppXml;
	delete m_parsedCosXml;
	delete m_cachedInfo;
}

bool TitleInfo::IsValid() const { return m_isValid; }
uint64 TitleInfo::GetUID() { return m_uid; }
fs::path TitleInfo::GetPath() const { return m_fullPath; }
bool TitleInfo::ParseXmlInfo() { return m_isValid; }
void TitleInfo::UnmountAll() { m_mountpoints.clear(); }

TitleInfo::CachedInfo TitleInfo::MakeCacheEntry()
{
	CachedInfo e{};
	e.titleDataFormat = m_titleFormat;
	e.path = m_fullPath;
	e.subPath = m_subPath;
	e.titleId = GetAppTitleId();
	e.titleVersion = GetAppTitleVersion();
	e.region = CafeConsoleRegion::USA;
	return e;
}

TitleId TitleInfo::GetAppTitleId() const
{
	return m_parsedAppXml ? m_parsedAppXml->title_id : m_cachedInfo ? m_cachedInfo->titleId : 0;
}

uint16 TitleInfo::GetAppTitleVersion() const
{
	return m_parsedAppXml ? m_parsedAppXml->title_version : m_cachedInfo ? m_cachedInfo->titleVersion : 0;
}

uint32 TitleInfo::GetAppSDKVersion() const { return 0; }
uint32 TitleInfo::GetAppGroup() const { return 0; }
uint32 TitleInfo::GetAppType() const { return 0; }

bool TitleInfo::ParseWuaTitleFolderName(std::string_view name, TitleId& titleIdOut, uint16& titleVersionOut)
{
	return false;
}

// no .wua in this test
ZArchive* ZArchive::Open(const fs::path& path, std::string* whyNot)
{
	return nullptr;
}

bool ZArchive::ListDir(std::string_view path, std::vector<DirEntry>& out) const
{
	return false;
}

ZArchive::~ZArchive() {}

// helpers.cpp's, for ConvertString, which reads the cache file's numbers
std::string_view& ltrim(std::string_view& str, const std::string& chars)
{
	str.remove_prefix(std::min(str.find_first_not_of(chars), str.size()));
	return str;
}
