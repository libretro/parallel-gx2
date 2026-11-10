// The core's virtual file system over two host folders mounted at one path,
// one at the base priority and one at the patch priority, as a title and its
// update are:
//  - fsc_getFilePriority names the mount a file is opened from: the patch
//    for a file in both and for one only it has, the base for one only the
//    base has, none for a directory or what is not there, and the base when
//    asked no higher;
//  - fsc_readFileInChunks hands over a file whole through a buffer smaller
//    than it, in order, and the RPX hash taken over the pieces is the hash
//    the whole file had as CafeSystem took it; it says no for what is not
//    there;
//  - fsc_readFileInto reads up to what fits, at the priority asked, and says
//    -1 for what is not there;
//  - the mount table read without a lock: threads open, read, look up and
//    list while another unmounts and mounts the update over and over; each
//    sees the update or not, never half of it, and every table replaced is
//    freed once nothing reads it;
//  - a directory listed through both mounts has every name once, with the
//    sizes and types the host's directory read gave;
//  - a homebrew bundle (.wuhb) mounted: its directories list their
//    directories and files with their sizes - a name too long for the
//    entry cut and ended - and its files read back, mapped and not;
//  - a Wii U archive (.wua) mounted the same way (fscDeviceWua.cpp,
//    zarchive.c): listed a name at a time through the device, with the
//    sizes the archive has, and its files read back, mapped and not; a
//    file opened as a directory and the other way round are not found.
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/RPXHash.h"
#include "Cafe/Filesystem/WUHB/wuhb.h"
#include "Cafe/Filesystem/zarchive.h"
#include <formats/zar.h>
#include "../disc_image/romfs_image.h"
#include "Common/FileStream.h"
#include "Cemu/Logging/CemuLogging.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

// The hash as CafeSystem took it over the whole file, before it could be
// taken in pieces: what graphic packs and log.txt know a title's RPX by
static uint32 WholeFileRpxHash(const std::vector<uint8>& data)
{
	uint32 h = 0x3416DCBF;
	for (size_t i = 0; i < data.size(); i++)
	{
		uint32 c = data[i];
		h = (h << 3) | (h >> 29);
		h += c;
	}
	return h;
}

static std::vector<uint8> Bytes(size_t n, uint32 seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8> v(n);
	for (uint8& b : v)
		b = (uint8)rng();
	return v;
}

