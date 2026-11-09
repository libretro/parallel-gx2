#pragma once

#include "util/xml/Xml.h"
#include "util/helpers/MappedFile.h"
#include "Common/FileStream.h"
#include "config/ConfigValue.h"

#include <string>
#include <mutex>

template <typename T>
concept HasConstCharConstructor = requires { T(std::declval<const char*>()); };

// Values are read and written as tinyxml2, which this used to be built on,
// read and wrote them, so the settings files stay as they were.
namespace XMLConfigText
{
	inline bool ToInt(const char* s, sint32& v) { int i; if (!s || sscanf(s, "%d", &i) != 1) return false; v = i; return true; }
	inline bool ToUnsigned(const char* s, uint32& v) { unsigned u; if (!s || sscanf(s, "%u", &u) != 1) return false; v = u; return true; }
	inline bool ToInt64(const char* s, sint64& v) { long long i; if (!s || sscanf(s, "%lld", &i) != 1) return false; v = (sint64)i; return true; }
	inline bool ToFloat(const char* s, float& v) { return s && sscanf(s, "%f", &v) == 1; }
	inline bool ToDouble(const char* s, double& v) { return s && sscanf(s, "%lf", &v) == 1; }
	inline bool ToBool(const char* s, bool& v)
	{
		sint32 i;
		if (ToInt(s, i)) { v = i != 0; return true; }
		if (s && !strcmp(s, "true")) { v = true; return true; }
		if (s && !strcmp(s, "false")) { v = false; return true; }
		return false;
	}
	template<typename T>
	T Parse(const char* s, T def)
	{
		if constexpr (std::is_same_v<T, bool>) { bool v; return ToBool(s, v) ? v : def; }
		else if constexpr (std::is_same_v<T, float>) { float v; return ToFloat(s, v) ? v : def; }
		else if constexpr (std::is_same_v<T, double>) { double v; return ToDouble(s, v) ? v : def; }
		else if constexpr (std::is_same_v<T, sint32>) { sint32 v; return ToInt(s, v) ? v : def; }
		else if constexpr (std::is_same_v<T, uint32>) { uint32 v; return ToUnsigned(s, v) ? v : def; }
		else if constexpr (std::is_same_v<T, sint64>) { sint64 v; return ToInt64(s, v) ? v : def; }
		else if constexpr (std::is_same_v<T, uint64>) { sint64 v; return ToInt64(s, v) ? (uint64)v : def; } // doesnt support real uint64...
		else return def;
	}
	inline std::string ToStr(const char* v) { return v ? v : ""; }
	inline std::string ToStr(const std::string& v) { return v; }
	inline std::string ToStr(bool v) { return v ? "true" : "false"; }
	inline std::string ToStr(float v) { char b[32]; snprintf(b, sizeof(b), "%.8g", v); return b; }
	inline std::string ToStr(double v) { char b[40]; snprintf(b, sizeof(b), "%.17g", v); return b; }
	inline std::string ToStr(sint32 v) { return fmt::format("{}", v); }
	inline std::string ToStr(uint32 v) { return fmt::format("{}", v); }
	inline std::string ToStr(sint64 v) { return fmt::format("{}", v); }
	inline std::string ToStr(uint64 v) { return fmt::format("{}", (sint64)v); }
}

class XMLConfigParser
{
public:
	XMLConfigParser(xml::Document* document)
		: m_document(document), m_current_element(nullptr), m_is_root(true) {}

private:
	XMLConfigParser(xml::Document* document, xml::Element* element)
		: m_document(document), m_current_element(element), m_is_root(false) {}

