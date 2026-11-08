// The lock-free handoffs between the core's threads, each tested for what its
// users rely on:
//  - the recompiler's work queue delivers every address exactly once and in
//    each pusher's order, wakes its consumer on a push (no poll - a lost wake
//    hangs this test), refuses rather than grows when full, and lets a waiting
//    consumer out on a stop;
//  - the GPU thread's wake ends a wait as soon as the predicate is made true,
//    a bounded wait comes back by its deadline when it is not, and the boot's
//    back-and-forth between the GPU thread and the CPU thread - several
//    waiters, each with its own predicate, on the one wake - never loses one;
//  - the GPU's command ring as LatteCP_readU32Deprc now waits on it: every word
//    arrives, in order, and a consumer with nothing to read sleeps until a
//    submission or its vsync deadline instead of spinning;
//  - EventWake, the eventcount the title scan, the IOSU ioctl workers'
//    shutdown, the boss daemon and its task waits, and nim's waits sleep on:
//    the last of many workers out wakes the shutdown wait, a bounded wait
//    comes back by its deadline, a daemon with nothing to run sleeps until a
//    task is made ready or it is stopped;
//  - log.txt's lock-free sink: lines written from many threads at once come
//    out whole and all there, and switching the file off and on under them
//    never closes a handle a write is still using;
//  - SeqlockValue (the GamePad's motion, retro_run to the guest): a reader
//    never sees half of one sample and half of another;
//  - MpscList (the GamePad's rumble requests, the guest to retro_run): every
//    item arrives, each producer's in the order pushed;
//  - LibretroVkQueue: the frontend's queue lock is always released through
//    the interface it was taken through, while the frontend swaps interfaces
//    under a submitting GPU thread, and a frame that hands over the same
//    interface again does not wait;
//  - the audio devices' gate (AudioDevicesGate.h): AX threads playing
//    through a device while a title's start and stop create and destroy it
//    never touch one being destroyed, and are never kept waiting;
//  - AtomicPageSet (the data cache flushes the CPU reports for the GPU): a
//    page added before a drain begins is reported, by it or an earlier one,
//    and every page added is reported once all is drained;
//  - PPCTimerClock (the guest's time base): never goes backwards on any core
//    or across them, runs at the console's clock over the TSC's rate times
//    the shift, and moves no time already counted when the rate is refined
//    or the shift changes under readers;
//  - AtomicPtrBag (the pipelines a shader or framebuffer is used by): compile
//    threads registering and dropping pipelines on shared shaders at once
//    lose none and keep none they dropped, and an owner's destruction hands
//    each remaining one over once, even as their destructors remove them
//    from it and from the other bags;
//  - MpscQueue and MpmcQueue (the GX2 event, thread deallocator, curl and
//    pipeline cache writer queues; the two pipeline compile pools): every
//    item taken exactly once, each producer's in order where promised, a pop
//    asleep until there is something, and the MPMC overflow past its ring
//    handed back to sleeping consumers;
//  - ClaimTicket (the shader compile pool): a queued shader is compiled once,
//    by a pool thread or by the thread that needs it now, never after it was
//    destroyed, and a destroyed one's ticket left in the queue is harmless;
//  - nexWait (the nex thread and the online logins): a wait ends when a packet
//    arrives or the wake is signalled from another thread, well before its
//    timeout, and runs to the timeout otherwise;
//  - RcuCell (the emulated controllers and their mappings): readers never see
//    a value freed or half-built, however many are inside when a writer
//    replaces it, and a writer is never held up by readers that came after
//    it;
//  - the thread-started flag the IOSU services start their threads with lets
//    the starter go as soon as the thread says so, not on a clock;
//  - RetroArch's audio thread, pulling from an empty ring, sleeps until the
//    producer's next block (ending as it arrives, not on a fixed millisecond),
//    a stop, or its bound;
//  - the audio ring hands out whole frames in place, across the wrap, for 2 and
//    6 channels, and streams intact between a producer and a consumer thread.
#include "Cafe/HW/Espresso/Recompiler/PPCRecompilerWorkQueue.h"
#include "Cafe/HW/Latte/Core/LatteGpuWake.h"
#include "util/helpers/ThreadStartedFlag.h"
#include "util/helpers/EventWake.h"
#include "Cemu/Logging/LogFileSink.h"
#include "util/helpers/SeqlockValue.h"
#include "util/helpers/MpscList.h"
#include "libretro/LibretroVkQueue.h"
#include "audio/AudioDevicesGate.h"
#include "util/helpers/AtomicPageSet.h"
#include "util/helpers/AtomicPtrBag.h"
#include "util/helpers/MpscQueue.h"
#include "util/helpers/MpmcQueue.h"
#include "util/helpers/ClaimTicket.h"
#include "Cemu/nex/nexWait.h"
#include "util/helpers/RcuCell.h"
#include "Cafe/HW/Espresso/PPCTimerClock.h"
#include <set>
#include "libretro.h"
#include "libretro_vulkan.h"
#include <fstream>
#include <string>
#include "libretro/LibretroAudioRing.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- recompiler work queue ----------------------------------------------

// The core's pushers are serialised by the recompiler's spinlock; this is that
static std::atomic_flag s_pushLock = ATOMIC_FLAG_INIT;

static void TestWorkQueueStream()
{
	auto q = std::make_unique<PPCRecompilerWorkQueue>();
	CHECK(q->Init());
	constexpr uint32_t kProducers = 3, kPerProducer = 60000;
	std::atomic_bool stop{false};
	std::vector<uint32_t> lastSeen(kProducers, 0);
	std::vector<uint8_t> seen(kProducers * kPerProducer + 1, 0);
	std::atomic<uint32_t> received{0};
	bool ordered = true, unique = true;

	std::thread consumer([&] {
		for (;;)
		{
			q->WaitForWork(stop);
			uint32_t a;
			while (q->Pop(a))
			{
				const uint32_t producer = a / (kPerProducer + 1);
				const uint32_t seq = a % (kPerProducer + 1);
				if (seq <= lastSeen[producer])
					ordered = false;
				lastSeen[producer] = seq;
				if (seen[producer * kPerProducer + seq - 1]++)
					unique = false;
				received.fetch_add(1, std::memory_order_relaxed);
			}
			if (stop.load())
				return;
		}
	});
	std::vector<std::thread> producers;
	for (uint32_t p = 0; p < kProducers; p++)
	{
		producers.emplace_back([&, p] {
			for (uint32_t seq = 1; seq <= kPerProducer;)
			{
				while (s_pushLock.test_and_set(std::memory_order_acquire))
					std::this_thread::yield();
				const bool pushed = q->Push(p * (kPerProducer + 1) + seq);
				s_pushLock.clear(std::memory_order_release);
				if (pushed)
					seq++;
				else
					std::this_thread::yield(); // full: the interpreter would offer it again later
			}
		});
	}
	for (auto& t : producers)
		t.join();
	while (received.load() < kProducers * kPerProducer)
		std::this_thread::yield();
	stop = true;
	q->Wake();
	consumer.join();
	CHECK(received.load() == kProducers * kPerProducer);
	CHECK(ordered);
	CHECK(unique);
	q->Free();
}

