#pragma once

// Reads keys.txt (in the user data folder) once, or again with reload: an
// edit takes effect at the next load, with no restart.
void KeyCache_Prepare(bool reload = false);
// The same, from keysPath
void KeyCache_Load(const fs::path& keysPath, bool reload);

uint8* KeyCache_GetAES128(sint32 index);

// A disc key kept in a file: 16 bytes, or 32 hex digits as text. Added to the
// key cache (once), so the image is found without its key in keys.txt.
enum class KeyCacheFile
{
	ABSENT,
	KEY,
	NOT_A_KEY,
};
KeyCacheFile KeyCache_AddKeyFile(const fs::path& keyPath);

// What a disc key was looked for in
struct KeyCacheSearch
{
	size_t keysTxt = 0;                // keys from keys.txt, the example not counted
	std::vector<std::string> beside;   // .key files beside the image, tried
	std::vector<std::string> notKeys;  // .key files beside it that are no key
};

// The key that decrypts a disc's header (the 48 octets at 0x18100, zeros
// once decrypted) into keyOut: from keys.txt, the image's own <name>.key, or
// any other .key in its folder (the game.key a dumper writes)
bool KeyCache_FindDiscKey(const fs::path& image, const uint8* header48, uint8* keyOut, KeyCacheSearch* search = nullptr);

// What the user is told when none fits: what was tried, and where a key goes
std::string KeyCache_DescribeMissing(const fs::path& image, const fs::path& keysPath, const KeyCacheSearch& search);
