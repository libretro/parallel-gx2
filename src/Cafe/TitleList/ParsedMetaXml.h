#pragma once

#include "config/CemuConfig.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/title_xml.h"

// A title's meta/meta.xml (or a WUHB's meta/meta.ini), parsed in C
// (title_xml.h)
struct ParsedMetaXml
{
	title_meta_t m{};

	ParsedMetaXml() = default;
	ParsedMetaXml(const ParsedMetaXml& other) { title_meta_copy(&m, &other.m); }
	ParsedMetaXml& operator=(const ParsedMetaXml&) = delete;
	~ParsedMetaXml() { title_meta_free(&m); }

	std::string GetShortName(CafeConsoleLanguage languageId) const
	{
		return Named(m.short_name, languageId);
	}

	std::string GetLongName(CafeConsoleLanguage languageId) const
	{
		return Named(m.long_name, languageId);
	}

	TitleId GetTitleId() const
	{
		return m.title_id;
	}

	uint16 GetTitleVersion() const
	{
		return (uint16)m.version;
	}

	CafeConsoleRegion GetRegion() const
	{
		return (CafeConsoleRegion)m.region;
	}

	std::string GetProductCode() const
	{
		return m.product_code ? m.product_code : "";
	}

	std::string GetCompanyCode() const
	{
		return m.company_code ? m.company_code : "";
	}

	uint32 GetOlvAccesskey() const
	{
		return m.olv_accesskey;
	}

	// nullptr if it is not a meta.xml, and then in @whyNot, if given, which
	// part of one it lacks
	static ParsedMetaXml* Parse(const uint8* xmlData, size_t xmlSize, std::string* whyNot = nullptr)
	{
		char why[256] = {};
		auto parsed = std::make_unique<ParsedMetaXml>();
		if (!title_meta_parse(xmlData, xmlSize, &parsed->m, why, sizeof(why)))
		{
			if (whyNot)
				*whyNot = why;
			return nullptr;
		}
		return parsed.release();
	}

	// a WUHB's meta/meta.ini; nullptr if it has no [menu]
	static ParsedMetaXml* ParseIni(const uint8* iniData, size_t iniSize)
	{
		auto parsed = std::make_unique<ParsedMetaXml>();
		if (!title_meta_parse_ini(iniData, iniSize, &parsed->m))
			return nullptr;
		return parsed.release();
	}

private:
	// the language's, or the English one when it has none
	static std::string Named(char* const (&names)[TITLE_LANGUAGES], CafeConsoleLanguage languageId)
	{
		const char* n = names[(size_t)languageId];
		if (!n || !*n)
			n = names[(size_t)CafeConsoleLanguage::EN];
		return n ? n : "";
	}
};