	// the element this parser stands for: the document itself at the root
	xml::Node Base() const { return m_current_element ? xml::Node(m_current_element) : xml::Node(*m_document); }
	// an element's text, nullptr if it has none (tinyxml2's GetText)
	static const char* TextOf(xml::Node n) { return n && n.text() ? n.text().get() : nullptr; }
	const char* AttributeOf(const char* name) const
	{
		xml::Attribute a = xml::Node(m_current_element).attribute(name);
		return a ? a.value() : nullptr;
	}

public:
	template <typename T>
	auto get(const char* name, T default_value = {})
	{
		if constexpr(std::is_enum_v<T>)
			return static_cast<T>(get(name, static_cast<std::underlying_type_t<T>>(default_value)));

		const xml::Node element = Base().child(name);
		if (!element)
			return default_value;

		if constexpr (std::is_arithmetic_v<T>)
			return XMLConfigText::Parse<T>(TextOf(element), default_value);
		else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, std::string> || HasConstCharConstructor<T>)
		{
			const char* text = TextOf(element);
			return text ? text : default_value;
		}
		else
			return default_value;
	}
	
	template <typename T>
	T get(const char* name, const ConfigValue<T>& default_value)
	{
		return get(name, default_value.GetInitValue());
	}
	
	template <typename T>
	T get(const char* name, const ConfigValueBounds<T>& default_value)
	{
		return get(name, default_value.GetInitValue());
	}

	template <typename T>
	auto get_attribute(const char* name, T default_value = {})
	{
		if constexpr (std::is_enum_v<T>)
			return static_cast<T>(get_attribute(name, static_cast<std::underlying_type_t<T>>(default_value)));

		if (m_current_element == nullptr)
			return default_value;

		if constexpr (std::is_arithmetic_v<T>)
			return XMLConfigText::Parse<T>(AttributeOf(name), default_value);
		else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, std::string>)
		{
			const char* text = AttributeOf(name);
			return text ? text : default_value;
		}
		else
			return default_value;
	}

	template <typename T>
	T get_attribute(const char* name, const ConfigValue<T>& default_value)
	{
		return get_attribute(name, default_value.GetInitValue());
	}

	template <typename T>
	T get_attribute(const char* name, const ConfigValueBounds<T>& default_value)
	{
		return get_attribute(name, default_value.GetInitValue());
	}

	int char2int(char input)
	{
	  if(input >= '0' && input <= '9')
	    return input - '0';
	  if(input >= 'A' && input <= 'F')
	    return input - 'A' + 10;
	  if(input >= 'a' && input <= 'f')
	    return input - 'a' + 10;
	  throw std::invalid_argument("Invalid input string");
	}

	template <typename TType, size_t TSize>
	std::array<TType, TSize>& get(const char* name, std::array<TType, TSize>& arr)
	{
		arr = {};
		const xml::Node element = Base().child(name);
		if (!element)
			return arr;

		const char* text = TextOf(element);
		if(text)
		{
			assert(strlen(text) == (arr.size() * 2));
			std::istringstream iss(text);
			for(int i = 0; i < arr.size(); ++i)
			{
				arr[i] = (char2int(text[i*2]) << 4) + char2int(text[i * 2 + 1]);
			}
		}

		return arr;
	}
	
	template <typename T>
	T value(T default_value = T())
	{
		//peterBreak();
		return default_value;
	}

	bool value(bool default_value) { return XMLConfigText::Parse(Text(), default_value); }
	float value(float default_value) { return XMLConfigText::Parse(Text(), default_value); }
	double value(double default_value) { return XMLConfigText::Parse(Text(), default_value); }
	uint32 value(uint32 default_value) { return XMLConfigText::Parse(Text(), default_value); }
	sint32 value(sint32 default_value) { return XMLConfigText::Parse(Text(), default_value); }
	uint64 value(uint64 default_value) { return XMLConfigText::Parse(Text(), default_value); }
	sint64 value(sint64 default_value) { return XMLConfigText::Parse(Text(), default_value); }

	const char* value(const char* default_value)
	{
		const char* text = Text();
		return text ? text : default_value;
	}

	template <typename TType, size_t TSize>
	void set(const char* name, const std::array<TType, TSize>& value)
	{
		std::stringstream str;
		for(const auto& v : value)
		{
			str << fmt::format("{:02x}", v);
		}

		Base().append_child(name).text().set(str.str());
	}

	template <typename T>
	void set(const char* name, T value)
	{
		xml::Node element = Base().append_child(name);

		if constexpr (std::is_enum_v<T>)
			element.text().set(fmt::format("{}", static_cast<typename std::underlying_type<T>::type>(value)));
		else
			element.text().set(fmt::format("{}", value));
	}

	template <typename T>
	void set(const char* name, const std::atomic<T>& value)
	{
		set(name, value.load());
	}

	template <typename T>
	void set(const char* name, const ConfigValue<T>& value)
	{
		set(name, value.GetValue());
	}

	void set(const char* name, uint64 value)
	{
		set(name, (sint64)value);
	}

	xml::Element* GetCurrentElement() const { return m_current_element; }

	XMLConfigParser get(const char* name) const
	{
		return {m_document, Base().child(name).element()};
	}

	XMLConfigParser get(const char* name, const XMLConfigParser& parser)
	{
		// the document itself has no siblings
		return { m_document, xml::Node(parser.m_current_element).next_sibling(name).element() };
	}

	XMLConfigParser set(const char* name) const
	{
		return {m_document, Base().append_child(name).element()};
	}

	template <typename T>
	XMLConfigParser& set_attribute(const char* name, const T& value)
	{
		cemu_assert_debug(m_current_element != nullptr);
		if (m_current_element)
		{
			xml::Node node(m_current_element);
			xml::Attribute a = node.attribute(name);
			if (!a)
				a = node.append_attribute(name);
			if constexpr (std::is_enum_v<T>)
				a.set_value(XMLConfigText::ToStr(static_cast<std::underlying_type_t<T>>(value)));
			else
				a.set_value(XMLConfigText::ToStr(value));
		}
		return *this;
	}

	template <typename T>
	XMLConfigParser& set_attribute(const char* name, const ConfigValue<T>& value)
	{
		return set_attribute(name, value.GetValue());
	}

	template <typename T>
	XMLConfigParser& set_attribute(const char* name, const ConfigValueBounds<T>& value)
	{
		return set_attribute(name, value.GetValue());
	}
	
	XMLConfigParser& set_attribute(const char* name, const std::string& value)
	{
		return set_attribute(name, value.c_str());
	}


	bool valid() const
	{
		if (m_is_root)
			return m_document != nullptr;

		return m_document != nullptr && m_current_element != nullptr;
	}

