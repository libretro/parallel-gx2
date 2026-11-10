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
//  - a directory listed through both mounts has every name once, with the
//    sizes and types the host's directory read gave.
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/RPXHash.h"
#include "Common/FileStream.h"
#include "Cemu/Logging/CemuLogging.h"

#include <cstdio>
#include <map>
#include <random>

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

	fsc_unmountAll();
	FileStream::RemoveAll(dir);
	if (s_failures)
	{
		std::printf("fsc_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("fsc_test: all passed\n");
	return 0;
}
