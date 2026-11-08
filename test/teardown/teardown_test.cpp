// What the core stops before the frontend unloads it. A thread of the core
// still running once retro_deinit has returned is left in code the frontend
// then unmaps, and the first one to wake crashes the frontend (a DEP
// violation on Windows: RetroArch went down a few seconds after a title
// failed to start).
//  - the TSC rate measurement (PPCTimer.cpp), which ran for three seconds
//    after the first retro_run on a detached thread: PPCTimer_shutdown ends
//    it at once and joins it, keeps the rate measured so far (if long enough
//    to mean anything), and PPCTimer_init starts it again.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "Cafe/HW/Espresso/PPCState.h"

bool PPCTimer_isReady();
void PPCTimer_shutdown();

static int s_failures = 0;
#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			s_failures++; \
		} \
	} while (0)

static void TestTimerCalibration()
{
	using namespace std::chrono;
	// stopped right after it started: back at once, not after three seconds
	auto t0 = steady_clock::now();
	PPCTimer_init();
	PPCTimer_shutdown();
	CHECK(steady_clock::now() - t0 < milliseconds(500));
	// a second stop has nothing to do
	PPCTimer_shutdown();
	// a stop that soon keeps nothing: a rate measured over microseconds is
	// noise, and the next start measures it
	CHECK(!PPCTimer_isReady());
	// started again, stopped after its first estimate (25 ms in): still
	// ready, back at once
	t0 = steady_clock::now();
	PPCTimer_init();
	PPCTimer_waitForInit();
	CHECK(steady_clock::now() - t0 >= milliseconds(20));
	CHECK(PPCTimer_isReady());
	t0 = steady_clock::now();
	PPCTimer_shutdown();
	CHECK(steady_clock::now() - t0 < milliseconds(500));
	CHECK(PPCTimer_isReady());
	// the rate it kept is a TSC rate (between 100 MHz and 10 GHz)
	const uint64 perSecond = PPCTimer_microsecondsToTsc(1000000);
	CHECK(perSecond > 100000000ull && perSecond < 10000000000ull);
	// started and stopped from several threads in turn, many times over
	for (int i = 0; i < 50; i++)
	{
		std::thread t([] {
			PPCTimer_init();
			PPCTimer_shutdown();
		});
		t.join();
	}
	CHECK(PPCTimer_isReady());
}

int main()
{
	TestTimerCalibration();
	if (s_failures)
	{
		std::printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	std::printf("teardown: all passed\n");
	return 0;
}