private:
	const char* Text() const { return m_current_element ? TextOf(xml::Node(m_current_element)) : nullptr; }

	xml::Document* m_document;
	xml::Element* m_current_element;
	bool m_is_root;
};

using ChildXMLConfigParser = std::pair<std::function<void(XMLConfigParser&)>, std::function<void(XMLConfigParser&)>>;

template<typename T>
concept XMLConfigurable = requires(T t, XMLConfigParser& configParser) {
	{ t.Save(configParser) } -> std::same_as<XMLConfigParser>;
	{ t.Load(configParser) } -> std::same_as<XMLConfigParser>;
};

template <XMLConfigurable T>
class XMLConfig
{
public:
	XMLConfig() = delete;

	XMLConfig(T& instance)
		: m_instance(instance)
	{}

	XMLConfig(T& instance, std::wstring_view filename)
		: m_instance(instance), m_filename(filename) {}

	XMLConfig(const XMLConfig&) = delete;

	virtual ~XMLConfig() = default;

	bool Load()
	{
		if (m_filename.empty())
			return false;

		return Load(m_filename);
	}

	bool Load(const std::wstring& filename)
	{
		FileStream* fs = FileStream::openFile(filename.c_str());
		if (!fs)
		{
			cemuLog_logDebug(LogType::Force, "XMLConfig::Load > failed \"{}\" with error {}", boost::nowide::narrow(filename), errno);
			return false;
		}
		std::vector<uint8> xmlData;
		xmlData.resize(fs->GetSize());
		fs->readData(xmlData.data(), xmlData.size());
		delete fs;

		xml::Document doc;
		if (!doc.load(xmlData.data(), xmlData.size()))
		{
			cemuLog_logDebug(LogType::Force, "XMLConfig::Load > not well-formed at offset {}", doc.error_offset());
			return false;
		}

		auto parser = XMLConfigParser(&doc);
		auto parentParser = m_instance.Load(parser);

		for (auto [save, load] : m_childConfigParsers)
			load(parentParser);

		return true;
	}

	bool Save()
	{
		if (m_filename.empty())
			return false;

		return Save(m_filename);
	}

	bool Save(const std::wstring& filename)
	{
		std::error_code err;
		fs::create_directories(fs::path(filename).parent_path(), err);
		if (err)
		{
			cemuLog_log(LogType::Force, "can't create parent path for save file: {}", err.message());
			return false;
		}

		xml::Document doc;

		auto parser = XMLConfigParser(&doc);
		auto parentParser = m_instance.Save(parser);

		for (auto [save, load] : m_childConfigParsers)
			save(parentParser);

		// through a file beside it that then takes its name
		const std::string out = doc.save("    ");
		if (out.empty() || !ReplaceFileContents(fs::path(filename), out.data(), out.size()))
		{
			cemuLog_log(LogType::Force, "Unable to save settings to file: {}", boost::nowide::narrow(filename));
			return false;
		}
		return true;
	}

