#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Espresso/Const.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"
#include <atomic>

sint32 s_customVsyncFrequency = -1;

void LatteTiming_signalVsync();

// calculate time between vsync events in timer units
// standard rate on Wii U is 59.94, however to prevent tearing and microstutter on ~60Hz displays it is better if we slightly overshoot 60 Hz
// can be modified by graphic packs
HRTick LatteTime_CalculateTimeBetweenVSync()
{
	// 59.94 -> 60 * 0.999

	HRTick tick = HighResolutionTimer::getFrequency();
	if (s_customVsyncFrequency > 0)
	{
		tick /= (uint64)s_customVsyncFrequency;
	}
	else
	{
		tick *= 1000ull;
		tick /= 1002ull;
		tick /= 60ull;
	}
	return tick;
}

void LatteTiming_setCustomVsyncFrequency(sint32 frequency)
{
	s_customVsyncFrequency = frequency;
}

void LatteTiming_disableCustomVsyncFrequency()
{
	s_customVsyncFrequency = -1;
}

bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency)
{
	sint32 t = s_customVsyncFrequency;
	if (t <= 0)
		return false;
	customFrequency = t;
	return true;
}

// The vsync comes from the guest's time (LatteTiming_GuestVSync), not from
// the timer in LatteTiming_HandleTimedVsync: the frontend's frames set that
// time's pace
static bool s_usingLibretroVSync = true;

void LatteTiming_EnableLibretroVSync()
{
	s_usingLibretroVSync = true;
}

bool LatteTiming_IsUsingLibretroVSync()
{
	return s_usingLibretroVSync;
}

void LatteTiming_TriggerVSync()
{
	if (s_usingLibretroVSync)
		LatteTiming_signalVsync();
}

void LatteTiming_Init()
{
	LatteGPUState.timer_frequency = HighResolutionTimer::getFrequency();
	LatteGPUState.timer_bootUp = HighResolutionTimer::now().getTick();
	LatteGPUState.timer_nextVSync = LatteGPUState.timer_bootUp + LatteTime_CalculateTimeBetweenVSync();
}

void LatteTiming_signalVsync()
{
	static uint32 s_vsyncIntervalCounter = 0;

	if (!LatteGPUState.gx2InitCalled)
		return;
	s_vsyncIntervalCounter++;
	uint32 swapInterval = 1;
	if (LatteGPUState.sharedArea)
		swapInterval = LatteGPUState.sharedArea->swapInterval;

	// flip
	if (s_vsyncIntervalCounter >= swapInterval)
	{
		if (LatteGPUState.sharedArea)
		{
			// hack/workaround - only execute flip if GX2SwapScanBuffers() isn't lagging behind
			uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
			if (currentTitleId == 0x00050000101c9500 || currentTitleId == 0x00050000101c9400 || currentTitleId == 0x0005000e101c9300)
			{
				uint32 currentFlipRequestCount = _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE);
				uint32 currentFlipExecuteCount = _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE);

				if ((currentFlipRequestCount >= currentFlipExecuteCount) || (currentFlipExecuteCount - currentFlipRequestCount < 4))
				{
					LatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE) + 1);
				}

				LatteGPUState.flipCounter++;

			}
			else
			{
				// old code for all other games
				if (LatteGPUState.flipRequestCount > 0)
				{
					LatteGPUState.flipRequestCount.fetch_sub(1);
					LatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE) + 1);
				}

			}
		}
		GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::FLIP);
		s_vsyncIntervalCounter = 0;
	}
	// vsync
	GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::VSYNC);
}

// The emulated vsync by the guest's time, at the rate the timer in
// LatteTiming_HandleTimedVsync keeps on the wall clock
uint64 LatteTiming_GuestVSyncPeriod()
{
	sint32 custom;
	return LatteTiming_getCustomVsyncFrequency(custom)
		? Espresso::CORE_CLOCK / (uint64)custom
		: Espresso::CORE_CLOCK * 1000ull / 1002ull / 60ull;
}

static std::atomic<uint64> s_nextGuestVSync{0};

// When the next vsync is due, in the guest's time; 0 before the first
uint64 LatteTiming_NextGuestVSync()
{
	return s_nextGuestVSync.load(std::memory_order_acquire);
}

// From the main core's idle loop: the vsync signalled as the guest's time
// reaches it. Returns when the next is due.
uint64 LatteTiming_GuestVSync(uint64 now)
{
	const uint64 period = LatteTiming_GuestVSyncPeriod();
	uint64 next = s_nextGuestVSync.load(std::memory_order_relaxed);
	if (next == 0 || now + period < next || now >= next + period * 10)
		next = now + period; // the first, or the time was reset (a new title)
	else if (now >= next)
	{
		LatteTiming_signalVsync();
		next += period;
	}
	s_nextGuestVSync.store(next, std::memory_order_release);
	return next;
}

// handle timed vsync event
void LatteTiming_HandleTimedVsync()
{
	// simulate VSync
	uint64 currentTimer = HighResolutionTimer::now().getTick();
	if( currentTimer >= LatteGPUState.timer_nextVSync )
	{
		// In libretro mode, vsync is triggered by retro_run(), not by timer
		if(!LatteTiming_IsUsingLibretroVSync())
			LatteTiming_signalVsync();
		// even if vsync is delegated to the host device, we still use this virtual vsync timer to check finished states
		LatteQuery_UpdateFinishedQueries();
		LatteTextureReadback_UpdateFinishedTransfers(false);
		// update vsync timer
		uint64 vsyncTime = LatteTime_CalculateTimeBetweenVSync();
		uint64 missedVsyncCount = (currentTimer - LatteGPUState.timer_nextVSync) / vsyncTime;
		if (missedVsyncCount >= 2)
		{
			LatteGPUState.timer_nextVSync += vsyncTime*(missedVsyncCount+1ULL);
		}
		else	
			LatteGPUState.timer_nextVSync += vsyncTime;
	}
}