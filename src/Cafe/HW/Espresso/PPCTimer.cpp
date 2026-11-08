#include "Cafe/HW/Espresso/Const.h"
#include "config/ActiveSettings.h"
#include "util/helpers/SeqlockValue.h"
#include "Cafe/HW/Espresso/PPCTimerClock.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Common/cpu_features.h"

#include <atomic>
#include <thread>
#include "util/helpers/EventWake.h"

#if defined(ARCH_X86_64)
#include <immintrin.h>
#pragma intrinsic(__rdtsc)
#endif

// Published by the calibration thread, first after 25 ms and then twice more
// as it gets better (PPCTimer_initThread); taken up by the next guest time
// read (PPCTimerClock.h)
static std::atomic<uint64> _rdtscFrequency{0};
// The title start waits on this for the first estimate (PPCTimer_waitForInit),
// and the calibration thread for its next measurement or a stop
static ImmortalEventWake sTimerReady;
// The calibration thread, joined by PPCTimer_shutdown. It was detached: a
// frontend that unloaded the core within its three seconds - a title that
// failed to start, say - left it asleep in code that was then unmapped, and
// it crashed the frontend when it woke (DEP violation on Windows).
static std::thread sCalibrationThread;
static std::atomic<bool> sCalibrationStop{false};
static PPCTimerClock sGuestClock;
static_assert(PPCTimerClock::kConsoleClock == Espresso::CORE_CLOCK);

struct uint128_t
{
	uint64 low;
	uint64 high;
};

static_assert(sizeof(uint128_t) == 16);

uint64 muldiv64(uint64 a, uint64 b, uint64 d)
{
	uint64 diva = a / d;
	uint64 moda = a % d;
	uint64 divb = b / d;
	uint64 modb = b % d;
	return diva * b + moda * divb + moda * modb / d;
}

// The TSC's rate, measured against the high resolution timer. Upstream
// measured once, for three seconds, and the title start waited for it - three
// seconds off every boot that came sooner than that after the core was loaded,
// which in RetroArch is every boot. The two clocks are read back to back, so
// the error is the gap between those reads (well under a microsecond) over the
// time measured: 25 ms is already good to about one part in 10^5. So the first
// estimate is published after 25 ms, and the same measurement, still running,
// replaces it at 200 ms and at 3 s with the one upstream would have had. The
// rate is only ever a divisor of elapsed TSC ticks (PPCTimer_getFromRDTSC), so
// a refined one changes how fast guest time runs by that one part in 10^5 from
// then on, and moves no time already counted.
int PPCTimer_initThread()
{
    #if defined(ARCH_X86_64)
	if (!g_CPUFeatures.x86.invariant_tsc)
		cemuLog_log(LogType::Force, "Invariant TSC not supported");
    #endif

	_mm_mfence();
	const uint64 tscStart = __rdtsc();
	const HRTick startTick = HighResolutionTimer::now().getTick();
	const auto start = std::chrono::steady_clock::now();
	for (const int ms : { 25, 200, 3000 })
	{
		// A stop ends the wait. What was measured until then is still
		// published if it is long enough to mean something (a few
		// milliseconds); otherwise the rate stays as it was, and the next
		// PPCTimer_init measures it again
		const bool stopping = sTimerReady->WaitUntil([] { return sCalibrationStop.load(std::memory_order_acquire); },
			start + std::chrono::milliseconds(ms));
		if (stopping && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5))
			break;
		_mm_mfence();
		const HRTick stopTick = HighResolutionTimer::now().getTick();
		const uint64 tscEnd = __rdtsc();
		uint64 hrtFreq = 0;
		const uint64 hrtDiff = HighResolutionTimer::getTimeDiffEx(startTick, stopTick, hrtFreq);
		if (hrtDiff == 0)
			continue;
		const uint64 tscFreq = muldiv64(tscEnd - tscStart, hrtFreq, hrtDiff);
		if (tscFreq == 0)
			continue;
		_rdtscFrequency.store(tscFreq, std::memory_order_release);
		sTimerReady->Notify();
		if (stopping)
			break;
	}
	return 0;
}

void PPCTimer_init()
{
	if (!sCalibrationThread.joinable())
	{
		sCalibrationStop.store(false, std::memory_order_release);
		sCalibrationThread = std::thread(PPCTimer_initThread);
	}
	sGuestClock.Restart(__rdtsc());
}

// Ends the measurement PPCTimer_init started, keeping what it measured if
// that is long enough to mean anything; before the library is unloaded
// (retro_deinit). PPCTimer_init starts it again.
void PPCTimer_shutdown()
{
	if (!sCalibrationThread.joinable())
		return;
	sCalibrationStop.store(true, std::memory_order_release);
	sTimerReady->Notify();
	sCalibrationThread.join();
}

void PPCTimer_start()
{
	sGuestClock.Restart(__rdtsc());
}

uint64 PPCTimer_getRawTsc()
{
	return __rdtsc();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return (us * _rdtscFrequency.load(std::memory_order_relaxed)) / 1000000ULL;
}

uint64 PPCTimer_tscToMicroseconds(uint64 us)
{
	uint128_t r{};
	r.low = _umul128(us, 1000000ULL, &r.high);

	uint64 remainder;
	const uint64 microseconds = _udiv128(r.high, r.low, _rdtscFrequency.load(std::memory_order_relaxed), &remainder);

	return microseconds;
}

bool PPCTimer_isReady()
{
	return _rdtscFrequency.load(std::memory_order_acquire) != 0;
}

void PPCTimer_waitForInit()
{
	// Asleep until the first estimate, 25 ms after PPCTimer_init at most
	sTimerReady->WaitUntil([] { return PPCTimer_isReady(); });
}

// thread safe, and without a lock: every guest time read on every core comes
// here (OSGetTime, the scheduler's alarms, mftb), and it was a spinlock all of
// them took (PPCTimerClock.h)
uint64 PPCTimer_getFromRDTSC()
{
	_mm_mfence();
	const uint64 tsc = __rdtsc();
	return sGuestClock.Read(tsc, _rdtscFrequency.load(std::memory_order_acquire), ActiveSettings::GetTimerShiftFactor());
}
