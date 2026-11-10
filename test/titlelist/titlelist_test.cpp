// The core's title list (src/Cafe/TitleList/TitleList.cpp), over a folder of
// stand-in titles made here (stub_titleinfo.cpp):
//  - a scan finds the titles in a folder and the ones below it, and writes
//    them to the cache file; a title whose file is gone is gone after the
//    next scan, and one added is there;
//  - the cache file read back on Initialize gives the titles it holds, the
//    same title found by a scan takes the cached one's place, and a cached
//    title the scan does not find is dropped;
//  - one title per title id, in the order found, with nothing allocated
//    (CafeTitleListSnapshot::ForEachFirstOfId, as MCP lists them), and the
//    version a lookup gives is the highest of them;
//  - Refresh then WaitForScan always waits for a scan that started after the
//    Refresh, with other threads refreshing all the while: a file put in
//    place before the Refresh is in the list after the wait, every time,
//    and a wait never hangs on a Refresh made as the worker finished, with
//    none after it (a watchdog fails the test after a minute);
//  - readers walking the list, looking titles up and copying them while
//    scans and AddTitleFromPath change it, with no lock: a title a scan
//    drops is freed only once no list that holds it is being read (run
//    under ThreadSanitizer and AddressSanitizer by CI);
//  - OnceGate: work done once by whichever of many threads comes first,
//    the others waiting for it, and again after a Reset.
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/Logging/CemuLogging.h"
#include "util/helpers/OnceGate.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

void SetThreadName(const char* name) {}

// A Refresh made just as the worker has served all it saw and is about to
// let go (TITLELIST_LETTING_GO in TitleList.cpp): it finds the worker still
// active and leaves itself to it, so the worker must look once more.
static std::atomic<int> s_refreshAsLettingGo{0};
// A worker slow to let go, for Shutdown to wait for
static std::atomic<bool> s_slowLettingGo{false};

void TitleListTest_LettingGo()
{
	if (s_slowLettingGo.exchange(false))
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	if (s_refreshAsLettingGo.load() > 0)
	{
		s_refreshAsLettingGo--;
		CafeTitleList::Refresh();
	}
}

static void WriteTitle(const fs::path& path, uint64 titleId, uint16 version)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << fmt::format("{:016x} {}\n", titleId, version);
}

static bool Has(uint64 titleId)
{
	uint16 version;
	return CafeTitleList::HasTitle(titleId, version);
}

static size_t Count()
{
	auto list = CafeTitleList::ReadList();
	return list->titles.size();
}

static void Rescan()
{
	CafeTitleList::Refresh();
	CafeTitleList::WaitForScan();
}

