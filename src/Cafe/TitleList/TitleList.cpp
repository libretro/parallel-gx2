#include "TitleList.h"
#include "util/helpers/MappedFile.h"
#include "Common/FileStream.h"
#include "util/helpers/EventWake.h"

#include "util/helpers/helpers.h"

#include "Cafe/Filesystem/ZArchiveVFS.h"

// test/titlelist runs code of its own where a worker has served every
// Refresh it saw and is about to let go: the moment a Refresh can come in
// and find it still active
#ifdef TITLELIST_TEST_HOOKS
void TitleListTest_LettingGo();
#define TITLELIST_LETTING_GO() TitleListTest_LettingGo()
#else
#define TITLELIST_LETTING_GO() do {} while (0)
#endif

// The title list, without a lock. What is known is a CafeTitleListSnapshot
// in an RcuCell: a lookup reads the one in place, from any thread, and never
// waits; a title found or dropped makes the next list and puts it in place,
// one change at a time, after the lookups still in the old one have left.
// The titles themselves are shared by the lists that hold them, so one a
// scan drops is freed with the last list that had it.
//
// Note: The list may only contain titles with valid meta data (except for
// certain system titles). Entries loaded from the cache may not have been
// parsed yet, but they will use a cached value for titleId and titleVersion.

namespace
{
	// Built in its own storage and never destroyed: a scan that outlives a
	// close (Shutdown detaches it) still finds the list as it was.
	template<typename T>
	class Immortal
	{
	public:
		Immortal() { new (m_storage) T(); }
		Immortal(const Immortal&) = delete;
		Immortal& operator=(const Immortal&) = delete;
		T& Get() { return *std::launder(reinterpret_cast<T*>(m_storage)); }
		T* operator->() { return &Get(); }

	private:
		alignas(T) unsigned char m_storage[sizeof(T)];
	};

	struct TitleListPaths
	{
		fs::path cacheFile;
		fs::path mlc;
		std::vector<fs::path> scan;
	};

	Immortal<RcuCell<CafeTitleListSnapshot>> sTLState;
	Immortal<RcuCell<TitleListPaths>> sTLPaths;

	// Where the titles found since the current scan started are, by location
	// (TitleInfo::GetUID). Only ever touched in a writer's turn of sTLState: a
	// scan starts by clearing it, and ends by dropping the titles it had then
	// that are not in it.
	std::vector<uint64> sTLSeen;

	std::atomic_bool sTLInitialized{false};
	std::atomic_bool sTLCacheDirty{false};
	std::atomic_bool sTLIsScanMandatory{false};

	// The worker. Every Refresh is numbered (sTLRefreshRequested), and a scan
	// takes the number when it starts and, as it ends, says every Refresh up
	// to it is served (sTLRefreshServed): a wait for a scan waits for the
	// number its own Refresh was given, not for a flag a worker may clear an
	// instant before it scans again. Whoever turns sTLRefreshWorkerActive
	// from false to true starts a worker, or keeps the one it is running
	// going, and the worker looks for a Refresh it has not served once more
	// after letting go, so one made as it was finishing is never left
	// waiting. The thread handle is only touched by the Refresh that started
	// the worker, and by Shutdown; sTLRefreshWorkerStored says it is in
	// place, so that a worker never lets go before it is - the next Refresh
	// would take the handle while it is still being stored.
	std::atomic_uint64_t sTLRefreshRequested{0};
	std::atomic_uint64_t sTLRefreshServed{0};
	std::atomic_bool sTLRefreshWorkerActive{false};
	std::atomic_bool sTLRefreshWorkerStored{true};
	std::thread sTLRefreshWorker;
	// What a wait for the scan sleeps on; the worker notifies as it finishes.
	ImmortalEventWake sTLScanDone;

	// change() edits a copy of the list, in this writer's turn; the copy
	// becomes the list
	template<typename TChange>
	void UpdateList(TChange change)
	{
		sTLState->Write([&] {
			CafeTitleListSnapshot next;
			{
				auto current = sTLState->Read();
				next = *current;
			}
			change(next);
			return next;
		});
	}

	template<typename TChange>
	void UpdatePaths(TChange change)
	{
		sTLPaths->Write([&] {
			TitleListPaths next;
			{
				auto current = sTLPaths->Read();
				next = *current;
			}
			change(next);
			return next;
		});
	}

