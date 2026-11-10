#pragma once

#include "TitleInfo.h"
#include "GameInfo.h"
#include "util/helpers/RcuCell.h"

#include <memory>

// The titles known at one moment, in the order they were found. A scan never
// changes one: it builds the next and puts it in place, and a title it drops
// lives on for as long as a list that was read before still holds it.
struct CafeTitleListSnapshot
{
	std::vector<std::shared_ptr<TitleInfo>> titles;

	// fn(title) for the first title of each title id that accept(title)
	// lets through, in the order they were found, until fn returns false.
	// Nothing is allocated: the titles before a match are looked through
	// again instead, which for the few a list holds costs less than a set.
	template<typename TAccept, typename TFn>
	void ForEachFirstOfId(TAccept accept, TFn fn) const
	{
		for (size_t i = 0; i < titles.size(); i++)
		{
			const TitleInfo& title = *titles[i];
			if (!accept(title))
				continue;
			const TitleId titleId = title.GetAppTitleId();
			bool seen = false;
			for (size_t j = 0; j < i && !seen; j++)
				seen = titles[j]->GetAppTitleId() == titleId && accept(*titles[j]);
			if (seen)
				continue;
			if (!fn(title))
				return;
		}
	}
};

class CafeTitleList
{
public:

	static void Initialize(const fs::path cacheXmlFile);
	static void LoadCacheFile();
	static void StoreCacheFile();

	static void ClearScanPaths();
	static void AddScanPath(fs::path path);
	static void SetMLCPath(fs::path path);
	static void Refresh(); // scan all paths
	static bool IsScanning(); // returns true if async refresh is currently active
	static void WaitForMandatoryScan(); // wait for current scan result if no cached info is available
	static void WaitForScan(); // wait for the scan that is running, if any
	static void Shutdown(); // join the refresh worker, so nothing outlives the library
	static void AddTitleFromPath(fs::path path);

	// utility functions
	static bool HasTitle(TitleId titleId, uint16& versionOut);
	static bool HasTitleAndVersion(TitleId titleId, uint16 version);
	static std::vector<TitleId> GetAllTitleIds();
	static bool GetFirstByTitleId(TitleId titleId, TitleInfo& titleInfoOut);
	static bool FindBaseTitleId(TitleId titleId, TitleId& titleIdBaseOut);

	// The list as it is now, with no lock: (*list).titles. A scan that wants
	// to change the list waits for the readers already in to leave, so hold
	// one for a walk over it, not across a wait - and never while this thread
	// adds a title or scans.
	using ListRef = RcuCell<CafeTitleListSnapshot>::Reader;
	static ListRef ReadList();

	static GameInfo2 GetGameInfo(TitleId titleId);
	static TitleInfo GetTitleInfoByUID(uint64 uid);

private:
	static bool RefreshWorkerThread();
	static void ScanGamePath(const fs::path& path);
	static void ScanMLCPath(const fs::path& path);

	static void AddDiscoveredTitle(TitleInfo* titleInfo);
};
