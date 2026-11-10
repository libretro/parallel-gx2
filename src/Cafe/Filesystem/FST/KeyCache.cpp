#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "util/helpers/RcuCell.h"
#include "Cafe/Filesystem/FST/key_cache.h"


// What keys.txt holds, read whole and swapped in whole: a search reads the
// set in place when it starts, with no lock, and a reload (each load, so an
// edit counts without a restart) builds a new one beside it. The reading,
// the key files and the test of a key are C (key_cache.c); this keeps the
// set and says what was tried.
struct KeySet
{
	key_set_t set{};
	bool prepared = false;

	KeySet() = default;
	KeySet(KeySet&& other) noexcept : set(other.set), prepared(other.prepared) { other.set = {}; }
	KeySet(const KeySet&) = delete;
	~KeySet() { key_set_free(&set); }
};

static RcuCell<KeySet> s_keySet;

void KeyCache_Prepare(bool reload)
{
	KeyCache_Load(ActiveSettings::GetUserDataPath("keys.txt"), reload);
}

void KeyCache_Load(const fs::path& keysPath, bool reload)
{
	if (!reload)
	{
		auto current = s_keySet.Read();
		if (current->prepared)
		{
			cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: already prepared (keys_loaded={})", (sint32)current->set.count);
			return;
		}
	}
	KeySet set;
	set.prepared = true;
	cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: keysPath='{}'", keysPath.generic_string());
	const std::string path = FileStream::HostPath(keysPath);
	unsigned lines = 0;
	const bool loaded = key_set_load(&set.set, path.c_str(), [](void*, unsigned line, int what) {
		if (what == KEY_CACHE_LINE_NOT_HEX)
		{
			cemuLog_log(LogType::Force, "[KeyCache] Invalid hex at line {}", line);
			WindowSystem::ShowErrorDialog(_tr("Error in keys.txt at line {}", line), WindowSystem::ErrorCategory::KEYS_TXT_CREATION);
		}
		else
			cemuLog_log(LogType::Force, "[KeyCache] Invalid key length at line {}", line);
	}, nullptr, &lines);
	if (!loaded && !FileStream::Exists(keysPath))
	{
		if (key_cache_write_template(path.c_str()))
			cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: created keys.txt (writing default template)");
		else
		{
			cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: keys.txt could not be created");
			WindowSystem::ShowErrorDialog(_tr("Unable to create file keys.txt\nThis can happen if Cemu does not have write permission to its own directory, the disk is full or if anti-virus software is blocking Cemu."), _tr("Error"), WindowSystem::ErrorCategory::KEYS_TXT_CREATION);
		}
	}
	else
		cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare done (lines_read={}, total_keys_loaded={})", lines, (sint32)set.set.count);
	// published however the reading ended
	s_keySet.Write([&] { return std::move(set); });
}

KeyCacheFile KeyCache_ReadKeyFile(const fs::path& keyPath, uint8* keyOut)
{
	switch (key_cache_read_key_file(FileStream::HostPath(keyPath).c_str(), keyOut))
	{
	case KEY_CACHE_FILE_KEY: return KeyCacheFile::KEY;
	case KEY_CACHE_FILE_NOT_A_KEY:
		cemuLog_log(LogType::Force, "[KeyCache] {} is not a disc key (16 bytes, or 32 hex digits)", _pathToUtf8(keyPath));
		return KeyCacheFile::NOT_A_KEY;
	default: return KeyCacheFile::ABSENT;
	}
}

bool KeyCache_FindDiscKey(const fs::path& image, const uint8* header48, uint8* keyOut, KeyCacheSearch* search)
{
	KeyCacheSearch local;
	KeyCacheSearch& s = search ? *search : local;
	s = {};
	// the image's own <name>.key first, then any other .key in its folder: a
	// dumper writes the disc's key as game.key beside game.wux. Tried for
	// this image only, not kept.
	uint8 beside[65][KEY_CACHE_KEY_SIZE];
	const size_t besideCount = key_cache_keys_beside(FileStream::HostPath(image).c_str(), beside, 65,
		[](void* ud, const char* name, int what) {
			KeyCacheSearch& s = *(KeyCacheSearch*)ud;
			if (what == KEY_CACHE_FILE_KEY)
				s.beside.emplace_back(name);
			else
			{
				s.notKeys.emplace_back(name);
				cemuLog_log(LogType::Force, "[KeyCache] {} is not a disc key (16 bytes, or 32 hex digits)", name);
			}
		}, &s);
	{
		// keys.txt as it is now: read in place, not copied
		auto set = s_keySet.Read();
		s.keysTxt = set->set.keys_txt;
		for (size_t i = 0; i < set->set.count; i++)
		{
			if (key_cache_fits(set->set.keys[i], header48))
			{
				memcpy(keyOut, set->set.keys[i], KEY_CACHE_KEY_SIZE);
				cemuLog_log(LogType::Force, "[KeyCache] disc key found, keys.txt's {}", i);
				return true;
			}
		}
	}
	for (size_t i = 0; i < besideCount; i++)
	{
		if (key_cache_fits(beside[i], header48))
		{
			memcpy(keyOut, beside[i], KEY_CACHE_KEY_SIZE);
			cemuLog_log(LogType::Force, "[KeyCache] disc key found, {} beside the image", s.beside[i]);
			return true;
		}
	}
	return false;
}

static std::string KeyCache_JoinNames(const std::vector<std::string>& names)
{
	std::string out;
	for (size_t i = 0; i < names.size(); i++)
	{
		if (i)
			out += i + 1 == names.size() ? " and " : ", ";
		out += names[i];
	}
	return out;
}

std::string KeyCache_DescribeMissing(const fs::path& image, const fs::path& keysPath, const KeyCacheSearch& search)
{
	const std::string imageName = _pathToUtf8(image.filename());
	const std::string keys = _pathToUtf8(keysPath);
	std::string out = fmt::format("No disc key fits {}: it is encrypted with its own, which the dumper wrote beside it as a .key file.", imageName);
	if (!search.notKeys.empty())
		out += fmt::format(" {} {} not a disc key (16 bytes, or 32 hex digits).", KeyCache_JoinNames(search.notKeys), search.notKeys.size() == 1 ? "is" : "are");
	if (!search.beside.empty())
		out += fmt::format(" {} beside it {} not its key.", KeyCache_JoinNames(search.beside), search.beside.size() == 1 ? "is" : "are");
	else if (search.notKeys.empty())
		out += " There is no .key file beside it.";
	if (search.keysTxt == 0)
		out += fmt::format(" {} has no keys.", keys);
	else
		out += fmt::format(" None of the {} key{} in {} fits.", search.keysTxt, search.keysTxt == 1 ? "" : "s", keys);
	out += fmt::format(" Put the disc's key beside the image (any name ending in .key), or add it to {}.", keys);
	return out;
}
