#pragma once

#include <span>
#include <string_view>
#include <optional>
#include <utility>

#include "util/IniParser/ini_file.h"

// An ini file walked a section at a time, over the C parser (ini_file.h)
class IniParser
{
public:
	// the contents are copied: a span borrowed from a mapping will do
	IniParser(std::span<const char> iniContents, std::string_view name = {}) : m_ini(ini_file_parse(iniContents.data(), iniContents.size())) {}
	IniParser(std::span<const unsigned char> iniContents, std::string_view name = {}) : IniParser(std::span<const char>((const char*)iniContents.data(), iniContents.size()), name) {}
	IniParser(const IniParser&) = delete;
	IniParser& operator=(const IniParser&) = delete;
	~IniParser() { ini_file_free(m_ini); }

	// the first call moves to the first section
	bool NextSection()
	{
		const size_t count = m_ini ? m_ini->section_count : 0;
		if (m_current == kNone)
			m_current = 0;
		else if (m_current < count)
			m_current++;
		return m_current < count;
	}

	std::string_view GetCurrentSectionName() const
	{
		const ini_section_t* s = Current();
		return s ? View(s->name) : std::string_view();
	}

	size_t GetCurrentSectionLineNumber() const
	{
		const ini_section_t* s = Current();
		return s ? s->line : 0;
	}

	// the first option of that name in this section, ignoring case
	std::optional<std::string_view> FindOption(std::string_view optionName) const
	{
		if (!Current())
			return std::nullopt;
		const ini_option_t* o = ini_file_find(m_ini, m_current, optionName.data(), optionName.size());
		if (!o)
			return std::nullopt;
		return View(o->value);
	}

	// this section's options, as (name, value)
	class Options
	{
	public:
		class Iterator
		{
		public:
			explicit Iterator(const ini_option_t* o) : m_o(o) {}
			std::pair<std::string_view, std::string_view> operator*() const { return {View(m_o->name), View(m_o->value)}; }
			Iterator& operator++() { m_o++; return *this; }
			bool operator!=(const Iterator& other) const { return m_o != other.m_o; }
		private:
			const ini_option_t* m_o;
		};
		Options(const ini_option_t* first, size_t count) : m_first(first), m_count(count) {}
		Iterator begin() const { return Iterator(m_first); }
		Iterator end() const { return Iterator(m_first + m_count); }
	private:
		const ini_option_t* m_first;
		size_t m_count;
	};

	Options GetAllOptions() const
	{
		const ini_section_t* s = Current();
		return s ? Options(m_ini->options + s->first, s->count) : Options(nullptr, 0);
	}

private:
	static constexpr size_t kNone = ~(size_t)0;

	static std::string_view View(ini_span_t v)
	{
		return std::string_view(v.s, v.len);
	}

	const ini_section_t* Current() const
	{
		if (!m_ini || m_current == kNone || m_current >= m_ini->section_count)
			return nullptr;
		return &m_ini->sections[m_current];
	}

	ini_file_t* m_ini;
	size_t m_current{kNone};
};