	TitleListPaths ReadPaths()
	{
		auto current = sTLPaths->Read();
		return *current;
	}

	// Puts titleInfo in the list unless a title at the same location is there,
	// taking it either way: a parsed title takes the place of one from the
	// cache. True if the list changed.
	bool AddTitleTo(CafeTitleListSnapshot& list, TitleInfo* titleInfo)
	{
		auto& titles = list.titles;
		auto it = std::find_if(titles.begin(), titles.end(), [titleInfo](const std::shared_ptr<TitleInfo>& known) { return known->IsEqualByLocation(*titleInfo); });
		if (it != titles.end())
		{
			if (titleInfo->IsCached() || !(*it)->IsCached())
			{
				// title already known
				delete titleInfo;
				return false;
			}
			// replace cached entry with newly parsed title
			titles.erase(it);
		}
		titles.emplace_back(titleInfo);
		return true;
	}
}

void CafeTitleList::Initialize(const fs::path cacheXmlFile)
{
	UpdatePaths([&](TitleListPaths& paths) { paths.cacheFile = cacheXmlFile; });
	sTLInitialized = true;
	LoadCacheFile();
}

void CafeTitleList::LoadCacheFile()
{
	sTLIsScanMandatory = true;
	cemu_assert_debug(sTLInitialized);
	MappedFile xmlData(ReadPaths().cacheFile);
	if (!xmlData.opened())
		return;
	xml::Document doc;
	if (!doc.load(xmlData.data(), xmlData.size()))
		return;
	std::vector<TitleInfo*> cached;
	for (const auto& titleInfoNode : doc.child("title_list"))
	{
		TitleId titleId;
		if( !TitleIdParser::ParseFromStr(titleInfoNode.attribute("titleId").as_string(), titleId))
			continue;
		uint16 titleVersion = titleInfoNode.attribute("version").as_uint();
		uint32 sdkVersion = titleInfoNode.attribute("sdk_version").as_uint();
		TitleInfo::TitleDataFormat format = (TitleInfo::TitleDataFormat)ConvertString<uint32>(titleInfoNode.child_value("format"));
		CafeConsoleRegion region = (CafeConsoleRegion)ConvertString<uint32>(titleInfoNode.child_value("region"));
		std::string name = titleInfoNode.child_value("name");
		std::string path = titleInfoNode.child_value("path");
		std::string sub_path = titleInfoNode.child_value("sub_path");
		uint32 group_id = ConvertString<uint32>(titleInfoNode.attribute("group_id").as_string(), 16);
		uint32 app_type = ConvertString<uint32>(titleInfoNode.attribute("app_type").as_string(), 16);

		TitleInfo::CachedInfo cacheEntry;
		cacheEntry.titleId = titleId;
		cacheEntry.titleVersion = titleVersion;
		cacheEntry.sdkVersion = sdkVersion;
		cacheEntry.titleDataFormat = format;
		cacheEntry.region = region;
		cacheEntry.titleName = std::move(name);
		cacheEntry.path = _utf8ToPath(path);
		cacheEntry.subPath = std::move(sub_path);
		cacheEntry.group_id = group_id;
		cacheEntry.app_type = app_type;

		TitleInfo* ti = new TitleInfo(cacheEntry);
		if (!ti->IsValid())
		{
			cemuLog_log(LogType::Force, "Title cache contained invalid title");
			delete ti;
			continue;
		}
		cached.emplace_back(ti);
	}
	bool added = false;
	UpdateList([&](CafeTitleListSnapshot& list) {
		for (TitleInfo* ti : cached)
			added |= AddTitleTo(list, ti);
	});
	if (added)
		sTLCacheDirty = true;
	sTLIsScanMandatory = false;
}

