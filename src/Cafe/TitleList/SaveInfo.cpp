#include "SaveInfo.h"
#include "util/helpers/MappedFile.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "ParsedMetaXml.h"

SaveInfo::SaveInfo(TitleId titleId) : m_titleId(titleId)
{
	m_path = GetSavePath(titleId);
	m_isValid = FileStream::IsDirectory(m_path);
}

std::string SaveInfo::GetStorageSubpathByTitleId(TitleId titleId)
{
	// usr/save/<titleIdHigh>/<titleIdLow>/
	return fmt::format("usr/save/{:08x}/{:08x}", ((uint64)titleId) >> 32, (uint64)titleId & 0xFFFFFFFF);
}

fs::path SaveInfo::GetSavePath(TitleId titleId)
{
	return ActiveSettings::GetMlcPath(GetStorageSubpathByTitleId(titleId));
}

bool SaveInfo::ParseMetaData()
{
	if (m_hasMetaLoaded)
		return m_parsedMetaXml != nullptr;
	m_hasMetaLoaded = true;
	MappedFile xmlData(m_path / "meta/meta.xml");
	if (!xmlData.opened())
		return false;
	m_parsedMetaXml = ParsedMetaXml::Parse(xmlData.data(), xmlData.size());
	return m_parsedMetaXml != nullptr;
}