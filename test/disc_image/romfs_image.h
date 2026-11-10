#pragma once
// A homebrew bundle (.wuhb, a RomFS image) made from a list of files, for the
// tests of the core's reader (disc_image) and of the file system device over
// it (fsc): directories "", content, content/sub and meta; hash chains of
// three buckets, so names collide; each directory's lists of directories and
// files filled in. loop makes a chain come back round on itself.
#include <string>
#include <string_view>
#include <vector>

static inline void PutBE(std::vector<uint8>& v, size_t at, uint64 x, int bytes)
{
	for (int i = 0; i < bytes; i++)
		v[at + i] = (uint8)(x >> (8 * (bytes - 1 - i)));
}

static inline uint32 RomfsHash(uint32 parent, std::string_view name)
{
	uint32 hash = parent ^ 123456789;
	for (char ch : name)
	{
		unsigned char c = (unsigned char)ch;
		if (c >= 'a' && c <= 'z')
			c = c + 'A' - 'a';
		hash = (hash >> 5) | (hash << 27);
		hash ^= c;
	}
	return hash;
}

struct RomfsFile
{
	std::string dir;  // "" for the root, else "a/b"
	std::string name;
	std::vector<uint8> data;
};

// A RomFS image as a .wuhb holds one: the root, content, content/sub and
// meta; hash tables of three buckets, so that chains collide. With loop, the
// last entry of every file chain points back at the chain's first.
static inline std::vector<uint8> MakeWuhb(const std::vector<RomfsFile>& files, bool loop = false)
{
	struct Dir { std::string path, name; uint32 parent; uint32 offset; uint32 hash = 0xFFFFFFFF; };
	std::vector<Dir> dirs = {{"", "", 0}, {"content", "content", 0}, {"content/sub", "sub", 1}, {"meta", "meta", 0}};
	const uint32 BUCKETS = 3;
	auto padded = [](size_t n) { return (n + 3) & ~(size_t)3; };
	uint32 at = 0;
	for (Dir& d : dirs)
	{
		d.offset = at;
		at += (uint32)(24 + padded(d.name.size()));
	}
	const uint32 dirTableSize = at;
	auto dirIndex = [&](const std::string& path) {
		for (size_t i = 0; i < dirs.size(); i++)
			if (dirs[i].path == path)
				return (uint32)i;
		return 0u;
	};
	std::vector<uint32> fileOffset(files.size()), fileData(files.size());
	at = 0;
	uint32 dataAt = 0;
	for (size_t i = 0; i < files.size(); i++)
	{
		fileOffset[i] = at;
		at += (uint32)(32 + padded(files[i].name.size()));
		fileData[i] = dataAt;
		dataAt += (uint32)((files[i].data.size() + 31) & ~(size_t)31);
	}
	const uint32 fileTableSize = at;
	// buckets: heads, and each entry's next in its chain
	std::vector<uint32> dirBucket(BUCKETS, 0xFFFFFFFF), fileBucket(BUCKETS, 0xFFFFFFFF);
	std::vector<uint32> fileNext(files.size(), 0xFFFFFFFF);
	for (size_t i = dirs.size(); i-- > 0;)
	{
		const uint32 b = RomfsHash(dirs[dirs[i].parent].offset, dirs[i].name) % BUCKETS;
		dirs[i].hash = dirBucket[b];
		dirBucket[b] = dirs[i].offset;
	}
	for (size_t i = files.size(); i-- > 0;)
	{
		const uint32 b = RomfsHash(dirs[dirIndex(files[i].dir)].offset, files[i].name) % BUCKETS;
		fileNext[i] = fileBucket[b];
		fileBucket[b] = fileOffset[i];
	}
	if (loop)
	{
		for (size_t i = 0; i < files.size(); i++)
			if (fileNext[i] == 0xFFFFFFFF)
			{
				const uint32 b = RomfsHash(dirs[dirIndex(files[i].dir)].offset, files[i].name) % BUCKETS;
				fileNext[i] = fileBucket[b];
			}
	}

	const size_t header = 0x50;
	const size_t dirHash = header, dirTable = dirHash + BUCKETS * 4;
	const size_t fileHash = dirTable + dirTableSize, fileTable = fileHash + BUCKETS * 4;
	const size_t partition = (fileTable + fileTableSize + 63) & ~(size_t)63;
	std::vector<uint8> img(partition + dataAt);
	memcpy(img.data(), "WUHB", 4);
	PutBE(img, 4, header, 4);
	const uint64 fields[] = {dirHash, BUCKETS * 4, dirTable, dirTableSize, fileHash, BUCKETS * 4, fileTable, fileTableSize, partition};
	for (int i = 0; i < 9; i++)
		PutBE(img, 8 + 8 * i, fields[i], 8);
	for (uint32 b = 0; b < BUCKETS; b++)
	{
		PutBE(img, dirHash + 4 * b, dirBucket[b], 4);
		PutBE(img, fileHash + 4 * b, fileBucket[b], 4);
	}
	for (const Dir& d : dirs)
	{
		const size_t e = dirTable + d.offset;
		PutBE(img, e, dirs[d.parent].offset, 4);
		PutBE(img, e + 4, 0xFFFFFFFF, 4); // sibling
		PutBE(img, e + 8, 0xFFFFFFFF, 4); // child
		PutBE(img, e + 12, 0xFFFFFFFF, 4);
		PutBE(img, e + 16, d.hash, 4);
		PutBE(img, e + 20, d.name.size(), 4);
		memcpy(img.data() + e + 24, d.name.data(), d.name.size());
	}
	// each directory's directories, for a listing (the root is no child)
	for (size_t i = dirs.size(); i-- > 1;)
	{
		const size_t pe = dirTable + dirs[dirs[i].parent].offset;
		const uint32 head = (uint32)img[pe + 8] << 24 | (uint32)img[pe + 9] << 16 | (uint32)img[pe + 10] << 8 | img[pe + 11];
		PutBE(img, dirTable + dirs[i].offset + 4, head, 4);
		PutBE(img, pe + 8, dirs[i].offset, 4);
	}
	// each directory's first file, for a listing
	for (size_t i = files.size(); i-- > 0;)
	{
		const size_t e = dirTable + dirs[dirIndex(files[i].dir)].offset;
		const uint32 head = (uint32)img[e + 12] << 24 | (uint32)img[e + 13] << 16 | (uint32)img[e + 14] << 8 | img[e + 15];
		PutBE(img, fileTable + fileOffset[i] + 4, head, 4);
		PutBE(img, e + 12, fileOffset[i], 4);
	}
	for (size_t i = 0; i < files.size(); i++)
	{
		const size_t e = fileTable + fileOffset[i];
		PutBE(img, e, dirs[dirIndex(files[i].dir)].offset, 4);
		PutBE(img, e + 8, fileData[i], 8);
		PutBE(img, e + 16, files[i].data.size(), 8);
		PutBE(img, e + 24, fileNext[i], 4);
		PutBE(img, e + 28, files[i].name.size(), 4);
		memcpy(img.data() + e + 32, files[i].name.data(), files[i].name.size());
		if (!files[i].data.empty())
			memcpy(img.data() + partition + fileData[i], files[i].data.data(), files[i].data.size());
	}
	return img;
}