void CafeTitleList::StoreCacheFile()
{
	cemu_assert_debug(sTLInitialized);
	const fs::path cacheFile = ReadPaths().cacheFile;
	if (cacheFile.empty())
		return;

	xml::Document doc;
	auto title_list_node = doc.append_child("title_list");

	{
		auto list = sTLState->Read();
		for (const auto& tiIt : list->titles)
		{
			TitleInfo::CachedInfo info = tiIt->MakeCacheEntry();
			auto titleInfoNode = title_list_node.append_child("title");
			titleInfoNode.append_attribute("titleId").set_value(fmt::format("{:016x}", info.titleId).c_str());
			titleInfoNode.append_attribute("version").set_value(fmt::format("{:}", info.titleVersion).c_str());
			titleInfoNode.append_attribute("sdk_version").set_value(fmt::format("{:}", info.sdkVersion).c_str());
			titleInfoNode.append_attribute("group_id").set_value(fmt::format("{:08x}", info.group_id).c_str());
			titleInfoNode.append_attribute("app_type").set_value(fmt::format("{:08x}", info.app_type).c_str());
			titleInfoNode.append_child("region").text().set(fmt::format("{}", (uint32)info.region).c_str());
			titleInfoNode.append_child("name").text().set(info.titleName.c_str());
			titleInfoNode.append_child("format").text().set(fmt::format("{}", (uint32)info.titleDataFormat).c_str());
			titleInfoNode.append_child("path").text().set(_pathToUtf8(info.path).c_str());
			if(!info.subPath.empty())
				titleInfoNode.append_child("sub_path").text().set(_pathToUtf8(info.subPath).c_str());
		}
	}

	// through a file beside it that then takes its name
	const std::string out = doc.save(" ");
	if (!ReplaceFileContents(cacheFile, out.data(), out.size()))
		cemuLog_log(LogType::Force, "Unable to store title list in {}", _pathToUtf8(cacheFile));
}

void CafeTitleList::ClearScanPaths()
{
	UpdatePaths([](TitleListPaths& paths) { paths.scan.clear(); });
}

void CafeTitleList::AddScanPath(fs::path path)
{
	UpdatePaths([&](TitleListPaths& paths) { paths.scan.emplace_back(std::move(path)); });
}

void CafeTitleList::SetMLCPath(fs::path path)
{
	if (!FileStream::IsDirectory(path))
	{
		cemuLog_log(LogType::Force, "MLC set to invalid path: {}", _pathToUtf8(path));
		return;
	}
	UpdatePaths([&](TitleListPaths& paths) { paths.mlc = std::move(path); });
}

void CafeTitleList::Refresh()
{
	cemu_assert_debug(sTLInitialized);
	sTLRefreshRequested.fetch_add(1);
	if (!sTLRefreshWorkerActive.exchange(true))
	{
		// This caller starts the worker. The one before, if any, has let go,
		// so all it has left is to store the cache: joining it costs at most that.
		sTLRefreshWorkerStored = false;
		if (sTLRefreshWorker.joinable())
			sTLRefreshWorker.join();
		sTLRefreshWorker = std::thread(RefreshWorkerThread);
		sTLRefreshWorkerStored = true;
		sTLScanDone->Notify(); // the worker, if it is done already
	}
	sTLIsScanMandatory = false;
}

// The refresh worker is started by Refresh() and never joined: it clears
// sTLRefreshWorkerActive when it finishes, but the std::thread object stays
// joinable. In a library that gets unloaded, destroying that object during the
// static destructors is std::terminate - "terminate called without an active
// exception" as the process exits.
void CafeTitleList::Shutdown()
{
	// whoever started the worker has put its handle in place
	sTLScanDone->WaitUntil([] { return sTLRefreshWorkerStored.load(); });
	if (!sTLRefreshWorker.joinable())
		return;
	// A scan that is still running can take minutes - the paths it walks may
	// be a network mount - and waiting for it would hang the frontend on exit.
	// Detaching it at least leaves no joinable thread behind.
	if (IsScanning())
	{
		sTLRefreshWorker.detach();
		return;
	}
	// The usual case: every Refresh served, and a worker that is letting go -
	// at most still storing the cache, which this waits for rather than
	// leaving it writing as the library goes away.
	sTLRefreshWorker.join();
}

bool CafeTitleList::IsScanning()
{
	return sTLRefreshServed.load() < sTLRefreshRequested.load();
}

void CafeTitleList::WaitForMandatoryScan()
{
	if (!sTLIsScanMandatory)
		return;
	WaitForScan();
}

// Asleep until the scan finishes rather than checking every 100 ms: on the way
// to starting a title from a cold cache, that was up to a tenth of a second of
// nothing on top of the scan
void CafeTitleList::WaitForScan()
{
	const uint64 requested = sTLRefreshRequested.load();
	sTLScanDone->WaitUntil([requested] { return sTLRefreshServed.load() >= requested; });
}