static std::string ReadText(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

constexpr uint64 TA = 0x0005000010100000;
constexpr uint64 TB = 0x0005000010200000;
constexpr uint64 TC = 0x0005000010300000;
constexpr uint64 TD = 0x0005000010400000;

static void TestOnceGate()
{
	OnceGate gate;
	std::atomic<int> runs{0};
	std::atomic<int> sawUndone{0};
	std::atomic<bool> inside{false};
	std::vector<std::thread> threads;
	for (int round = 0; round < 3; round++)
	{
		threads.clear();
		for (int i = 0; i < 8; i++)
		{
			threads.emplace_back([&] {
				gate.Run([&] {
					inside = true;
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					runs++;
					inside = false;
				});
				// whoever returns, returns with the work done
				if (!gate.IsDone() || inside.load())
					sawUndone++;
			});
		}
		for (auto& t : threads)
			t.join();
		CHECK(runs.load() == round + 1);
		CHECK(sawUndone.load() == 0);
		CHECK(gate.IsDone());
		gate.Reset();
		CHECK(!gate.IsDone());
	}
}

static void TestForEachFirstOfId(const fs::path& dir)
{
	// two copies of A (versions 1 and 5), B, and C
	CafeTitleListSnapshot list;
	auto add = [&](const char* name, uint64 id, uint16 version) {
		WriteTitle(dir / name, id, version);
		list.titles.emplace_back(std::make_shared<TitleInfo>(dir / name));
	};
	add("x1.wud", TA, 1);
	add("x2.wud", TB, 0);
	add("x3.wud", TA, 5);
	add("x4.wud", TC, 0);
	std::vector<std::pair<uint64, uint16>> got;
	list.ForEachFirstOfId([](const TitleInfo&) { return true; }, [&](const TitleInfo& t) {
		got.emplace_back(t.GetAppTitleId(), t.GetAppTitleVersion());
		return true;
	});
	CHECK(got.size() == 3);
	if (got.size() == 3)
	{
		CHECK(got[0] == std::make_pair(TA, (uint16)1));
		CHECK(got[1] == std::make_pair(TB, (uint16)0));
		CHECK(got[2] == std::make_pair(TC, (uint16)0));
	}
	// a filter that turns the first A down lets the second one through
	got.clear();
	list.ForEachFirstOfId([](const TitleInfo& t) { return t.GetAppTitleVersion() != 1; }, [&](const TitleInfo& t) {
		got.emplace_back(t.GetAppTitleId(), t.GetAppTitleVersion());
		return true;
	});
	CHECK(got.size() == 3 && got[0] == std::make_pair(TB, (uint16)0) && got[1] == std::make_pair(TA, (uint16)5));
	// stops when told to
	int calls = 0;
	list.ForEachFirstOfId([](const TitleInfo&) { return true; }, [&](const TitleInfo&) { return ++calls < 2; });
	CHECK(calls == 2);
	for (const char* n : {"x1.wud", "x2.wud", "x3.wud", "x4.wud"})
		fs::remove(dir / n);
}

int main()
{
	// A wait that never ends is the failure this test looks for most.
	std::atomic<bool> finished{false};
	std::thread watchdog([&] {
		for (int i = 0; i < 600 && !finished.load(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!finished.load())
		{
			std::fprintf(stderr, "FAIL: timed out - a wait for a scan never ended\n");
			std::fflush(stderr);
			std::_Exit(1);
		}
	});

	TestOnceGate();

	const fs::path root = fs::temp_directory_path() / fmt::format("titlelist_test_{}", (uint64)std::chrono::steady_clock::now().time_since_epoch().count());
	const fs::path games = root / "games";
	const fs::path cache = root / "title_list_cache.xml";
	fs::create_directories(games / "sub");
	fs::create_directories(root / "other");

	TestForEachFirstOfId(root / "other");

	// a scan finds what is there, below it too, and keeps it in the cache
	WriteTitle(games / "a.wud", TA, 0);
	WriteTitle(games / "b.wud", TB, 16);
	WriteTitle(games / "sub" / "c.wud", TC, 0);
	WriteTitle(games / "notes.txt", TD, 0); // not a title file name
	// A cache from the last run: A where it still is, and X, which is gone.
	// Both are in the list before any scan.
	constexpr uint64 TX = 0x0005000010900000;
	{
		std::ofstream f(cache, std::ios::binary);
		f << "<?xml version=\"1.0\"?>\n<title_list>\n";
		for (auto [id, path] : {std::make_pair(TA, games / "a.wud"), std::make_pair(TX, games / "gone.wud")})
			f << fmt::format(" <title titleId=\"{:016x}\" version=\"3\" sdk_version=\"0\" group_id=\"0\" app_type=\"0\">\n  <region>2</region>\n  <name>t</name>\n  <format>2</format>\n  <path>{}</path>\n </title>\n", id, _pathToUtf8(path));
		f << "</title_list>\n";
	}
	CafeTitleList::Initialize(cache);
	CHECK(Count() == 2);
	CHECK(Has(TA) && Has(TX));
	CHECK(CafeTitleList::HasTitleAndVersion(TA, 3)); // as the cache had it
	CafeTitleList::AddScanPath(games);
	Rescan();
	// the scan's A in place of the cached one, X dropped
	CHECK(Has(TA) && Has(TB) && Has(TC) && !Has(TD) && !Has(TX));
	CHECK(Count() == 3);
	CHECK(CafeTitleList::HasTitleAndVersion(TA, 0) && !CafeTitleList::HasTitleAndVersion(TA, 3));
	uint16 version = 0;
	CHECK(CafeTitleList::HasTitle(TB, version) && version == 16);
	CHECK(CafeTitleList::HasTitleAndVersion(TB, 16) && !CafeTitleList::HasTitleAndVersion(TB, 15));
	TitleInfo first;
	CHECK(CafeTitleList::GetFirstByTitleId(TC, first) && first.GetAppTitleId() == TC);
	CHECK(CafeTitleList::GetAllTitleIds().size() == 3);
	// the worker stores the cache as it lets go: give it the moment
	for (int i = 0; i < 200 && ReadText(cache).find("0005000010300000") == std::string::npos; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(ReadText(cache).find("0005000010100000") != std::string::npos);
	CHECK(ReadText(cache).find("0005000010300000") != std::string::npos);

	// a second copy of A, newer: one per title id, the highest version wins
	WriteTitle(games / "sub" / "a2.wud", TA, 32);
	Rescan();
	CHECK(Count() == 4);
	CHECK(CafeTitleList::HasTitle(TA, version) && version == 32);
	CHECK(CafeTitleList::GetAllTitleIds().size() == 3);

	// a title whose file is gone goes with the next scan; one put back comes back
	fs::remove(games / "b.wud");
	Rescan();
	CHECK(!Has(TB) && Count() == 3);
	WriteTitle(games / "b.wud", TB, 16);
	Rescan();
	CHECK(Has(TB) && Count() == 4);

	// AddTitleFromPath, outside the scan paths, stays until a scan misses it
	WriteTitle(root / "other" / "d.wud", TD, 0);
	CafeTitleList::AddTitleFromPath(root / "other" / "d.wud");
	CHECK(Has(TD));
	CafeTitleList::AddTitleFromPath(root / "other" / "d.wud"); // known: no second one
	CHECK(Count() == 5);
	Rescan();
	CHECK(!Has(TD) && Count() == 4);

	// What the cache holds follows the list, and is written by the time
	// Shutdown returns: a worker that has served every Refresh is joined, not
	// left writing the cache as the library goes away. B is in the cache
	// now; the scan below drops it, and its worker is slow to let go.
	for (int i = 0; i < 200 && ReadText(cache).find("0005000010200000") == std::string::npos; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(ReadText(cache).find("0005000010200000") != std::string::npos);
	fs::remove(games / "b.wud");
	s_slowLettingGo = true;
	Rescan();
	CafeTitleList::Shutdown();
	{
		std::string text = ReadText(cache);
		CHECK(text.find("0005000010100000") != std::string::npos);
		CHECK(text.find("0005000010200000") == std::string::npos);
		CHECK(text.find("0005000010400000") == std::string::npos);
		CHECK(text.find("0005000010900000") == std::string::npos);
	}
	WriteTitle(games / "b.wud", TB, 16);
	Rescan();
	CHECK(Has(TB) && Count() == 4);

	// A Refresh that comes as the worker lets go is served all the same: with
	// no other Refresh after it, a worker that did not look again would leave
	// this wait hanging (the watchdog).
	for (int i = 0; i < 5; i++)
	{
		const fs::path p = games / "late.wud";
		WriteTitle(p, TD, 0);
		s_refreshAsLettingGo = 1;
		CafeTitleList::Refresh();
		while (s_refreshAsLettingGo.load() > 0)
			std::this_thread::yield();
		CafeTitleList::WaitForScan();
		CHECK(Has(TD));
		fs::remove(p);
		Rescan();
		CHECK(!Has(TD));
	}

	// Refresh then WaitForScan, with refreshes all around: what was there
	// before the Refresh is in the list after the wait, every time.
	{
		std::atomic<bool> stop{false};
		std::thread spammer([&] {
			while (!stop.load())
			{
				CafeTitleList::Refresh();
				std::this_thread::yield();
			}
		});
		for (int i = 0; i < 300; i++)
		{
			const fs::path p = games / fmt::format("e{}.wud", i);
			WriteTitle(p, 0x0005000020000000ull + i, 0);
			Rescan();
			if (!Has(0x0005000020000000ull + i))
			{
				CHECK(!"a file in place before Refresh was missing after WaitForScan");
				break;
			}
			fs::remove(p);
		}
		stop = true;
		spammer.join();
		CafeTitleList::WaitForScan();
		Rescan();
		CHECK(Count() == 4);
	}

	// Readers with no lock, while scans and AddTitleFromPath change the list.
	{
		std::atomic<bool> stop{false};
		std::atomic<uint64> reads{0};
		std::vector<std::thread> threads;
		for (int r = 0; r < 3; r++)
		{
			threads.emplace_back([&] {
				while (!stop.load())
				{
					{
						auto list = CafeTitleList::ReadList();
						uint64 sum = 0;
						for (const auto& t : list->titles)
							sum += t->GetAppTitleId() + t->GetAppTitleVersion() + t->GetPath().native().size();
						list->ForEachFirstOfId([](const TitleInfo&) { return true; }, [&](const TitleInfo& t) { sum ^= t.GetAppTitleId(); return true; });
						if (sum == 1)
							std::abort(); // keep the walk
					}
					TitleInfo copy;
					if (CafeTitleList::GetFirstByTitleId(TD, copy))
						CHECK(copy.GetAppTitleId() == TD);
					GameInfo2 game = CafeTitleList::GetGameInfo(TA);
					CHECK(game.GetBase().IsValid());
					uint16 v;
					CafeTitleList::HasTitle(TB, v);
					reads++;
				}
			});
		}
		threads.emplace_back([&] {
			for (int i = 0; i < 40; i++)
			{
				CafeTitleList::Refresh();
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		});
		threads.emplace_back([&] {
			for (int i = 0; i < 40; i++)
			{
				const fs::path p = games / "flicker.wud";
				if (i & 1)
					fs::remove(p);
				else
					WriteTitle(p, TD, (uint16)i);
				CafeTitleList::AddTitleFromPath(root / "other" / "d.wud");
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
			}
			fs::remove(games / "flicker.wud");
		});
		threads[3].join();
		threads[4].join();
		stop = true;
		for (int r = 0; r < 3; r++)
			threads[r].join();
		CHECK(reads.load() > 0);
		// settled: what is in the folder now, and nothing else
		Rescan();
		CHECK(Count() == 4);
		CHECK(!Has(TD));
	}

	CafeTitleList::Shutdown();
	// and it starts again after Shutdown, as a core loaded a second time does
	Rescan();
	CHECK(Count() == 4);
	CafeTitleList::Shutdown();

	fs::remove_all(root);
	finished = true;
	watchdog.join();
	if (s_failures)
	{
		std::fprintf(stderr, "%d check(s) failed\n", s_failures);
		return 1;
	}
	std::printf("titlelist: all checks passed\n");
	return 0;
}
