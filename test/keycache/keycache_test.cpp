// The core's disc key search (KeyCache.cpp), over a folder made here:
//  - with keys.txt missing it is created, with only the example key, which
//    does not count as a key; with no key file beside the image no key fits,
//    and the user is told there is none beside it and keys.txt has none;
//  - the dumper's game.key (16 bytes) beside game.wux - a name of its own -
//    is found, and so is <image>.key as 32 hex digits among spaces;
//  - a .key file that is no key is named as not one, and a key file that is
//    some other disc's is named as not this one's;
//  - keys.txt (CRLF lines, comments, dashes) is read once, and again on
//    reload: a key added after the first read counts at the next load, and
//    one taken out no longer does;
//  - keys.txt read again on one thread while others search: each search
//    finds the key or does not, as the set it read says, with no lock
//    (run under ThreadSanitizer by CI).
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "util/crypto/aes128.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

static void WriteFile(const fs::path& path, const void* data, size_t size)
{
	FILE* f = std::fopen(path.string().c_str(), "wb");
	CHECK(f != nullptr);
	if (!f)
		return;
	CHECK(std::fwrite(data, 1, size, f) == size);
	std::fclose(f);
}

static void WriteText(const fs::path& path, std::string_view text)
{
	WriteFile(path, text.data(), text.size());
}

static const uint8 KEY[16] = {0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
static const uint8 OTHER[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static bool Contains(const std::string& s, std::string_view part)
{
	return s.find(part) != std::string::npos;
}

int main()
{
	AES128_init();
	const fs::path dir = fs::temp_directory_path() / "parallel_gx2_keycache_test";
	fs::remove_all(dir);
	fs::create_directories(dir / "system" / "Cemu");
	fs::create_directories(dir / "games");
	ActiveSettings::s_userDataPath = dir / "system" / "Cemu";
	const fs::path keysTxt = dir / "system" / "Cemu" / "keys.txt";
	const fs::path image = dir / "games" / "Zelda (USA) (En,Fr,Es).wux";
	WriteText(image, "not looked at");

	// a disc's header: an IV, then two blocks of zeros encrypted with KEY
	uint8 header[48]{};
	for (int i = 0; i < 16; i++)
		header[i] = (uint8)(i * 7 + 1);
	{
		uint8 zeros[32]{};
		AES128_CBC_encrypt(header + 16, zeros, 32, KEY, header);
	}
	uint8 key[16];
	KeyCacheSearch search;

	// nothing anywhere: keys.txt made, with the example only
	KeyCache_Prepare();
	CHECK(fs::exists(keysTxt));
	CHECK(!KeyCache_FindDiscKey(image, header, key, &search));
	CHECK(search.keysTxt == 0 && search.beside.empty() && search.notKeys.empty());
	std::string why = KeyCache_DescribeMissing(image, keysTxt, search);
	CHECK(Contains(why, "Zelda (USA) (En,Fr,Es).wux") && Contains(why, "no .key file beside it") && Contains(why, "has no keys"));

	// the dumper's game.key, 16 bytes, under its own name
	WriteFile(dir / "games" / "game.key", KEY, 16);
	memset(key, 0, 16);
	CHECK(KeyCache_FindDiscKey(image, header, key, &search) && memcmp(key, KEY, 16) == 0);
	CHECK(search.beside.size() == 1 && search.beside[0] == "game.key");
	fs::remove(dir / "games" / "game.key");

	// the cache forgets it on reload; then <image>.key, as hex among spaces
	KeyCache_Prepare(true);
	CHECK(!KeyCache_FindDiscKey(image, header, key, &search));
	WriteText(dir / "games" / "Zelda (USA) (En,Fr,Es).key", " 0F1E2D3C 4b5a6978\n8796A5B4c3d2e1f0 \r\n");
	memset(key, 0, 16);
	CHECK(KeyCache_FindDiscKey(image, header, key, &search) && memcmp(key, KEY, 16) == 0);
	fs::remove(dir / "games" / "Zelda (USA) (En,Fr,Es).key");
	KeyCache_Prepare(true);

	// no key, and some other disc's
	WriteText(dir / "games" / "notes.KEY", "this is not a key at all, it is far too long to be one");
	WriteFile(dir / "games" / "other.key", OTHER, 16);
	CHECK(!KeyCache_FindDiscKey(image, header, key, &search));
	CHECK(search.notKeys.size() == 1 && search.notKeys[0] == "notes.KEY");
	CHECK(search.beside.size() == 1 && search.beside[0] == "other.key");
	why = KeyCache_DescribeMissing(image, keysTxt, search);
	CHECK(Contains(why, "notes.KEY is not a disc key") && Contains(why, "other.key beside it is not its key"));
	fs::remove(dir / "games" / "notes.KEY");
	fs::remove(dir / "games" / "other.key");
	// a directory named like a key file is not one
	fs::create_directories(dir / "games" / "keys.key");
	CHECK(!KeyCache_FindDiscKey(image, header, key, &search) && search.notKeys.empty() && search.beside.empty());

	// keys.txt edited after it was read: counted at the next load, not before
	WriteText(keysTxt, "# keys\r\n541b9889519b27d363cd21604b97c67a # example\r\n"
		"01020304-05060708-090a0b0c-0d0e0f10\r\n0f1e2d3c4b5a6978_8796a5b4c3d2e1f0 ; botw\r\n");
	KeyCache_Prepare();
	CHECK(!KeyCache_FindDiscKey(image, header, key, &search));
	KeyCache_Prepare(true);
	memset(key, 0, 16);
	CHECK(KeyCache_FindDiscKey(image, header, key, &search) && memcmp(key, KEY, 16) == 0);
	CHECK(search.keysTxt == 2);
	// and taken out again
	WriteText(keysTxt, "01020304050607080910111213141516\r\n");
	KeyCache_Prepare(true);
	CHECK(!KeyCache_FindDiscKey(image, header, key, &search) && search.keysTxt == 1);
	why = KeyCache_DescribeMissing(image, keysTxt, search);
	CHECK(Contains(why, "None of the 1 key in"));

	// searches on three threads while keys.txt is read again, with the key and
	// without it by turns
	{
		const std::string withKey = "0f1e2d3c4b5a69788796a5b4c3d2e1f0\r\n";
		const std::string without = "01020304050607080910111213141516\r\n";
		std::atomic<bool> stop{false};
		std::atomic<int> found{0}, notFound{0}, bad{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 3; t++)
		{
			threads.emplace_back([&]() {
				while (!stop.load(std::memory_order_relaxed))
				{
					uint8 k[16];
					KeyCacheSearch se;
					if (KeyCache_FindDiscKey(image, header, k, &se))
					{
						found++;
						if (memcmp(k, KEY, 16) != 0 || se.keysTxt != 1)
							bad++;
					}
					else
					{
						notFound++;
						if (se.keysTxt != 1)
							bad++;
					}
				}
			});
		}
		for (int i = 0; i < 200 || found == 0 || notFound == 0; i++)
		{
			WriteText(keysTxt, (i & 1) ? withKey : without);
			KeyCache_Prepare(true);
			if (i > 20000)
				break;
		}
		stop = true;
		for (std::thread& t : threads)
			t.join();
		CHECK(bad == 0 && found > 0 && notFound > 0);
	}

	fs::remove_all(dir);
	if (s_failures)
	{
		std::printf("keycache_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("keycache_test: all passed\n");
	return 0;
}