// check if path is a valid title and if it is, permanently add it to the title list
// in the special case that path points to a WUA file, all contained titles will be added
void CafeTitleList::AddTitleFromPath(fs::path path)
{
	if (path.has_extension() && boost::iequals(_pathToUtf8(path.extension()), ".wua"))
	{
		ZArchivePtr zar = ZArchive_Open(path);
		std::vector<ZArchiveEntry> rootDir;
		if (!zar || !ZArchive_ListDir(zar.get(), "", rootDir))
		{
			cemuLog_log(LogType::Force, "Found {} but it is not a valid Wii U archive file", _pathToUtf8(path));
			return;
		}
		// enumerate all contained titles
		zar.reset();
		for (const ZArchiveEntry& dirEntry : rootDir)
		{
			if(!dirEntry.node.dir)
				continue;
			TitleId parsedId;
			uint16 parsedVersion;
			if (!TitleInfo::ParseWuaTitleFolderName(dirEntry.name, parsedId, parsedVersion))
			{
				cemuLog_log(LogType::Force, "Invalid title directory in {}: \"{}\"", _pathToUtf8(path), dirEntry.name);
				continue;
			}
			// valid subdirectory
			TitleInfo* titleInfo = new TitleInfo(path, dirEntry.name);
			if (titleInfo->IsValid())
				AddDiscoveredTitle(titleInfo);
			else
				delete titleInfo;
		}
		return;
	}
	TitleInfo* titleInfo = new TitleInfo(path);
	if (titleInfo->IsValid())
		AddDiscoveredTitle(titleInfo);
	else
		delete titleInfo;
}

bool CafeTitleList::RefreshWorkerThread()
{
	SetThreadName("TitleListWorker");
	for (;;)
	{
		while (sTLRefreshServed.load() < sTLRefreshRequested.load())
		{
			// every Refresh made by now is served by this scan
			const uint64 serving = sTLRefreshRequested.load();
			// create copies of all the paths
			const TitleListPaths paths = ReadPaths();
			// Remember where the titles known now are, and forget what the
			// last scan found: whatever this one does not find again is no
			// longer there, and goes at the end of it.
			std::vector<uint64> known;
			UpdateList([&](CafeTitleListSnapshot& list) {
				sTLSeen.clear();
				known.reserve(list.titles.size());
				for (const auto& it : list.titles)
					known.emplace_back(it->GetUID());
			});
			// scan game paths
			for (auto& it : paths.scan)
				ScanGamePath(it);
			// scan MLC
			if (!paths.mlc.empty())
			{
				for (FileStream::Dir d(paths.mlc / "usr/title"); d.Next();)
				{
					if (d.IsDirectory())
						ScanMLCPath(d.Path());
				}
				ScanMLCPath(paths.mlc / "sys/title/00050010");
				ScanMLCPath(paths.mlc / "sys/title/00050030");
			}

			// remove any titles that were known when the scan started and that it did not find
			bool removed = false;
			UpdateList([&](CafeTitleListSnapshot& list) {
				std::sort(known.begin(), known.end());
				std::sort(sTLSeen.begin(), sTLSeen.end());
				removed = std::erase_if(list.titles, [&](const std::shared_ptr<TitleInfo>& title) {
					const uint64 uid = title->GetUID();
					return std::binary_search(known.begin(), known.end(), uid) && !std::binary_search(sTLSeen.begin(), sTLSeen.end(), uid);
				}) != 0;
				sTLSeen.clear();
			});
			if (removed)
				sTLCacheDirty = true;
			sTLRefreshServed.store(serving);
			sTLScanDone->Notify(); // WaitForScan
		}
		TITLELIST_LETTING_GO();
		// let go, once the handle of this thread is in place
		sTLScanDone->WaitUntil([] { return sTLRefreshWorkerStored.load(); });
		sTLRefreshWorkerActive = false;
		if (sTLCacheDirty.exchange(false))
			StoreCacheFile();
		// A Refresh that came in after the last look, and found this worker
		// still active, left itself to it: serve it, unless another Refresh
		// has started a worker for it already.
		if (sTLRefreshServed.load() >= sTLRefreshRequested.load() || sTLRefreshWorkerActive.exchange(true))
			return true;
	}
}

