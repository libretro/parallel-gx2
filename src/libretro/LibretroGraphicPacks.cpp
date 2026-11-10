#include "LibretroGraphicPacks.h"

#include <cstring>
#include "util/helpers/Deflate.h"

#include "config/ActiveSettings.h"
#include "Common/FileStream.h"

// tools/pack_graphic_packs.c writes these
extern const unsigned char g_libretro_graphic_packs[];
extern const std::size_t g_libretro_graphic_packs_size;
extern const std::size_t g_libretro_graphic_packs_unpacked_size;
extern const char g_libretro_graphic_packs_version[];

namespace
{
	std::string readFirstLine(const fs::path& path)
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(path));
		std::string line;
		if (file)
			file->readLine(line);
		return line;
	}
}

// graphicPacks holds the build's set and nothing else, with its version in
// version.txt: when it is not this build's, the folder is emptied and the set
// unpacked afresh, so a pack renamed or removed upstream does not linger next
// to its replacement. The user's own packs go in customGraphicPacks, which
// this never touches (see libretro_load_graphic_packs).
void LibretroGraphicPacks_InstallBundled()
{
	if (g_libretro_graphic_packs_size == 0)
		return;

	const fs::path target = ActiveSettings::GetUserDataPath("graphicPacks");
	const fs::path versionFile = target / "version.txt";
	const std::string version = std::string("Bundled with the core: ") + g_libretro_graphic_packs_version;
	if (readFirstLine(versionFile) == version)
		return;

	// the archive is a run of entries: path, NUL, size (4 bytes LE), data
	std::vector<uint8> archive(g_libretro_graphic_packs_unpacked_size);
	size_t archiveLen = archive.size();
	size_t packedLen = g_libretro_graphic_packs_size;
	if (Deflate::Uncompress(archive.data(), archiveLen, g_libretro_graphic_packs, packedLen) != Deflate::Result::Ok
			|| archiveLen != archive.size())
	{
		cemuLog_log(LogType::Force, "graphic packs: the set compiled into the core could not be opened");
		return;
	}

	for (FileStream::Dir d(target); d.Next();)
		FileStream::RemoveAll(d.Path());
	FileStream::CreateDirectories(target);

	uint32 written = 0;
	size_t pos = 0;
	while (pos < archive.size())
	{
		const char* name = (const char*)archive.data() + pos;
		const size_t nameLen = strnlen(name, archive.size() - pos);
		if (pos + nameLen + 5 > archive.size())
			break;
		pos += nameLen + 1;
		const uint32 size = archive[pos] | (archive[pos + 1] << 8) | (archive[pos + 2] << 16) | ((uint32)archive[pos + 3] << 24);
		pos += 4;
		if (size > archive.size() - pos)
			break;
		const uint8* data = archive.data() + pos;
		pos += size;
		if (nameLen == 0 || std::strstr(name, "../") || std::strstr(name, "..\\") || name[0] == '/')
			continue;
		const fs::path path = target / _utf8ToPath(name);
		FileStream::CreateDirectories(path.parent_path());
		std::unique_ptr<FileStream> out(FileStream::createFile2(path));
		if (out)
		{
			out->writeData(data, size);
			written++;
		}
	}

	std::unique_ptr<FileStream> file(FileStream::createFile2(versionFile));
	if (file)
		file->writeString(version.c_str());
	cemuLog_log(LogType::Force, "graphic packs: unpacked the {} files compiled into the core into {}", written, _pathToUtf8(target));
}