static void TestWorkQueueFullAndStop()
{
	auto q = std::make_unique<PPCRecompilerWorkQueue>();
	CHECK(!q->Push(1)); // not initialised: refused, not a crash
	CHECK(q->Init());
	size_t pushed = 0;
	while (q->Push((uint32_t)pushed + 1))
		pushed++;
	CHECK(pushed == PPCRecompilerWorkQueue::kCapacity); // bounded, and that bound
	uint32_t a = 0;
	CHECK(q->Pop(a) && a == 1);
	CHECK(q->Push(0xABCD)); // room again
	q->Clear();
	CHECK(!q->Pop(a));

	// A consumer asleep on an empty queue is woken by a push: it has been
	// asleep long enough to be past any spin when the push comes, and a lost
	// wake shows as the push never being taken
	for (int round = 0; round < 5; round++)
	{
		std::atomic_bool never{false};
		std::atomic_bool took{false};
		std::thread sleeper([&] {
			q->WaitForWork(never);
			uint32_t v;
			took = q->Pop(v) && v == 77;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		CHECK(!took.load());
		CHECK(q->Push(77));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!took.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(took.load());
		if (!took.load())
		{
			// Let it out so the test can end and say so
			never = true;
			q->Wake();
		}
		sleeper.join();
	}

	// A consumer asleep on an empty queue is let out by a stop
	std::atomic_bool stop{false};
	std::atomic_bool out{false};
	std::thread consumer([&] {
		q->WaitForWork(stop);
		out = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(!out.load());
	stop = true;
	q->Wake();
	consumer.join();
	CHECK(out.load());
	q->Free();
}

// --- GPU thread wake ----------------------------------------------------

static void TestGpuWake()
{
	for (int round = 0; round < 2000; round++)
	{
		std::atomic_bool ready{false};
		std::thread helper([&] {
			ready.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
		});
		LatteGpuWake_WaitUntil([&] { return ready.load(std::memory_order_acquire); });
		helper.join();
		CHECK(ready.load());
	}
	// Asleep past any spin when the predicate turns true
	{
		std::atomic_bool ready{false};
		std::atomic_bool done{false};
		std::thread waiter([&] {
			LatteGpuWake_WaitUntil([&] { return ready.load(std::memory_order_acquire); });
			done = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		ready.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
		waiter.join(); // a lost wake hangs here, and the runner's timeout says so
		CHECK(done.load());
	}
	// Already true: returns without waiting
	LatteGpuWake_WaitUntil([] { return true; });
}

static void TestGpuWakeDeadline()
{
	using clock = std::chrono::steady_clock;
	// Never true: back by the deadline, saying so
	{
		const auto start = clock::now();
		const bool r = LatteGpuWake_WaitUntil([] { return false; }, start + std::chrono::milliseconds(50));
		CHECK(!r);
		CHECK(clock::now() - start >= std::chrono::milliseconds(50));
	}
	// A deadline already gone: checks once and returns
	CHECK(!LatteGpuWake_WaitUntil([] { return false; }, clock::now() - std::chrono::seconds(1)));
	CHECK(LatteGpuWake_WaitUntil([] { return true; }, clock::now() - std::chrono::seconds(1)));
	// Made true long before a far deadline: woken by the notify, not the
	// deadline (the GX2Init wait's deadline is its next log line, seconds out)
	{
		std::atomic_bool ready{false};
		const auto start = clock::now();
		std::thread helper([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			ready.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
		});
		const bool r = LatteGpuWake_WaitUntil([&] { return ready.load(std::memory_order_acquire); },
			start + std::chrono::seconds(60));
		helper.join();
		CHECK(r);
		CHECK(clock::now() - start < std::chrono::seconds(30));
	}
}

// The boot as the core runs it with the GPU on a thread of its own: Latte_Start
// waits for the thread's init, the thread waits for the title to start, the
// title's init waits for the GPU's, the GPU waits for GX2Init - each a waiter
// with a predicate of its own on the one wake, and each step notified once.
static void TestGpuWakeBootHandoff()
{
	for (int round = 0; round < 1000; round++)
	{
		std::atomic_bool threadInit{false}, titleRunning{false}, gpuInitFinished{false}, gx2Init{false};
		std::thread gpu([&] {
			threadInit.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
			LatteGpuWake_WaitUntil([&] { return titleRunning.load(std::memory_order_acquire); });
			gpuInitFinished.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
			LatteGpuWake_WaitUntil([&] { return gx2Init.load(std::memory_order_acquire); },
				std::chrono::steady_clock::now() + std::chrono::seconds(5));
		});
		LatteGpuWake_WaitUntil([&] { return threadInit.load(std::memory_order_acquire); });
		titleRunning.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
		LatteGpuWake_WaitUntil([&] { return gpuInitFinished.load(std::memory_order_acquire); });
		gx2Init.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
		gpu.join(); // a lost wake hangs here, and the runner's timeout says so
		CHECK(gpuInitFinished.load());
	}
}

// The command ring's idle wait: the emulated CPU submits bursts with gaps
// between them (TCLSubmitToRing, then one notify), the GPU thread reads until
// the ring is empty, then sleeps until the next submission or its next vsync
// (LatteCP_waitForWork). Every word arrives in order - a lost wake would hold a
// burst back to the vsync deadline, which the elapsed time below would show -
// and the consumer goes round its loop a handful of times per burst, not the
// millions of times a spin does.
static void TestGpuRingIdleWait()
{
	constexpr uint32_t kSize = 4096;
	static std::atomic<uint32_t> ring[kSize];
	std::atomic<uint32_t> readIndex{0}, writeIndex{0};
	std::atomic_bool done{false};
	constexpr int kBursts = 60;
	constexpr int kWords = 100;

	std::thread cpu([&] {
		uint32_t value = 0;
		for (int b = 0; b < kBursts; b++)
		{
			uint32_t w = writeIndex.load(std::memory_order_relaxed);
			for (int i = 0; i < kWords; i++)
			{
				while (((w + 1) & (kSize - 1)) == readIndex.load(std::memory_order_acquire))
					std::this_thread::yield();
				ring[w].store(value++, std::memory_order_relaxed);
				w = (w + 1) & (kSize - 1);
			}
			writeIndex.store(w, std::memory_order_release);
			LatteGpuWake_Notify();
			std::this_thread::sleep_for(std::chrono::milliseconds(3)); // the gap
		}
		done.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
	});

	uint32_t expect = 0;
	uint64_t idleRounds = 0;
	const auto start = std::chrono::steady_clock::now();
	auto nextVSync = start + std::chrono::microseconds(16667);
	for (;;)
	{
		const uint32_t r = readIndex.load(std::memory_order_relaxed);
		if (r != writeIndex.load(std::memory_order_acquire))
		{
			CHECK(ring[r].load(std::memory_order_relaxed) == expect);
			expect++;
			readIndex.store((r + 1) & (kSize - 1), std::memory_order_release);
			continue;
		}
		if (done.load(std::memory_order_acquire) && r == writeIndex.load(std::memory_order_acquire))
			break;
		idleRounds++;
		if (std::chrono::steady_clock::now() >= nextVSync)
			nextVSync += std::chrono::microseconds(16667);
		LatteGpuWake_WaitUntil([&] {
			return readIndex.load(std::memory_order_relaxed) != writeIndex.load(std::memory_order_acquire) ||
				done.load(std::memory_order_acquire);
		}, nextVSync);
	}
	cpu.join();
	CHECK(expect == uint32_t(kBursts * kWords));
	// one wake per burst, one per vsync, and a few spurious ones to spare
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const uint64_t vsyncs = uint64_t(elapsed / std::chrono::microseconds(16667)) + 1;
	CHECK(idleRounds <= uint64_t(kBursts) * 4 + vsyncs * 2);
	if (idleRounds > uint64_t(kBursts) * 4 + vsyncs * 2)
		std::fprintf(stderr, "ring idle wait: %llu rounds for %d bursts\n", (unsigned long long)idleRounds, kBursts);
}

// --- EventWake ----------------------------------------------------------

static void TestEventWake()
{
	using clock = std::chrono::steady_clock;
	static ImmortalEventWake s_wake;
	EventWake& wake = s_wake.Get();

	// iosuIoctl_waitForWorkersToStop: the last worker out notifies, and the
	// wait ends then - long before its deadline - however the exits interleave
	for (int round = 0; round < 200; round++)
	{
		std::atomic<uint32_t> running{0};
		std::atomic_bool go{false};
		std::vector<std::thread> workers;
		for (int i = 0; i < 8; i++)
		{
			running.fetch_add(1);
			workers.emplace_back([&] {
				while (!go.load(std::memory_order_acquire))
					std::this_thread::yield();
				if (running.fetch_sub(1, std::memory_order_acq_rel) == 1)
					wake.Notify();
			});
		}
		go.store(true, std::memory_order_release);
		const auto start = clock::now();
		const bool stopped = wake.WaitUntil([&] { return running.load(std::memory_order_acquire) == 0; },
			start + std::chrono::seconds(30));
		CHECK(stopped);
		CHECK(clock::now() - start < std::chrono::seconds(10));
		for (auto& t : workers)
			t.join();
	}
	// A deadline with nothing coming: false, after it
	{
		const auto start = clock::now();
		CHECK(!wake.WaitUntil([] { return false; }, start + std::chrono::milliseconds(20)));
		CHECK(clock::now() - start >= std::chrono::milliseconds(20));
	}
	// The boss daemon: asleep with nothing to run, a task made ready wakes it,
	// a stop ends it - and the stop's own bounded wait for it to leave ends as
	// it leaves
	{
		std::atomic_bool running{true}, exited{false};
		std::atomic<int> ready{0}, ran{0};
		std::thread daemon([&] {
			while (running.load())
			{
				if (ready.load() > 0)
				{
					ready.fetch_sub(1);
					ran.fetch_add(1);
					wake.Notify(); // the task's state change
					continue;
				}
				wake.WaitUntil([&] { return !running.load() || ready.load() > 0; });
			}
			exited.store(true, std::memory_order_release);
			wake.Notify();
		});
		for (int i = 0; i < 100; i++)
		{
			ready.fetch_add(1);
			wake.Notify();
			const int want = i + 1;
			wake.WaitUntil([&] { return ran.load() == want; }); // HandleTaskWait
		}
		running.store(false);
		wake.Notify();
		const auto start = clock::now();
		CHECK(wake.WaitUntil([&] { return exited.load(std::memory_order_acquire); }, start + std::chrono::seconds(30)));
		CHECK(clock::now() - start < std::chrono::seconds(10));
		daemon.join();
		CHECK(ran.load() == 100);
	}
}

// --- log.txt ------------------------------------------------------------

static std::string LogTestLine(int thread, int seq)
{
	// long enough that a torn write would show
	std::string line = "T" + std::to_string(thread) + ":" + std::to_string(seq) + ":";
	line.append(200 + (seq % 50), char('a' + thread));
	line.push_back('\n');
	return line;
}

static bool LogTestLineWhole(const std::string& line)
{
	int thread, seq;
	if (std::sscanf(line.c_str(), "T%d:%d:", &thread, &seq) != 2)
		return false;
	return line + "\n" == LogTestLine(thread, seq);
}

static void TestLogFileSink()
{
	const auto path = std::filesystem::temp_directory_path() / "handoff_log_test.txt";
	constexpr int kThreads = 8, kLines = 2000;
	// All on: every line there, each whole
	{
		LogFileSink sink;
		CHECK(sink.Open(path));
		std::vector<std::thread> writers;
		for (int t = 0; t < kThreads; t++)
			writers.emplace_back([&sink, t] {
				for (int i = 0; i < kLines; i++)
				{
					const std::string line = LogTestLine(t, i);
					sink.Write(line.data(), line.size());
				}
			});
		for (auto& w : writers)
			w.join();
		sink.Close();
		std::ifstream in(path, std::ios::binary);
		std::string line;
		int count = 0;
		bool whole = true;
		while (std::getline(in, line))
		{
			whole = whole && LogTestLineWhole(line);
			count++;
		}
		CHECK(whole);
		CHECK(count == kThreads * kLines);
	}
	// Switched off and on while they write: no write lands on a closed handle
	// (TSan and the OS would say), and what is there is whole
	{
		LogFileSink sink;
		CHECK(sink.Open(path));
		std::atomic_bool stop{false};
		std::vector<std::thread> writers;
		for (int t = 0; t < kThreads; t++)
			writers.emplace_back([&sink, &stop, t] {
				for (int i = 0; !stop.load(); i++)
				{
					const std::string line = LogTestLine(t, i % 100000);
					sink.Write(line.data(), line.size());
				}
			});
		for (int round = 0; round < 200; round++)
		{
			sink.Close();
			CHECK(!sink.IsOpen());
			CHECK(sink.Open(path));
		}
		stop.store(true);
		for (auto& w : writers)
			w.join();
		sink.Close();
		std::ifstream in(path, std::ios::binary);
		std::string line;
		bool whole = true;
		while (std::getline(in, line))
			whole = whole && LogTestLineWhole(line);
		CHECK(whole);
	}
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

// --- input and rumble handoffs -------------------------------------------

struct SeqlockTestValue
{
	uint64_t a, b, c, d, e, f, g; // all equal, always
};

static void TestSeqlockValue()
{
	SeqlockValue<SeqlockTestValue> value;
	std::atomic_bool stop{false};
	std::atomic_bool torn{false};
	std::vector<std::thread> readers;
	for (int r = 0; r < 3; r++)
		readers.emplace_back([&] {
			uint64_t last = 0;
			while (!stop.load(std::memory_order_relaxed))
			{
				const SeqlockTestValue v = value.Load();
				if (v.a != v.b || v.a != v.c || v.a != v.d || v.a != v.e || v.a != v.f || v.a != v.g || v.a < last)
					torn.store(true);
				last = v.a;
			}
		});
	for (uint64_t i = 1; i <= 200000; i++)
		value.Store({i, i, i, i, i, i, i});
	stop.store(true);
	for (auto& t : readers)
		t.join();
	CHECK(!torn.load());
	CHECK(value.Load().a == 200000);
}

static void TestMpscList()
{
	MpscList<std::pair<int, int>> list;
	constexpr int kProducers = 4, kItems = 20000;
	std::atomic<int> producersDone{0};
	std::vector<std::thread> producers;
	for (int p = 0; p < kProducers; p++)
		producers.emplace_back([&, p] {
			for (int i = 0; i < kItems; i++)
				list.Push({p, i});
			producersDone.fetch_add(1);
		});
	int next[kProducers] = {};
	bool ordered = true;
	int total = 0;
	for (;;)
	{
		const bool done = producersDone.load() == kProducers;
		list.TakeAll([&](std::pair<int, int>&& item) {
			ordered = ordered && item.second == next[item.first];
			next[item.first] = item.second + 1;
			total++;
		});
		if (done)
			break;
		std::this_thread::yield();
	}
	for (auto& t : producers)
		t.join();
	list.TakeAll([&](std::pair<int, int>&&) { total++; });
	CHECK(ordered);
	CHECK(total == kProducers * kItems);
}

// Two fake frontends' interfaces: each counts its own lock_queue/unlock_queue
struct FakeVkFrontend
{
	std::atomic<int> held{0};
	std::atomic<bool> bad{false};
	retro_hw_render_interface_vulkan iface{};
};

static void FakeLockQueue(void* handle)
{
	auto* f = static_cast<FakeVkFrontend*>(handle);
	if (f->held.fetch_add(1) != 0)
		f->bad.store(true); // one submitting thread: never held twice
}

static void FakeUnlockQueue(void* handle)
{
	auto* f = static_cast<FakeVkFrontend*>(handle);
	if (f->held.fetch_sub(1) != 1)
		f->bad.store(true); // released through one it was not taken through
}

static void TestVkQueueLock()
{
	FakeVkFrontend a, b;
	for (FakeVkFrontend* f : {&a, &b})
	{
		f->iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
		f->iface.handle = f;
		f->iface.lock_queue = FakeLockQueue;
		f->iface.unlock_queue = FakeUnlockQueue;
	}
	LibretroVkQueue::SetInterface(&a.iface);
	std::atomic_bool stop{false};
	std::atomic<uint64_t> submits{0};
	std::thread gpu([&] {
		while (!stop.load(std::memory_order_relaxed))
		{
			LibretroVkQueue::Lock lock;
			submits.fetch_add(1, std::memory_order_relaxed);
		}
	});
	// The frontend, each frame: usually the same interface (no wait at all),
	// now and then a rebuilt one, or none
	// (started once the submitting thread is: the swaps are the point, and
	// 20000 of them can be over before a new thread has run at all)
	while (submits.load(std::memory_order_relaxed) == 0)
		std::this_thread::yield();
	const void* sequence[] = {&a.iface, &a.iface, &a.iface, &b.iface, &b.iface, nullptr, &a.iface};
	for (int frame = 0; frame < 20000; frame++)
		LibretroVkQueue::SetInterface(sequence[frame % 7]);
	stop.store(true);
	gpu.join();
	LibretroVkQueue::SetInterface(nullptr);
	CHECK(!a.bad.load() && !b.bad.load());
	CHECK(a.held.load() == 0 && b.held.load() == 0);
	CHECK(submits.load() > 0);
}

// --- audio devices' gate ------------------------------------------------

struct FakeAudioDevice
{
	std::atomic<uint64_t> fed{0};
	uint64_t canary = 0x5ca1ab1e;
};

static void TestAudioDevicesGate()
{
	// A plain pointer, as g_tvAudio is: what the gate protects is exactly that
	// it is never read while it is being replaced
	static FakeAudioDevice* device = nullptr;
	std::atomic_bool stop{false}, bad{false};
	std::atomic<uint64_t> fed{0}, skipped{0};
	std::vector<std::thread> players;
	for (int p = 0; p < 3; p++)
		players.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed))
			{
				AudioDevicesUse devices;
				if (!devices)
				{
					skipped.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				if (device)
				{
					if (device->canary != 0x5ca1ab1e)
						bad.store(true);
					device->fed.fetch_add(1, std::memory_order_relaxed);
					fed.fetch_add(1, std::memory_order_relaxed);
				}
			}
		});
	// Titles starting and stopping, and a portal coming up alongside
	std::thread portal([&] {
		for (int i = 0; i < 300 && !stop.load(); i++)
		{
			AudioDevicesChange change;
			if (!device)
				device = new FakeAudioDevice();
		}
	});
	for (int round = 0; round < 1000; round++)
	{
		{
			AudioDevicesChange change;
			if (!device)
				device = new FakeAudioDevice();
		}
		std::this_thread::yield();
		{
			AudioDevicesChange change;
			if (device)
			{
				device->canary = 0;
				delete device;
				device = nullptr;
			}
		}
	}
	portal.join();
	stop.store(true);
	for (auto& t : players)
		t.join();
	{
		AudioDevicesChange change;
		delete device;
		device = nullptr;
	}
	CHECK(!bad.load());
	CHECK(fed.load() > 0);
}

// --- data cache flush pages -------------------------------------------------

static void TestAtomicPageSet()
{
	constexpr size_t kPages = 1 << 16;
	AtomicPageSet<kPages> set;
	// One page at a time, handed over: added, then the drain that starts
	// after must report it unless one before already did
	{
		std::atomic<uint32_t> added{UINT32_MAX};
		std::atomic_bool stop{false};
		bool missed = false;
		std::thread producer([&] {
			for (uint32_t i = 0; i < 20000; i++)
			{
				const uint32_t page = (i * 2654435761u) % kPages;
				set.Add(page, page);
				added.store(page, std::memory_order_release);
				while (added.load(std::memory_order_acquire) != UINT32_MAX && !stop.load())
					std::this_thread::yield();
			}
			stop.store(true);
		});
		while (!stop.load())
		{
			const uint32_t page = added.load(std::memory_order_acquire);
			if (page == UINT32_MAX)
			{
				std::this_thread::yield();
				continue;
			}
			bool seen = false;
			set.Drain([&](uint32_t p) { seen = seen || p == page; });
			missed = missed || !seen;
			added.store(UINT32_MAX, std::memory_order_release);
		}
		producer.join();
		CHECK(!missed);
	}
	// Ranges from several threads while it is drained: everything added is
	// reported by the time all of it is drained, nothing that was not
	{
		std::vector<std::atomic<uint8_t>> expected(kPages);
		std::vector<std::atomic<uint8_t>> got(kPages);
		std::atomic<int> done{0};
		std::vector<std::thread> producers;
		for (int t = 0; t < 4; t++)
			producers.emplace_back([&, t] {
				uint32_t x = 12345 + t;
				for (int i = 0; i < 20000; i++)
				{
					x = x * 1103515245u + 12345u;
					const uint32_t first = (x >> 8) % kPages;
					const uint32_t last = std::min<uint32_t>(first + (x % 200), kPages - 1);
					for (uint32_t p = first; p <= last; p++)
						expected[p].store(1, std::memory_order_relaxed);
					set.Add(first, last);
				}
				done.fetch_add(1);
			});
		auto drain = [&] { set.Drain([&](uint32_t p) { got[p].store(1, std::memory_order_relaxed); }); };
		while (done.load() != 4)
			drain();
		for (auto& t : producers)
			t.join();
		drain();
		bool same = true;
		for (size_t p = 0; p < kPages; p++)
			same = same && expected[p].load() == got[p].load();
		CHECK(same);
		CHECK(set.MaybeEmpty());
	}
}

// --- guest time base -----------------------------------------------------

static void TestPPCTimerClock()
{
	PPCTimerClock clock;
	std::atomic<uint64_t> tsc{1000000};
	std::atomic<uint64_t> freq{0};
	std::atomic<int> shift{3};
	clock.Restart(tsc.load());
	// No rate yet: time stands still at 0
	CHECK(clock.Read(tsc.fetch_add(1000), 0, 3) == 0);
	// Rate: 1 TSC tick per console tick at shift 3 (x1)
	freq.store(PPCTimerClock::kConsoleClock);
	const uint64_t t0 = clock.Read(tsc.load(), freq.load(), 3);
	tsc.fetch_add(5000);
	const uint64_t t1 = clock.Read(tsc.load(), freq.load(), 3);
	CHECK(t1 - t0 == 5000);
	// Shift 2 (x2): twice the rate from here, nothing already counted moves
	const uint64_t t2 = clock.Read(tsc.load(), freq.load(), 2);
	CHECK(t2 == t1);
	tsc.fetch_add(5000);
	CHECK(clock.Read(tsc.load(), freq.load(), 2) - t2 == 10000);
	shift.store(2);

	// Readers on four threads, the TSC moving, the rate refined and the shift
	// changed under them: every thread's reads ascend, and no read is behind
	// one any thread finished before it started
	std::atomic_bool stop{false}, bad{false};
	std::atomic<uint64_t> finished{0}; // the highest value a read has returned
	std::vector<std::thread> readers;
	for (int r = 0; r < 4; r++)
		readers.emplace_back([&] {
			uint64_t mine = 0;
			while (!stop.load(std::memory_order_relaxed))
			{
				const uint64_t floor = finished.load(std::memory_order_acquire);
				const uint64_t v = clock.Read(tsc.fetch_add(3, std::memory_order_relaxed), freq.load(), (uint8_t)shift.load());
				if (v < mine || v < floor)
					bad.store(true);
				mine = v;
				uint64_t f = finished.load(std::memory_order_relaxed);
				while (v > f && !finished.compare_exchange_weak(f, v, std::memory_order_release))
					;
			}
		});
	for (int i = 0; i < 200; i++)
	{
		freq.store(PPCTimerClock::kConsoleClock + (i % 7) * 1000);
		shift.store(i % 4);
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	stop.store(true);
	for (auto& t : readers)
		t.join();
	CHECK(!bad.load());
}

// --- pipeline dependency bags ---------------------------------------------

struct FakePipeline;
static AtomicPtrBag<FakePipeline>* s_fakeShaders[3];
static std::atomic<int> s_fakePipelinesAlive{0};

struct FakePipeline
{
	FakePipeline()
	{
		s_fakePipelinesAlive.fetch_add(1);
		for (auto* shader : s_fakeShaders)
			shader->Add(this);
	}
	~FakePipeline()
	{
		for (auto* shader : s_fakeShaders)
			shader->Remove(this);
		s_fakePipelinesAlive.fetch_sub(1);
	}
};

static void TestAtomicPtrBag()
{
	for (auto*& shader : s_fakeShaders)
		shader = new AtomicPtrBag<FakePipeline>();
	// Compile threads loading the cache: build a pipeline, keep some, drop
	// the rest, all on the same three shaders
	std::vector<std::vector<FakePipeline*>> kept(6);
	std::vector<std::thread> compilers;
	for (int t = 0; t < 6; t++)
		compilers.emplace_back([&, t] {
			for (int i = 0; i < 3000; i++)
			{
				FakePipeline* p = new FakePipeline();
				if (i % 3 == 0)
					kept[t].push_back(p);
				else
					delete p;
			}
		});
	for (auto& t : compilers)
		t.join();
	size_t keptCount = 0;
	for (auto& k : kept)
		keptCount += k.size();
	CHECK(s_fakePipelinesAlive.load() == (int)keptCount);
	// Each bag holds exactly the kept ones
	for (auto* shader : s_fakeShaders)
	{
		size_t n = 0;
		// counted by taking and putting back, on one thread
		std::vector<FakePipeline*> seen;
		shader->ForEachTaken([&](FakePipeline* p) { seen.push_back(p); n++; });
		for (auto* p : seen)
			shader->Add(p);
		CHECK(n == keptCount);
	}
	// The first shader goes: it deletes every pipeline still using it, each
	// of which takes itself out of all three as it goes
	int deleted = 0;
	s_fakeShaders[0]->ForEachTaken([&](FakePipeline* p) { delete p; deleted++; });
	CHECK(deleted == (int)keptCount);
	CHECK(s_fakePipelinesAlive.load() == 0);
	for (auto* shader : s_fakeShaders)
		CHECK(shader->Empty());
	for (auto*& shader : s_fakeShaders)
	{
		delete shader;
		shader = nullptr;
	}
}

// --- worker queues ----------------------------------------------------------

static void TestMpscQueue()
{
	MpscQueue<std::pair<int, int>> queue;
	constexpr int kProducers = 4, kItems = 20000;
	std::vector<std::thread> producers;
	for (int p = 0; p < kProducers; p++)
		producers.emplace_back([&, p] {
			for (int i = 0; i < kItems; i++)
			{
				queue.push({p, i});
				if (i % 1000 == 0)
					std::this_thread::sleep_for(std::chrono::microseconds(100)); // let the consumer catch up and sleep
			}
		});
	int next[kProducers] = {};
	bool ordered = true;
	for (int n = 0; n < kProducers * kItems; n++)
	{
		const auto item = queue.pop(); // asleep whenever it catches up
		ordered = ordered && item.second == next[item.first];
		next[item.first] = item.second + 1;
	}
	for (auto& t : producers)
		t.join();
	std::pair<int, int> extra;
	CHECK(ordered);
	CHECK(!queue.peek2(extra));
	CHECK(queue.empty());
	// pop_until (the IOSU ioctl queues): back at the deadline with nothing,
	// at once with something - pushed from another thread mid-wait
	{
		using clock = std::chrono::steady_clock;
		auto start = clock::now();
		CHECK(!queue.pop_until(extra, start + std::chrono::milliseconds(30)));
		CHECK(clock::now() - start >= std::chrono::milliseconds(25));
		std::thread t([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			queue.push({7, 7});
		});
		start = clock::now();
		CHECK(queue.pop_until(extra, start + std::chrono::seconds(30)));
		t.join();
		CHECK(extra.first == 7 && clock::now() - start < std::chrono::seconds(10));
	}
}

static void TestMpmcQueue()
{
	// Several consumers, items pushed both while they keep up and in bursts
	// past the ring, so the overflow is handed back to them
	MpmcQueue<int> queue;
	constexpr int kConsumers = 4, kItems = 200000;
	std::vector<std::atomic<uint8_t>> seen(kItems);
	std::atomic<int> taken{0};
	std::vector<std::thread> consumers;
	for (int c = 0; c < kConsumers; c++)
		consumers.emplace_back([&] {
			for (;;)
			{
				const int v = queue.pop();
				if (v < 0)
					return; // one stop item per consumer
				if (seen[v].fetch_add(1) != 0)
					taken.store(-1000000); // taken twice
				taken.fetch_add(1);
			}
		});
	std::thread producer2([&] {
		for (int i = 1; i < kItems; i += 2)
			queue.push(i);
	});
	for (int i = 0; i < kItems; i += 2)
	{
		queue.push(i);
		if (i % 20000 == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	producer2.join();
	while (taken.load() >= 0 && taken.load() < kItems)
		std::this_thread::yield();
	for (int c = 0; c < kConsumers; c++)
		queue.push(-1);
	for (auto& t : consumers)
		t.join();
	CHECK(taken.load() == kItems);
	bool once = true;
	for (auto& s : seen)
		once = once && s.load() == 1;
	CHECK(once);
	// In order while it fits the ring, one consumer
	MpmcQueue<int> ordered;
	for (int i = 0; i < 1000; i++)
		ordered.push(i);
	bool inOrder = true;
	for (int i = 0; i < 1000; i++)
		inOrder = inOrder && ordered.pop() == i;
	CHECK(inOrder);
	// Past the ring with nobody taking: all still there
	MpmcQueue<int> burst;
	for (int i = 0; i < 5000; i++)
		burst.push(i);
	CHECK(burst.size() == 5000);
	std::vector<uint8_t> got(5000);
	int v, n = 0;
	while (burst.peek2(v))
	{
		got[v]++;
		n++;
	}
	CHECK(n == 5000);
	bool all = true;
	for (auto g : got)
		all = all && g == 1;
	CHECK(all);
}

// --- shader compile pool --------------------------------------------------

struct FakeShader
{
	ClaimTicket<FakeShader>* ticket;
	std::atomic<int> state{0}; // 0 queued, 1 compiling, 2 done
	std::atomic<int> compiles{0};
	uint64_t canary = 0xC0FFEE;
};

static void TestClaimTicket()
{
	static ImmortalEventWake s_done;
	MpmcQueue<ClaimTicket<FakeShader>*> queue;
	std::atomic_bool bad{false};
	auto compile = [&](FakeShader* sh) {
		if (sh->canary != 0xC0FFEE)
			bad.store(true); // a destroyed one
		sh->state.store(1);
		sh->compiles.fetch_add(1);
		sh->state.store(2, std::memory_order_release);
		s_done->Notify();
	};
	auto waitDone = [&](FakeShader* sh) {
		s_done->WaitUntil([sh] { return sh->state.load(std::memory_order_acquire) == 2; });
	};
	std::vector<std::thread> pool;
	for (int t = 0; t < 2; t++)
		pool.emplace_back([&] {
			for (;;)
			{
				auto* ticket = queue.pop();
				if (!ticket)
					return;
				FakeShader* sh = ticket->Claim();
				ticket->Release();
				if (sh)
					compile(sh);
			}
		});
	int compiled = 0;
	for (int i = 0; i < 30000; i++)
	{
		FakeShader* sh = new FakeShader();
		sh->ticket = new ClaimTicket<FakeShader>(sh);
		queue.push(sh->ticket);
		const int what = i % 3;
		if (what == 1)
		{
			// PreponeCompilation: compiled here, or waited for
			if (FakeShader* mine = sh->ticket->Claim())
				compile(mine);
			else
				waitDone(sh);
		}
		// the destructor (every one, now or right after the prepone)
		if (what != 2 || (i % 6) == 2)
		{
			// as ~RendererShaderVk: taken by someone else means compiled or
			// being compiled - and the destructor waits for it
			if (sh->ticket->Claim() == nullptr)
				waitDone(sh);
			if (sh->compiles.load() > 1)
				bad.store(true);
			compiled += sh->compiles.load();
			sh->ticket->Release();
			sh->canary = 0;
			delete sh;
		}
		else
		{
			// left alive: some compile thread gets to it
			waitDone(sh);
			if (sh->compiles.load() != 1)
				bad.store(true);
			compiled++;
			if (sh->ticket->Claim() != nullptr)
				bad.store(true);
			sh->ticket->Release();
			sh->canary = 0;
			delete sh;
		}
	}
	for (size_t t = 0; t < pool.size(); t++)
		queue.push(nullptr);
	for (auto& t : pool)
		t.join();
	CHECK(!bad.load());
	CHECK(compiled > 0);
}

// --- nex waits ------------------------------------------------------------

static void TestNexWait()
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	using clock = std::chrono::steady_clock;
	nexWait::Wake wake;
	CHECK(wake.sock != nexWait::kNoSocket);
	// Nothing: the timeout
	{
		const auto start = clock::now();
		CHECK(!nexWait::WaitForSockets(nullptr, 0, 50, &wake));
		CHECK(clock::now() - start >= std::chrono::milliseconds(40));
	}
	// A wake from another thread ends a long wait at once
	for (int i = 0; i < 20; i++)
	{
		std::thread t([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			wake.Signal();
		});
		const auto start = clock::now();
		CHECK(nexWait::WaitForSockets(nullptr, 0, 30000, &wake));
		t.join();
		CHECK(clock::now() - start < std::chrono::seconds(10));
	}
	// Drained: the next wait is not ended by an old wake
	wake.Signal();
	wake.Signal();
	CHECK(nexWait::WaitForSockets(nullptr, 0, 1000, &wake));
	CHECK(!nexWait::WaitForSockets(nullptr, 0, 20, &wake));
	// A packet on a service's socket ends it too, without the wake
	{
		nexWait::Wake service; // a loopback socket like any other
		std::thread t([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			service.Signal();
		});
		const auto start = clock::now();
		CHECK(nexWait::WaitForSockets(&service.sock, 1, 30000, nullptr));
		t.join();
		CHECK(clock::now() - start < std::chrono::seconds(10));
	}
}

// --- RcuCell ------------------------------------------------------------------

struct RcuTestValue
{
	std::vector<uint64_t> items;
	uint64_t canary = 0x600DF00D;
	RcuTestValue() = default;
	explicit RcuTestValue(uint64_t n) : items(16, n) {}
	RcuTestValue(const RcuTestValue&) = default;
	~RcuTestValue() { canary = 0; }
};

static void TestRcuCell()
{
	RcuCell<RcuTestValue> cell(RcuTestValue(0));
	std::atomic_bool stop{false}, bad{false};
	std::atomic<uint64_t> reads{0};
	std::vector<std::thread> readers;
	for (int r = 0; r < 4; r++)
		readers.emplace_back([&] {
			uint64_t last = 0;
			while (!stop.load(std::memory_order_relaxed))
			{
				const auto v = cell.Read();
				if (v->canary != 0x600DF00D || v->items.size() != 16)
				{
					bad.store(true);
					continue;
				}
				const uint64_t n = v->items[0];
				for (uint64_t x : v->items)
					if (x != n)
						bad.store(true);
				if (n < last)
					bad.store(true); // went back to a replaced value
				last = n;
				reads.fetch_add(1, std::memory_order_relaxed);
			}
		});
	// Writers, two taking turns, while the readers never stop coming
	std::thread writer2([&] {
		for (uint64_t i = 1; i <= 2000; i++)
			cell.Write([] { return RcuTestValue(0); });
	});
	writer2.join();
	for (uint64_t i = 1; i <= 5000; i++)
		cell.Write([i] { return RcuTestValue(i); });
	stop.store(true);
	for (auto& t : readers)
		t.join();
	CHECK(!bad.load());
	CHECK(reads.load() > 0);
	CHECK(cell.Read()->items[0] == 5000);
}

// --- thread-started flag ------------------------------------------------

static void TestThreadStartedFlag()
{
	ThreadStartedFlag flag;
	CHECK(!flag.IsSet());
	// What the starter of a service does, again and again. With the 10 ms
	// sleep it replaced this took 3 s at the very least; woken by the thread
	// it is a few thread starts.
	const auto start = std::chrono::steady_clock::now();
	for (int round = 0; round < 300; round++)
	{
		std::atomic_bool stop{false};
		flag.Reset();
		std::thread service([&] {
			flag.Set();
			while (!stop.load(std::memory_order_acquire))
				std::this_thread::yield();
			flag.Clear();
		});
		flag.Wait();
		CHECK(flag.IsSet());
		stop.store(true, std::memory_order_release);
		service.join();
		CHECK(!flag.IsSet());
	}
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
	// Set before the wait: returns without waiting
	flag.Set();
	flag.Wait();
	CHECK(flag.IsSet());
}

// --- audio ring ---------------------------------------------------------

static void TestAudioRingSpans(unsigned channels)
{
	auto ring = std::make_unique<LibretroAudioRingBuffer>();
	int16_t next = 0, expect = 0;
	// Many laps of the ring in uneven pieces: every span is whole frames,
	// and what comes out is what went in
	for (int round = 0; round < 4000; round++)
	{
		const size_t frames = 1 + (round * 7919) % 3000;
		size_t want = frames * channels;
		while (want)
		{
			int16_t* dst;
			const size_t n = ring->Reserve(&dst, want);
			if (n == 0)
				break;
			CHECK(n % channels == 0);
			for (size_t i = 0; i < n; i++)
				dst[i] = next++;
			ring->Commit(n);
			want -= n;
		}
		size_t drain = ((round % 3) == 0) ? ring->GetReadAvailableSamples() : frames * channels / 2;
		drain -= drain % channels;
		while (drain)
		{
			const int16_t* src;
			const size_t n = ring->Peek(&src, drain);
			if (n == 0)
				break;
			CHECK(n % channels == 0);
			for (size_t i = 0; i < n; i++)
				if (src[i] != expect++)
				{
					s_failures++;
					std::fprintf(stderr, "FAIL audio ring: sample out of order (%u channels)\n", channels);
					return;
				}
			ring->Consume(n);
			drain -= n;
		}
	}
	// Never more than the ring less one frame
	int16_t* dst;
	ring->Reset();
	size_t total = 0;
	for (;;)
	{
		const size_t n = ring->Reserve(&dst, LibretroAudioRingBuffer::kBufferSamples);
		if (!n)
			break;
		ring->Commit(n);
		total += n;
	}
	CHECK(total == LibretroAudioRingBuffer::kBufferSamples - LibretroAudioRingBuffer::kChannels);
}

static void TestAudioRingThreads()
{
	auto ring = std::make_unique<LibretroAudioRingBuffer>();
	constexpr size_t kTotal = 8u * 1024 * 1024;
	std::atomic_bool bad{false};
	std::thread producer([&] {
		int16_t v = 0;
		size_t done = 0;
		while (done < kTotal)
		{
			int16_t* dst;
			size_t n = ring->Reserve(&dst, std::min<size_t>(kTotal - done, 960 * 2));
			n -= n % 2;
			if (!n)
			{
				std::this_thread::yield();
				continue;
			}
			for (size_t i = 0; i < n; i++)
				dst[i] = v++;
			ring->Commit(n);
			done += n;
		}
	});
	int16_t expect = 0;
	size_t got = 0;
	while (got < kTotal)
	{
		const int16_t* src;
		const size_t n = ring->Peek(&src, 1600);
		if (!n)
		{
			std::this_thread::yield();
			continue;
		}
		for (size_t i = 0; i < n; i++)
			if (src[i] != expect++)
				bad = true;
		ring->Consume(n);
		got += n;
	}
	producer.join();
	CHECK(!bad.load());
	CHECK(got == kTotal);
}

// The Audio Thread's pull with nothing to give: LibretroAudioAPI::WaitForFeed
// as libretro_audio_thread_pull calls it, on the ring and wake the core uses.
static void TestAudioFeedWait()
{
	using clock = std::chrono::steady_clock;
	auto ring = std::make_unique<LibretroAudioRingBuffer>();
	LibretroAudioFeedWake wake;
	std::atomic_bool active{true};
	const auto ready = [&] {
		return !active.load(std::memory_order_relaxed) || ring->GetReadAvailableSamples() != 0;
	};

	// Nothing comes: back at the bound, empty-handed
	{
		const auto start = clock::now();
		CHECK(!wake.WaitUntil(ready, start + std::chrono::milliseconds(20), false));
		CHECK(clock::now() - start >= std::chrono::milliseconds(20));
	}
	// A block arrives well inside a long bound: the wait ends with it, every
	// time - a fixed sleep would end at the bound instead
	for (int round = 0; round < 50; round++)
	{
		std::thread producer([&] {
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			int16_t block[480 * 2] = {};
			ring->Write(block, 480 * 2);
			wake.Notify();
		});
		const auto start = clock::now();
		const bool got = wake.WaitUntil(ready, start + std::chrono::seconds(30), false);
		producer.join();
		CHECK(got);
		CHECK(clock::now() - start < std::chrono::seconds(10));
		ring->Discard(ring->GetReadAvailableSamples());
	}
	// The frontend stops its audio thread: set_state(false) clears active and
	// wakes the wait, which does not run on to the bound
	{
		std::thread state([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			active.store(false, std::memory_order_relaxed);
			wake.Notify();
		});
		const auto start = clock::now();
		CHECK(wake.WaitUntil(ready, start + std::chrono::seconds(30), false));
		state.join();
		CHECK(clock::now() - start < std::chrono::seconds(10));
		active.store(true, std::memory_order_relaxed);
	}
	// A refused pull: any wake ends it, ready or not
	{
		std::thread other([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			wake.Notify();
		});
		const auto start = clock::now();
		CHECK(!wake.WaitUntil([] { return false; }, start + std::chrono::seconds(30), true));
		other.join();
		CHECK(clock::now() - start < std::chrono::seconds(10));
	}
}

int main()
{
	TestWorkQueueStream();
	TestWorkQueueFullAndStop();
	TestGpuWake();
	TestGpuWakeDeadline();
	TestGpuWakeBootHandoff();
	TestGpuRingIdleWait();
	TestThreadStartedFlag();
	TestEventWake();
	TestLogFileSink();
	TestSeqlockValue();
	TestMpscList();
	TestVkQueueLock();
	TestAudioDevicesGate();
	TestAtomicPageSet();
	TestPPCTimerClock();
	TestAtomicPtrBag();
	TestMpscQueue();
	TestMpmcQueue();
	TestClaimTicket();
	TestNexWait();
	TestRcuCell();
	TestAudioRingSpans(2);
	TestAudioRingSpans(6);
	TestAudioRingThreads();
	TestAudioFeedWait();
	if (s_failures)
	{
		std::fprintf(stderr, "%d check(s) failed\n", s_failures);
		return 1;
	}
	std::printf("handoff: all passed\n");
	return 0;
}
