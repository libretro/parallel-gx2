#include "util/helpers/MappedFile.h"
#include <fstream>

#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"
#include "config/ActiveSettings.h"
#include "util/crypto/aes128.h"
#include "Common/FileStream.h"
#include "util/helpers/StringHelpers.h"
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "util/helpers/RcuCell.h"

#include <retro_dirent.h>

// What keys.txt holds, read whole and swapped in whole: a search reads the
// set in place when it starts, with no lock, and a reload (each load, so an
// edit counts without a restart) builds a new one beside it.
struct KeySet
{
	std::vector<std::array<uint8, 16>> keys;
	size_t keysTxt = 0; // the example the file is created with not counted
	bool prepared = false;
};

static RcuCell<KeySet> s_keySet;

bool strishex(std::string_view str)
{
	for(size_t i=0; i<str.size(); i++)
	{
		char c = str[i];
		if( (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') )
			continue;
		return false;
	}
	return true;
}


static const uint8 sExampleKey[16] = {0x54, 0x1b, 0x98, 0x89, 0x51, 0x9b, 0x27, 0xd3, 0x63, 0xcd, 0x21, 0x60, 0x4b, 0x97, 0xc6, 0x7a};

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
			cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: already prepared (keys_loaded={})", (sint32)current->keys.size());
			return;
		}
	}
	KeySet set;
	set.prepared = true;
	// published however the reading ends
	struct Publish
	{
		KeySet& set;
		~Publish() { s_keySet.Write([this] { return std::move(set); }); }
	} publish{set};
	// load keys
	cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: keysPath='{}'", keysPath.generic_string());
	FileStream* fs_keys = FileStream::openFile2(keysPath);
	if( !fs_keys )
	{
		cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: openFile2 failed, attempting createFile2");
		fs_keys = FileStream::createFile2(keysPath);
		if(fs_keys)
		{
			cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: created keys.txt (writing default template)");
			fs_keys->writeString("# this file contains keys needed for decryption of disc file system data (WUD/WUX)\r\n");
			fs_keys->writeString("# 1 key per line, any text after a '#' character is considered a comment\r\n");
			fs_keys->writeString("# the emulator will automatically pick the right key\r\n");
			fs_keys->writeString("541b9889519b27d363cd21604b97c67a # example key (can be deleted)\r\n");
			delete fs_keys;
		}
		else
		{
			cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: createFile2 failed");
			WindowSystem::ShowErrorDialog(_tr("Unable to create file keys.txt\nThis can happen if Cemu does not have write permission to its own directory, the disk is full or if anti-virus software is blocking Cemu."), _tr("Error"), WindowSystem::ErrorCategory::KEYS_TXT_CREATION);
		}
		return;
	}
	cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare: opened keys.txt successfully");
	sint32 lineNumber = 0;
	std::string line;
	while( fs_keys->readLine(line) )
	{
		lineNumber++;
		if (lineNumber <= 10 || (lineNumber % 200) == 0)
			cemuLog_log(LogType::Force, "[KeyCache] Read line {} (raw_len={})", lineNumber, (sint32)line.size());
		// truncate anything after '#' or ';'
		for(size_t i=0; i<line.size(); i++)
		{
			if(line[i] == '#' || line[i] == ';' )
			{
				line.resize(i);
				break;
			}
		}
		// remove whitespaces and other common formatting characters
		auto itr = line.begin();
		while (itr != line.end())
		{
			char c = *itr;
			if (c == ' ' || c == '\t' || c == '-' || c == '_')
				itr = line.erase(itr);
			else
				itr++;
		}
		if (line.empty())
			continue;
		if( strishex(line) == false )
		{
			cemuLog_log(LogType::Force, "[KeyCache] Invalid hex at line {} (len={})", lineNumber, (sint32)line.size());
			auto errorMsg = _tr("Error in keys.txt at line {}", lineNumber);
			WindowSystem::ShowErrorDialog(errorMsg, WindowSystem::ErrorCategory::KEYS_TXT_CREATION);
			continue;
		}
		if(line.size() == 32 )
		{
			// 128-bit key
			uint8 keyData128[16];
			StringHelpers::ParseHexString(line, keyData128, 16);
			std::array<uint8, 16>& key = set.keys.emplace_back();
			memcpy(key.data(), keyData128, 16);
			if (memcmp(keyData128, sExampleKey, 16) != 0)
				set.keysTxt++;
			if ((set.keys.size() <= 10) || ((set.keys.size() % 500) == 0))
				cemuLog_log(LogType::Force, "[KeyCache] Parsed AES-128 key (line={}, total_keys_loaded={})", lineNumber, (sint32)set.keys.size());
		}
		else
		{
			cemuLog_log(LogType::Force, "[KeyCache] Invalid key length at line {} (len={})", lineNumber, (sint32)line.size());
			// invalid key length
		}
	}
	delete fs_keys;
	cemuLog_log(LogType::Force, "[KeyCache] KeyCache_Prepare done (lines_read={}, total_keys_loaded={})", lineNumber, (sint32)set.keys.size());
}