bool _IsKnownFileNameOrExtension(const fs::path& path)
{
	std::string fileExtension = _pathToUtf8(path.extension());
	for (auto& it : fileExtension)
		it = _ansiToLower(it);
	if(fileExtension == ".tmd")
	{
		// must be "title.tmd"
		std::string fileName = _pathToUtf8(path.filename());
		for (auto& it : fileName)
			it = _ansiToLower(it);
		return fileName == "title.tmd";
	}
	return
		fileExtension == ".wud" ||
		fileExtension == ".wux" ||
		fileExtension == ".iso" ||
		fileExtension == ".wua" ||
		fileExtension == ".wuhb";
	// note: To detect extracted titles with RPX we rely on the presence of the content,code,meta directory structure
}

void CafeTitleList::ScanGamePath(const fs::path& path)
{
	// scan the whole directory first to determine if this is a title folder
	std::vector<fs::path> filesInDirectory;
	std::vector<fs::path> dirsInDirectory;
	bool hasContentFolder = false, hasCodeFolder = false, hasMetaFolder = false;
	for (FileStream::Dir d(path); d.Next();)
	{
		if (!d.IsDirectory())
		{
			filesInDirectory.emplace_back(d.Path());
		}
		else
		{
			dirsInDirectory.emplace_back(d.Path());
			const std::string_view dirName = d.Name();
			if (boost::iequals(dirName, "content"))
				hasContentFolder = true;
			else if (boost::iequals(dirName, "code"))
				hasCodeFolder = true;
			else if (boost::iequals(dirName, "meta"))
				hasMetaFolder = true;
		}
	}
	// always check individual files
	for (auto& it : filesInDirectory)
	{
		// since checking individual files is slow, we limit it to known file names or extensions
		if (!it.has_extension())
			continue;
		if (!_IsKnownFileNameOrExtension(it))
			continue;
		AddTitleFromPath(it);
	}
	// is the current directory a title folder?
	if (hasContentFolder && hasCodeFolder && hasMetaFolder)
	{
		// verify if this folder is a valid title
		TitleInfo* titleInfo = new TitleInfo(path);
		if (titleInfo->IsValid())
			AddDiscoveredTitle(titleInfo);
		else
			delete titleInfo;
		// if there are other folders besides content/code/meta then traverse those
		if (dirsInDirectory.size() > 3)
		{
			for (auto& it : dirsInDirectory)
			{
				std::string dirName = _pathToUtf8(it.filename());
				if (!boost::iequals(dirName, "content") &&
					!boost::iequals(dirName, "code") &&
					!boost::iequals(dirName, "meta"))
					ScanGamePath(it);
			}
		}
	}
	else
	{
		// scan subdirectories
		for (auto& it : dirsInDirectory)
			ScanGamePath(it);
	}
}

void CafeTitleList::ScanMLCPath(const fs::path& path)
{
	for (FileStream::Dir d(path); d.Next();)
	{
		if (!d.IsDirectory())
			continue;
		// only scan directories which match the title id naming scheme
		const std::string_view dirName = d.Name();
		if(dirName.size() != 8)
			continue;
		bool containsNoHexCharacter = false;
		for (auto& it : dirName)
		{
			if(it >= 'A' && it <= 'F' ||
				it >= 'a' && it <= 'f' ||
				it >= '0' && it <= '9')
				continue;
			containsNoHexCharacter = true;
			break;
		}
		if(containsNoHexCharacter)
			continue;

		const fs::path titlePath = d.Path();
		if (FileStream::IsDirectory(titlePath / "code") &&
			FileStream::IsDirectory(titlePath / "content") &&
			FileStream::IsDirectory(titlePath / "meta"))
		{
			TitleInfo* titleInfo = new TitleInfo(titlePath);
			if (titleInfo->IsValid() && titleInfo->ParseXmlInfo())
				AddDiscoveredTitle(titleInfo);
			else
				delete titleInfo;
		}
	}
}

void CafeTitleList::AddDiscoveredTitle(TitleInfo* titleInfo)
{
	cemu_assert_debug(titleInfo->ParseXmlInfo());
	const uint64 uid = titleInfo->GetUID();
	bool added = false;
	UpdateList([&](CafeTitleListSnapshot& list) {
		// found by the scan, if one is running
		sTLSeen.emplace_back(uid);
		added = AddTitleTo(list, titleInfo);
	});
	if (added)
		sTLCacheDirty = true;
}