int main()
{
	const fs::path dir = fs::temp_directory_path() / "parallel_gx2_fsc_test";
	FileStream::RemoveAll(dir);
	const fs::path base = dir / "base", patch = dir / "patch";
	CHECK(FileStream::CreateDirectories(base / "code"));
	CHECK(FileStream::CreateDirectories(patch / "code" / "sub"));
	const std::vector<uint8> baseRpx = Bytes(700001, 1), patchRpx = Bytes(912345, 2), onlyBase = Bytes(4097, 3);
	CHECK(FileStream::WriteFile(base / "code" / "game.rpx", baseRpx.data(), baseRpx.size()));
	CHECK(FileStream::WriteFile(patch / "code" / "game.rpx", patchRpx.data(), patchRpx.size()));
	CHECK(FileStream::WriteFile(base / "code" / "base.bin", onlyBase.data(), onlyBase.size()));
	CHECK(FileStream::WriteFile(patch / "code" / "patch.bin", "p", 1));

	fsc_init();
	CHECK(FSCDeviceHostFS_Mount("/t/", _pathToUtf8(base) + "/", FSC_PRIORITY_BASE));
	CHECK(FSCDeviceHostFS_Mount("/t/", _pathToUtf8(patch) + "/", FSC_PRIORITY_PATCH));

	CHECK(fsc_getFilePriority("/t/code/game.rpx") == FSC_PRIORITY_PATCH);
	CHECK(fsc_getFilePriority("/t/code/game.rpx", FSC_PRIORITY_BASE) == FSC_PRIORITY_BASE);
	CHECK(fsc_getFilePriority("/t/code/base.bin") == FSC_PRIORITY_BASE);
	CHECK(fsc_getFilePriority("/t/code/patch.bin") == FSC_PRIORITY_PATCH);
	CHECK(fsc_getFilePriority("/t/code/patch.bin", FSC_PRIORITY_AOC) == -1);
	CHECK(fsc_getFilePriority("/t/code/none.bin") == -1);
	CHECK(fsc_getFilePriority("/t/code/sub") == -1);

	// read in pieces, each smaller than the file, the hash over them
	for (const auto& [prio, want] : {std::pair<sint32, const std::vector<uint8>*>{FSC_PRIORITY_MAX, &patchRpx}, {FSC_PRIORITY_BASE, &baseRpx}})
	{
		uint8 buf[65536 + 7];
		std::vector<uint8> got;
		uint32 h = RPXHASH_SEED;
		int pieces = 0;
		CHECK(fsc_readFileInChunks("/t/code/game.rpx", prio, buf, sizeof(buf), [&](const uint8* data, uint32 size) {
			got.insert(got.end(), data, data + size);
			h = RPXHash_Update(h, data, size);
			pieces++;
		}));
		CHECK(got == *want);
		CHECK(pieces == (int)((want->size() + sizeof(buf) - 1) / sizeof(buf)));
		CHECK(h == WholeFileRpxHash(*want));
	}
	uint8 small[16];
	CHECK(!fsc_readFileInChunks("/t/code/none.bin", FSC_PRIORITY_MAX, small, sizeof(small), [](const uint8*, uint32) {}));

	// read straight into a buffer, as much as fits
	uint8 into[5000];
	memset(into, 0xEE, sizeof(into));
	CHECK(fsc_readFileInto("/t/code/base.bin", into, 100) == 100 && memcmp(into, onlyBase.data(), 100) == 0 && into[100] == 0xEE);
	CHECK(fsc_readFileInto("/t/code/base.bin", into, sizeof(into)) == (sint64)onlyBase.size() && memcmp(into, onlyBase.data(), onlyBase.size()) == 0);
	CHECK(fsc_readFileInto("/t/code/game.rpx", into, 64, FSC_PRIORITY_BASE) == 64 && memcmp(into, baseRpx.data(), 64) == 0);
	CHECK(fsc_readFileInto("/t/code/none.bin", into, 1) == -1);

	// both mounts' entries, each name once
	sint32 status;
	FSCVirtualFile* d = fsc_openDirIterator("/t/code", &status);
	CHECK(d != nullptr);
	std::map<std::string, std::pair<bool, uint64>> names;
	FSCDirEntry e;
	int count = 0;
	while (d && fsc_nextDir(d, &e))
	{
		count++;
		names[e.path] = {e.isDirectory, e.isDirectory ? 0 : e.fileSize};
	}
	if (d)
		fsc_close(d);
	CHECK(count == 4 && names.size() == 4);
	CHECK(names["game.rpx"] == std::make_pair(false, (uint64)patchRpx.size()));
	CHECK(names["base.bin"] == std::make_pair(false, (uint64)onlyBase.size()));
	CHECK(names["patch.bin"] == std::make_pair(false, (uint64)1));
	CHECK(names["sub"].first);

	// the update unmounted and mounted again while threads read
	{
		std::atomic<bool> stop{false};
		std::atomic<int> bad{0}, sawBase{0}, sawPatch{0};
		std::vector<std::thread> readers;
		for (int t = 0; t < 4; t++)
		{
			readers.emplace_back([&, t]() {
				std::vector<uint8> buf(patchRpx.size() + 16);
				while (!stop.load(std::memory_order_relaxed))
				{
					sint32 st;
					FSCVirtualFile* f = fsc_open("/t/code/game.rpx", FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &st);
					if (!f)
					{
						bad++;
						continue;
					}
					const uint32 size = fsc_getFileSize(f);
					const uint32 got = fsc_readFile(f, buf.data(), (uint32)buf.size());
					fsc_close(f);
					if (size == patchRpx.size() && got == size && memcmp(buf.data(), patchRpx.data(), size) == 0)
						sawPatch++;
					else if (size == baseRpx.size() && got == size && memcmp(buf.data(), baseRpx.data(), size) == 0)
						sawBase++;
					else
						bad++;
					const sint32 prio = fsc_getFilePriority("/t/code/patch.bin");
					if (prio != FSC_PRIORITY_PATCH && prio != -1)
						bad++;
					if (t == 0)
					{
						FSCVirtualFile* d = fsc_openDirIterator("/t/code", &st);
						FSCDirEntry e;
						int n = 0;
						while (d && fsc_nextDir(d, &e))
							n++;
						if (d)
							fsc_close(d);
						if (n != 2 && n != 4) // patch.bin and sub there or not
							bad++;
					}
				}
			});
		}
		for (int i = 0; i < 300; i++)
		{
			CHECK(fsc_unmount("/t/", FSC_PRIORITY_PATCH));
			std::this_thread::yield();
			CHECK(FSCDeviceHostFS_Mount("/t/", _pathToUtf8(patch) + "/", FSC_PRIORITY_PATCH));
			std::this_thread::yield();
		}
		// both seen, at least once: keep reading until they have been
		for (int i = 0; i < 2000 && (sawBase == 0 || sawPatch == 0); i++)
		{
			fsc_unmount("/t/", FSC_PRIORITY_PATCH);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			FSCDeviceHostFS_Mount("/t/", _pathToUtf8(patch) + "/", FSC_PRIORITY_PATCH);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		stop = true;
		for (std::thread& r : readers)
			r.join();
		CHECK(bad == 0);
		CHECK(sawBase > 0 && sawPatch > 0);
		// nothing reads now: the next change frees every table but its own
		CHECK(fsc_unmount("/t/", FSC_PRIORITY_PATCH));
		CHECK(fsc_liveMountTables() == 1);
	}

	// a homebrew bundle, mounted as a title is
	{
		std::vector<RomfsFile> files;
		files.push_back({"meta", "meta.xml", Bytes(300, 9)});
		for (int i = 0; i < 9; i++)
			files.push_back({"content", fmt::format("file{}.bin", i), Bytes(1000 + i * 777, 10 + i)});
		files.push_back({"content", std::string(300, 'L'), Bytes(5, 30)});
		files.push_back({"content/sub", "deep.dat", Bytes(4321, 31)});
		const fs::path bundle = dir / "test.wuhb";
		const std::vector<uint8> image = MakeWuhb(files);
		CHECK(FileStream::WriteFile(bundle, image.data(), image.size()));
		for (unsigned flags : {0u, (unsigned)WUHB_OPEN_NO_MAP})
		{
			wuhb_t* reader = wuhb_open(_pathToUtf8(bundle).c_str(), flags);
			CHECK(reader != nullptr);
			if (!reader)
				continue;
			CHECK(FSCDeviceWUHB_Mount("/w/", "", reader, FSC_PRIORITY_BASE));
			sint32 st;
			FSCVirtualFile* listing = fsc_openDirIterator("/w/content", &st);
			CHECK(listing != nullptr);
			std::map<std::string, std::pair<bool, uint64>> seen;
			FSCDirEntry entry;
			while (listing && fsc_nextDir(listing, &entry))
				seen[std::string(entry.GetPath())] = {entry.isDirectory, entry.fileSize};
			if (listing)
				fsc_close(listing);
			CHECK(seen.size() == 11);
			CHECK(seen["sub"].first);
			CHECK(seen["file3.bin"] == std::make_pair(false, (uint64)(1000 + 3 * 777)));
			CHECK(seen.count(std::string(FSC_MAX_DIR_NAME_LENGTH - 1, 'L')) == 1);
			for (const RomfsFile& f : files)
			{
				if (f.name.size() > 100)
					continue;
				std::vector<uint8> got(f.data.size() + 8, 0xEE);
				const std::string p = "/w/" + f.dir + "/" + f.name;
				CHECK(fsc_readFileInto(p.c_str(), got.data(), (uint32)got.size()) == (sint64)f.data.size()
					&& memcmp(got.data(), f.data.data(), f.data.size()) == 0);
			}
			uint8 one[1];
			CHECK(fsc_readFileInto("/w/content/none.bin", one, 1) == -1);
			CHECK(fsc_unmount("/w/", FSC_PRIORITY_BASE));
			wuhb_close(reader);
		}
	}

	// a Wii U archive, mounted as a title in one is
	{
		struct Entry { std::string path; std::vector<uint8> data; };
		std::vector<Entry> entries = {
			{"0005000010101a00_v0/meta/meta.xml", Bytes(500, 40)},
			{"0005000010101a00_v0/content/a.bin", Bytes(70000, 41)},
			{"0005000010101a00_v0/content/b.bin", Bytes(3, 42)},
			{"0005000010101a00_v0/content/empty.bin", {}},
			{"0005000010101a00_v0/content/sub/deep.bin", Bytes(123457, 43)},
		};
		const fs::path archive = dir / "test.wua";
		FILE* out = std::fopen(_pathToUtf8(archive).c_str(), "wb");
		CHECK(out != nullptr);
		zar_writer_t* w = zar_writer_new([](void* ud, const void* data, size_t len) {
			return (int)(std::fwrite(data, 1, len, (FILE*)ud) == len);
		}, out, 3);
		for (const Entry& e : entries)
		{
			CHECK(zar_writer_file(w, e.path.c_str()) == ZAR_OK);
			CHECK(zar_writer_append(w, e.data.data(), e.data.size()) == ZAR_OK);
		}
		CHECK(zar_writer_finish(w) == ZAR_OK);
		zar_writer_free(w);
		std::fclose(out);
		for (unsigned flags : {0u, (unsigned)ZARCHIVE_OPEN_NO_MAP})
		{
			zarchive_t* zar = zarchive_open(_pathToUtf8(archive).c_str(), flags, nullptr, 0);
			CHECK(zar != nullptr);
			if (!zar)
				continue;
			CHECK(FSCDeviceWUA_Mount("/z/", "0005000010101a00_v0/", zar, FSC_PRIORITY_BASE));
			sint32 st;
			FSCVirtualFile* listing = fsc_openDirIterator("/z/content", &st);
			CHECK(listing != nullptr);
			std::map<std::string, std::pair<bool, uint64>> seen;
			FSCDirEntry entry;
			while (listing && fsc_nextDir(listing, &entry))
				seen[std::string(entry.GetPath())] = {entry.isDirectory, entry.fileSize};
			if (listing)
				fsc_close(listing);
			CHECK(seen.size() == 4);
			CHECK(seen["sub"].first);
			CHECK(seen["a.bin"] == std::make_pair(false, (uint64)70000));
			CHECK(seen["empty.bin"] == std::make_pair(false, (uint64)0));
			for (const Entry& e : entries)
			{
				std::vector<uint8> got(e.data.size() + 8, 0xEE);
				const std::string p = "/z/" + e.path.substr(e.path.find('/') + 1);
				CHECK(fsc_readFileInto(p.c_str(), got.data(), (uint32)got.size()) == (sint64)e.data.size()
					&& (e.data.empty() || memcmp(got.data(), e.data.data(), e.data.size()) == 0));
			}
			// a directory is no file, and a file no directory
			CHECK(fsc_open("/z/content/sub", FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &st) == nullptr);
			CHECK(fsc_openDirIterator("/z/content/a.bin", &st) == nullptr);
			CHECK(fsc_unmount("/z/", FSC_PRIORITY_BASE));
			zarchive_close(zar);
		}
	}

	fsc_unmountAll();
	CHECK(fsc_liveMountTables() == 1);
	FileStream::RemoveAll(dir);
	if (s_failures)
	{
		std::printf("fsc_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("fsc_test: all passed\n");
	return 0;
}