KeyCacheFile KeyCache_ReadKeyFile(const fs::path& keyPath, uint8* keyOut)
{
	if (!FileStream::IsRegularFile(keyPath))
		return KeyCacheFile::ABSENT;
	// read where it lies
	MappedFile file(keyPath);
	const std::string_view data((const char*)file.data(), file.size());
	uint8 key[16];
	if (data.size() == 16)
		memcpy(key, data.data(), 16);
	else
	{
		char hex[32];
		size_t n = 0;
		for (char c : data)
		{
			if (isspace((unsigned char)c))
				continue;
			if (n == sizeof(hex))
			{
				n++;
				break;
			}
			hex[n++] = c;
		}
		if (n != sizeof(hex) || !strishex(std::string_view(hex, n)))
		{
			cemuLog_log(LogType::Force, "[KeyCache] {} is not a disc key (16 bytes, or 32 hex digits)", _pathToUtf8(keyPath));
			return KeyCacheFile::NOT_A_KEY;
		}
		StringHelpers::ParseHexString(std::string_view(hex, n), key, 16);
	}
	memcpy(keyOut, key, 16);
	return KeyCacheFile::KEY;
}

static bool KeyCache_IsKeyFileName(std::string_view name)
{
	return name.size() > 4 && boost::iequals(name.substr(name.size() - 4), ".key");
}

bool KeyCache_FindDiscKey(const fs::path& image, const uint8* header48, uint8* keyOut, KeyCacheSearch* search)
{
	KeyCacheSearch local;
	KeyCacheSearch& s = search ? *search : local;
	s = {};
	// the image's own <name>.key first, then any other .key in its folder: a
	// dumper writes the disc's key as game.key beside game.wux. Tried for
	// this image only, not kept.
	std::vector<std::array<uint8, 16>> beside;
	auto add = [&](const fs::path& keyPath) {
		std::array<uint8, 16> key;
		switch (KeyCache_ReadKeyFile(keyPath, key.data()))
		{
		case KeyCacheFile::KEY:
			beside.push_back(key);
			s.beside.push_back(_pathToUtf8(keyPath.filename()));
			break;
		case KeyCacheFile::NOT_A_KEY: s.notKeys.push_back(_pathToUtf8(keyPath.filename())); break;
		case KeyCacheFile::ABSENT: break;
		}
	};
	const fs::path own = fs::path(image).replace_extension(".key");
	add(own);
	fs::path folder = image.parent_path();
	if (folder.empty())
		folder = ".";
	if (struct RDIR* dir = retro_opendir_include_hidden(_pathToUtf8(folder).c_str(), true))
	{
		// a folder of many: the first few dozen key files are plenty
		for (int tried = 0; tried < 64 && retro_readdir(dir);)
		{
			const char* name = retro_dirent_get_name(dir);
			if (!name || !KeyCache_IsKeyFileName(name))
				continue;
			const fs::path keyPath = folder / _utf8ToPath(name);
			if (keyPath.filename() == own.filename())
				continue;
			tried++;
			add(keyPath);
		}
		retro_closedir(dir);
	}

	auto fits = [&](const std::array<uint8, 16>& key) {
		uint8 decrypted[32];
		AES128_CBC_decrypt(decrypted, header48 + 16, sizeof(decrypted), key.data(), header48);
		if (!std::all_of(decrypted, decrypted + sizeof(decrypted), [](uint8 v) { return v == 0; }))
			return false;
		memcpy(keyOut, key.data(), 16);
		return true;
	};
	{
		// keys.txt as it is now: read in place, not copied
		auto set = s_keySet.Read();
		s.keysTxt = set->keysTxt;
		for (size_t i = 0; i < set->keys.size(); i++)
		{
			if (fits(set->keys[i]))
			{
				cemuLog_log(LogType::Force, "[KeyCache] disc key found, keys.txt's {}", i);
				return true;
			}
		}
	}
	for (size_t i = 0; i < beside.size(); i++)
	{
		if (fits(beside[i]))
		{
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
