#pragma once
#include "Cafe/Filesystem/zarchive.h"
#include "Common/FileStream.h"

// What the C++ side keeps over Wii U archives (zarchive.h): one archive
// shared by every title that holds it, and listings taken a name at a time.

// The one archive open for a path, shared by whatever holds it: opened by
// the first to ask (two at once may both open it; one is kept), closed when
// the last gives it back. No lock: the table of shared archives is replaced
// whole (RcuCell).
zarchive_t* ZArchive_AcquireShared(const fs::path& path);
void ZArchive_ReleaseShared(zarchive_t* zar);
// archives shared now
size_t ZArchive_SharedCount();

// fn(name, node) for each entry of the directory at path, in name order;
// false when the directory could not be listed. Nothing allocated: name is
// valid during the call.
template<typename TFn>
bool ZArchive_ForEachEntry(const zarchive_t* zar, std::string_view path, TFn fn)
{
	zarchive_dir_t dir;
	if (!zarchive_dir_open(zar, &dir, path.data(), path.size()))
	{
		zarchive_dir_close(&dir);
		return false;
	}
	const char* name;
	size_t len;
	zar_file_t node;
	while (zarchive_dir_next(&dir, &name, &len, &node))
		fn(std::string_view(name, len), node);
	const bool ok = !zarchive_dir_failed(&dir);
	zarchive_dir_close(&dir);
	return ok;
}

// A directory's entries collected, for a scan that holds on to them (the
// title list, a title's detection); a game's own listings go a name at a
// time (fscDeviceWua) and allocate nothing
struct ZArchiveEntry
{
	std::string name;
	zar_file_t node;
};

inline bool ZArchive_ListDir(const zarchive_t* zar, std::string_view path, std::vector<ZArchiveEntry>& entries)
{
	entries.clear();
	return ZArchive_ForEachEntry(zar, path, [&](std::string_view name, const zar_file_t& node) {
		entries.push_back({std::string(name), node});
	});
}

struct ZArchiveCloser
{
	void operator()(zarchive_t* zar) const { zarchive_close(zar); }
};
using ZArchivePtr = std::unique_ptr<zarchive_t, ZArchiveCloser>;

// An archive opened for one use, the path as FileStream opens one
inline ZArchivePtr ZArchive_Open(const fs::path& path, std::string* whyNot = nullptr)
{
	char why[512];
	ZArchivePtr zar(zarchive_open(FileStream::HostPath(path).c_str(), 0, why, sizeof(why)));
	if (!zar && whyNot)
		*whyNot = why;
	return zar;
}