	[[nodiscard]] const std::wstring& GetFilename() const { return m_filename; }
	void SetFilename(const std::wstring& filename) { m_filename = filename; }

	std::unique_lock<std::mutex> Lock() { return std::unique_lock(m_mutex); }

	void AddChildConfig(ChildXMLConfigParser childConfigParser)
	{
		m_childConfigParsers.push_back(childConfigParser);
	}

  private:
	std::vector<ChildXMLConfigParser> m_childConfigParsers;
	T& m_instance;
	std::wstring m_filename;
	std::mutex m_mutex;
};

template<typename T>
concept XMLConfigProvider = requires(T t, ChildXMLConfigParser& configParser) {
	{ t().Save() } -> std::same_as<bool>;
	{ t().Load() } -> std::same_as<bool>;
	{ t().Lock() } -> std::same_as<std::unique_lock<std::mutex>>;
	{ t().AddChildConfig(configParser) } -> std::same_as<void>;
};

template<typename T, void (T::*L)(XMLConfigParser&), void (T::*S)(XMLConfigParser&)>
class XMLChildConfig
{
  public:
	XMLChildConfig(XMLConfigProvider auto getParentConfig)
	{
		m_parentConfig = {
			.lock = [getParentConfig]() { return getParentConfig().Lock(); },
			.save = [getParentConfig]() { return getParentConfig().Save(); },
			.load = [getParentConfig]() { return getParentConfig().Load(); },
		};

		auto configParser = std::make_pair(
			[this](XMLConfigParser& parser) {
				(m_data.*S)(parser);
			},
			[this](XMLConfigParser& parser) {
				(m_data.*L)(parser);
			});

		getParentConfig().AddChildConfig(configParser);
	}

	bool Save()
	{
		return m_parentConfig.save();
	}

	bool Load()
	{
		return m_parentConfig.load();
	}

	T& Data()
	{
		return m_data;
	}

	std::unique_lock<std::mutex> Lock()
	{
		return m_parentConfig.lock();
	}

	virtual ~XMLChildConfig() = default;

  private:
	T m_data;
	struct
	{
		std::function<std::unique_lock<std::mutex>()> lock;
		std::function<bool()> save;
		std::function<bool()> load;
	} m_parentConfig;
};

template<typename T>
struct XMLDataConfig {};

template <XMLConfigurable T>
class XMLDataConfig<T> : public XMLConfig<T>
{
public:
	XMLDataConfig()
		: XMLConfig<T>::XMLConfig(m_data), m_data() {}

	XMLDataConfig(std::wstring_view filename)
		: XMLConfig<T>::XMLConfig(m_data, filename), m_data() {}

	XMLDataConfig(std::wstring_view filename, T init_data)
		: XMLConfig<T>::XMLConfig(m_data, filename), m_data(std::move(init_data)) {}

	XMLDataConfig(const XMLDataConfig& o) = delete;

	T& data() { return m_data; }

private:
	T m_data;
};

template<typename T>
concept XMLStandaloneConfigurable = requires(T t, XMLConfigParser& configParser) {
	{ t.Save(configParser) } -> std::same_as<void>;
	{ t.Load(configParser) } -> std::same_as<void>;
};

template<XMLStandaloneConfigurable T>
struct XMLConfigWrapper
{
	XMLConfigParser Save(XMLConfigParser& configParser)
	{
		data.Save(configParser);
		return configParser;
	}

	XMLConfigParser Load(XMLConfigParser& configParser)
	{
		data.Load(configParser);
		return configParser;
	}

	T data;
};

template<XMLStandaloneConfigurable T>
class XMLDataConfig<T> : public XMLConfig<XMLConfigWrapper<T>>
{
  public:
	XMLDataConfig()
		: XMLConfig<XMLConfigWrapper<T>>::XMLConfig(m_configWrapper), m_configWrapper() {}

	XMLDataConfig(std::wstring_view filename)
		: XMLConfig<XMLConfigWrapper<T>>::XMLConfig(m_configWrapper, filename), m_configWrapper() {}

	XMLDataConfig(std::wstring_view filename, T init_data)
		: XMLConfig<XMLConfigWrapper<T>>::XMLConfig(m_configWrapper, filename), m_configWrapper{.data = std::move(init_data)} {}

	XMLDataConfig(const XMLDataConfig& o) = delete;

	T& data()
	{
		return m_configWrapper.data;
	}

  private:
	XMLConfigWrapper<T> m_configWrapper;
};
