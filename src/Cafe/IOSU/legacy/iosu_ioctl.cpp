#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "iosu_ioctl.h"
#include "util/helpers/MpscQueue.h"

#include "util/helpers/EventWake.h"

#include <atomic>
#include <thread>
#include <chrono>

// deprecated IOCTL handling code

// A queue per device: the emulated threads push, the device's one worker
// takes - no lock (a mutex, a ring and a counting semaphore before). Never
// destroyed: the workers are detached, and one still waiting as the process
// exits finds its queue where it was. A null entry is a shutdown's wake.
static MpscQueue<ioQueueEntry_t*>* const sIoctlQueues = new MpscQueue<ioQueueEntry_t*>[IOS_DEVICE_COUNT];

sint32 iosuIoctl_pushAndWait(uint32 ioctlHandle, ioQueueEntry_t* ioQueueEntry)
{
	if (ioctlHandle != IOS_DEVICE_ACT && ioctlHandle != IOS_DEVICE_ACP_MAIN && ioctlHandle != IOS_DEVICE_MCP && ioctlHandle != IOS_DEVICE_BOSS && ioctlHandle != IOS_DEVICE_NIM && ioctlHandle != IOS_DEVICE_FPD)
	{
		cemuLog_logDebug(LogType::Force, "Unsupported IOSU device {}", ioctlHandle);
		cemu_assert_debug(false);
		return 0;
	}
	__OSLockScheduler();
	ioQueueEntry->ppcThread = coreinit::OSGetCurrentThread();
	sIoctlQueues[ioctlHandle].push(ioQueueEntry);
	coreinit::__OSSuspendThreadInternal(coreinit::OSGetCurrentThread());
	if (ioQueueEntry->isCompleted == false)
		assert_dbg();
	__OSUnlockScheduler();
	return ioQueueEntry->returnValue;
}

// The deprecated IOSU threads below park in their queue's pop and have no way
// out of their own: they are detached and their loops never end. A libretro
// core gets unloaded, so a shutdown wakes each of them to leave.
static std::atomic_bool sIoctlShuttingDown{false};
// How many of them are inside the wait right now
static std::atomic<int> sIoctlWaiters{0};

void iosuIoctl_requestShutdown()
{
	sIoctlShuttingDown.store(true, std::memory_order_release);
	// One wake per device is enough: each has a single worker.
	for (uint32 i = 0; i < IOS_DEVICE_COUNT; i++)
		sIoctlQueues[i].push(nullptr);
}

bool iosuIoctl_hasWaiters()
{
	return sIoctlWaiters.load(std::memory_order_acquire) > 0;
}

// How many workers are inside their loop. Not the same as the waiter count
// above: that one counts threads blocked in the semaphore, and a thread that
// has been woken but has not yet reached the end of its function is in neither
// state. Stopping has to mean gone.
static std::atomic<uint32> sIoctlWorkersRunning{0};
// What the wait for them to stop sleeps on; the last one out notifies. Never
// destroyed: these threads are detached and may be leaving as the process does.
static ImmortalEventWake sIoctlWorkersStopped;

IosuIoctlWorkerScope::IosuIoctlWorkerScope()
{
	sIoctlWorkersRunning.fetch_add(1, std::memory_order_acq_rel);
}

IosuIoctlWorkerScope::~IosuIoctlWorkerScope()
{
	if (sIoctlWorkersRunning.fetch_sub(1, std::memory_order_acq_rel) == 1)
		sIoctlWorkersStopped->Notify();
}

uint32 iosuIoctl_runningWorkerCount()
{
	return sIoctlWorkersRunning.load(std::memory_order_acquire);
}

// The one wait in this core that still has a deadline, and it is the one whose
// threads cannot be joined: upstream detaches these and keeps no handle, which
// is a decision to leak them rather than to let them hold a shutdown up. So
// this cannot be a join, and an unbounded version of it would hand the frontend
// a hang in exchange for a worker that was never going to come back anyway.
//
// Asleep until the last of them leaves (it notifies) rather than a millisecond
// at a time; the deadline only bounds it.
bool iosuIoctl_waitForWorkersToStop(int timeoutMs)
{
	return sIoctlWorkersStopped->WaitUntil(
		[] { return sIoctlWorkersRunning.load(std::memory_order_acquire) == 0; },
		std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs));
}

// requestShutdown posts once per device to let each worker out, including the
// devices that never had one - so every shutdown leaves posts behind that
// nobody consumes, and they were still there for the next run to walk through.
// The next run coped with them instead of being spared them, which is the kind
// of thing worth removing rather than documenting.
//
// It can be removed now only because the wait above became an answer: a reset
// while a worker is inside decrementWithWait would be a race, and before there
// was no way to know whether one was.
void iosuIoctl_resetAfterWorkersStopped()
{
	cemu_assert_debug(sIoctlWorkersRunning.load(std::memory_order_acquire) == 0);
	for (sint32 i = 0; i < IOS_DEVICE_COUNT; i++)
	{
		// Anything still queued belongs to a title that has ended, and the
		// emulated thread that put it there went with it - and the shutdown's
		// own wakes for devices that never had a worker
		sIoctlQueues[i].clear();
	}
}

// The shutdown above is a latch, and a libretro core is asked to come back:
// the frontend deinitialises this core when content is closed and initialises
// it again for the next content, in the same process, without unloading the
// library. With the latch left set, every worker below had already left and
// the next title's requests went into queues nobody was reading - which is a
// title that boots and then stops dead in its first save call, for good.
void iosuIoctl_clearShutdown()
{
	sIoctlShuttingDown.store(false, std::memory_order_release);
}

ioQueueEntry_t* iosuIoctl_getNextWithWait(uint32 deviceIndex)
{
	while (true)
	{
		sIoctlWaiters.fetch_add(1, std::memory_order_acq_rel);
		ioQueueEntry_t* entry = sIoctlQueues[deviceIndex].pop();
		sIoctlWaiters.fetch_sub(1, std::memory_order_acq_rel);
		if (sIoctlShuttingDown.load(std::memory_order_acquire))
			return nullptr;
		if (entry)
			return entry;
		// A shutdown's wake left over from before the queues were cleared:
		// wait again
	}
}

ioQueueEntry_t* iosuIoctl_getNextWithTimeout(uint32 deviceIndex, sint32 ms)
{
	ioQueueEntry_t* entry = nullptr;
	if (!sIoctlQueues[deviceIndex].pop_until(entry, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)))
		return nullptr; // timeout
	return entry;
}

void iosuIoctl_completeRequest(ioQueueEntry_t* ioQueueEntry, uint32 returnValue)
{
	ioQueueEntry->returnValue = returnValue;
	ioQueueEntry->isCompleted = true;
	coreinit::OSResumeThread(ioQueueEntry->ppcThread);
}

void iosuIoctl_init()
{
	for (sint32 i = 0; i < IOS_DEVICE_COUNT; i++)
	{
		sIoctlQueues[i].clear();
	}
}
