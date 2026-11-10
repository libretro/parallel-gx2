#include "Cafe/Filesystem/ZArchiveVFS.h"
#include "Common/FileStream.h"
#include "util/helpers/RcuCell.h"

// --- archives shared by path ----------------------------------------------------

struct ZArchiveShared
{
	fs::path path;
	zarchive_t* archive;
	uint32 holders;
};

static RcuCell<std::vector<ZArchiveShared>> s_zarchiveShared;

zarchive_t* ZArchive_AcquireShared(const fs::path& path)
{
	// held already: one more holder
	zarchive_t* found = nullptr;
	auto hold = [&]() {
		s_zarchiveShared.Write([&] {
			std::vector<ZArchiveShared> table = *s_zarchiveShared.Read();
			for (ZArchiveShared& e : table)
			{
				if (e.path == path)
				{
					e.holders++;
					found = e.archive;
					break;
				}
			}
			return table;
		});
	};
	bool there = false;
	{
		// not held while writing: a writer waits for the readers before it
		auto table = s_zarchiveShared.Read();
		for (const ZArchiveShared& e : *table)
			there = there || e.path == path;
	}
	if (there)
		hold();
	if (found)
		return found;
	// opening one can take a while: done outside the table's writer's turn
	zarchive_t* opened = zarchive_open(FileStream::HostPath(path).c_str(), 0, nullptr, 0);
	if (!opened)
		return nullptr;
	s_zarchiveShared.Write([&] {
		std::vector<ZArchiveShared> table = *s_zarchiveShared.Read();
		for (ZArchiveShared& e : table)
		{
			if (e.path == path)
			{
				// opened by another meanwhile: that one is shared
				e.holders++;
				found = e.archive;
				return table;
			}
		}
		table.push_back({path, opened, 1});
		found = opened;
		return table;
	});
	if (found != opened)
		zarchive_close(opened);
	return found;
}

void ZArchive_ReleaseShared(zarchive_t* zar)
{
	if (!zar)
		return;
	bool last = false;
	s_zarchiveShared.Write([&] {
		std::vector<ZArchiveShared> table = *s_zarchiveShared.Read();
		for (size_t i = 0; i < table.size(); i++)
		{
			if (table[i].archive != zar)
				continue;
			cemu_assert(table[i].holders > 0);
			if (--table[i].holders == 0)
			{
				table.erase(table.begin() + i);
				last = true;
			}
			return table;
		}
		cemu_assert(false); // not shared
		return table;
	});
	if (last)
		zarchive_close(zar);
}

size_t ZArchive_SharedCount()
{
	return s_zarchiveShared.Read()->size();
}