bool CafeTitleList::HasTitle(TitleId titleId, uint16& versionOut)
{
	bool matchFound = false;
	versionOut = 0;
	auto list = sTLState->Read();
	for (const auto& it : list->titles)
	{
		if (it->GetAppTitleId() == titleId)
		{
			uint16 titleVersion = it->GetAppTitleVersion();
			if (titleVersion > versionOut)
				versionOut = titleVersion;
			matchFound = true;
		}
	}
	return matchFound;
}

bool CafeTitleList::HasTitleAndVersion(TitleId titleId, uint16 version)
{
	auto list = sTLState->Read();
	for (const auto& it : list->titles)
	{
		if (it->GetAppTitleId() == titleId && it->GetAppTitleVersion() == version)
			return true;
	}
	return false;
}

std::vector<TitleId> CafeTitleList::GetAllTitleIds()
{
	std::vector<TitleId> titleIds;
	auto list = sTLState->Read();
	titleIds.reserve(list->titles.size());
	for (const auto& it : list->titles)
	{
		TitleId tid = it->GetAppTitleId();
		if (std::find(titleIds.begin(), titleIds.end(), tid) == titleIds.end())
			titleIds.emplace_back(tid);
	}
	return titleIds;
}

CafeTitleList::ListRef CafeTitleList::ReadList()
{
	return sTLState->Read();
}

bool CafeTitleList::GetFirstByTitleId(TitleId titleId, TitleInfo& titleInfoOut)
{
	auto list = sTLState->Read();
	for (const auto& it : list->titles)
	{
		if (it->GetAppTitleId() == titleId)
		{
			titleInfoOut = *it;
			return true;
		}
	}
	return false;
}

// takes update or AOC title id and returns the title id of the associated base title
// this can fail if trying to translate an AOC title id without having the base title meta information
bool CafeTitleList::FindBaseTitleId(TitleId titleId, TitleId& titleIdBaseOut)
{
	titleId = TitleIdParser::MakeBaseTitleId(titleId);

	// aoc to base
	// todo - this requires scanning all base titles and their updates to see if they reference this title id
	// for now we assume there is a direct match of ids
	if (((titleId >> 32) & 0xFF) == 0x0C)
	{
		titleId &= ~0xFF00000000;
		titleId |= 0x0000000000;
	}
	titleIdBaseOut = titleId;
	return true;
}

GameInfo2 CafeTitleList::GetGameInfo(TitleId titleId)
{
	GameInfo2 gameInfo;

	// find base title id
	uint64 baseTitleId;
	if (!FindBaseTitleId(titleId, baseTitleId))
	{
		cemu_assert_suspicious();
	}
	// determine if an optional update title id exists
	TitleIdParser tip(baseTitleId);
	bool hasSeparateUpdateTitleId = tip.CanHaveSeparateUpdateTitleId();
	uint64 updateTitleId = 0;
	if (hasSeparateUpdateTitleId)
		updateTitleId = tip.GetSeparateUpdateTitleId();	
	// scan the title list for base and update
	auto list = sTLState->Read();
	for (const auto& it : list->titles)
	{
		TitleId appTitleId = it->GetAppTitleId();
		if (appTitleId == baseTitleId)
		{
			gameInfo.SetBase(*it);
		}
		if (hasSeparateUpdateTitleId && appTitleId == updateTitleId)
		{
			gameInfo.SetUpdate(*it);
		}
	}

	// if this title can have AOC content then do a second scan
	// todo - get a list of all AOC title ids from the base/update meta information
	// for now we assume there is a direct match between the base titleId and the aoc titleId
	if (tip.CanHaveSeparateUpdateTitleId())
	{
		uint64 aocTitleId = baseTitleId | 0xC00000000;
		for (const auto& it : list->titles)
		{
			TitleId appTitleId = it->GetAppTitleId();
			if (appTitleId == aocTitleId)
			{
				gameInfo.AddAOC(*it); // stores the AOC with the highest title version
			}
		}
	}
	return gameInfo;
}

TitleInfo CafeTitleList::GetTitleInfoByUID(uint64 uid)
{
	TitleInfo titleInfo;
	auto list = sTLState->Read();
	for (const auto& it : list->titles)
	{
		if (it->GetUID() == uid)
		{
			titleInfo = *it;
			break;
		}
	}
	return titleInfo;
}
