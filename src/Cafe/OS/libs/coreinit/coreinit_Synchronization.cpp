#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/fast_mutex.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace coreinit
{
	/************* OSEvent ************/

	void OSInitEvent(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode)
	{
		event->magic = 'eVnT';
		cemu_assert_debug(event->magic == 0x65566e54);
		event->userData = nullptr;
		event->ukn08 = 0;
		event->state = initialState;
		event->mode = mode;
		OSInitThreadQueueEx(&event->threadQueue, event);
	}

	void OSInitEventEx(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode, void* userData)
	{
		OSInitEvent(event, initialState, mode);
		event->userData = userData;
	}

	void OSResetEvent(OSEvent* event)
	{
		__OSLockScheduler();
		if (event->state == OSEvent::EVENT_STATE::STATE_SIGNALED)
			event->state = OSEvent::EVENT_STATE::STATE_NOT_SIGNALED;
		__OSUnlockScheduler();
	}

	void OSWaitEventInternal(OSEvent* event)
	{
		if (event->state == OSEvent::EVENT_STATE::STATE_SIGNALED)
		{
			if (event->mode == OSEvent::EVENT_MODE::MODE_AUTO)
				event->state = OSEvent::EVENT_STATE::STATE_NOT_SIGNALED;
		}
		else
		{
			// enter wait queue
			event->threadQueue.queueAndWait(OSGetCurrentThread());
		}
	}

	void OSWaitEvent(OSEvent* event)
	{
		__OSLockScheduler();
		OSWaitEventInternal(event);
		__OSUnlockScheduler();
	}

	struct WaitEventWithTimeoutData
	{
		OSThread_t* thread;
		OSThreadQueue* threadQueue;
		std::atomic_bool hasTimeout;
	};

	void _OSWaitEventWithTimeoutHandler(uint64 currentTick, void* context)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		WaitEventWithTimeoutData* data = (WaitEventWithTimeoutData*)context;
		if (data->thread->state == OSThread_t::THREAD_STATE::STATE_WAITING)
		{
			data->hasTimeout = true;
			data->threadQueue->cancelWait(data->thread);
		}
	}

	bool OSWaitEventWithTimeout(OSEvent* event, uint64 timeout)
	{
		__OSLockScheduler();
		if (event->state == OSEvent::EVENT_STATE::STATE_SIGNALED)
		{
			if (event->mode == OSEvent::EVENT_MODE::MODE_AUTO)
				event->state = OSEvent::EVENT_STATE::STATE_NOT_SIGNALED;
		}
		else
		{
			if (timeout == 0)
			{
				// fail immediately
				__OSUnlockScheduler();
				return false;
			}
			// wait and set timeout

			// workaround for a bad implementation in some Unity games (like Qube Directors Cut, see FEventWiiU::Wait)
			// where the the return value of OSWaitEventWithTimeout is ignored and instead the game measures the elapsed time to determine if a timeout occurred
			if (timeout < 0x00FFFFFFFFFFFFFFULL)
				timeout = timeout * 98ULL / 100ULL; // 98% (we want the function to return slightly before the actual timeout)

			WaitEventWithTimeoutData data;
			data.thread = OSGetCurrentThread();
			data.threadQueue = &event->threadQueue;
			data.hasTimeout = false;
			auto hostAlarm = coreinit::OSHostAlarmCreate(OSGetTime() + coreinit::EspressoTime::ConvertNsToTimerTicks(timeout), 0, _OSWaitEventWithTimeoutHandler, &data);
			event->threadQueue.queueAndWait(OSGetCurrentThread());
			coreinit::OSHostAlarmDestroy(hostAlarm);
			if (data.hasTimeout)
			{
				__OSUnlockScheduler();
				return false;
			}
		}
		__OSUnlockScheduler();
		return true;
	}

	void OSSignalEventInternal(OSEvent* event)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		if (event->state == OSEvent::EVENT_STATE::STATE_SIGNALED)
		{
			return;
		}
		if (event->mode == OSEvent::EVENT_MODE::MODE_AUTO)
		{
			// in auto mode wake up one thread or if there is none then set signaled
			if (event->threadQueue.isEmpty())
				event->state = OSEvent::EVENT_STATE::STATE_SIGNALED;
			else
				event->threadQueue.wakeupSingleThreadWaitQueue(true);
		}
		else
		{
			// in manual mode wake up all threads and set to signaled
			event->state = OSEvent::EVENT_STATE::STATE_SIGNALED;
			event->threadQueue.wakeupEntireWaitQueue(true);
		}
	}

	void OSSignalEvent(OSEvent* event)
	{
		__OSLockScheduler();
		OSSignalEventInternal(event);
		__OSUnlockScheduler();
	}

	void OSSignalEventAllInternal(OSEvent* event)
	{
		if (event->state == OSEvent::EVENT_STATE::STATE_SIGNALED)
		{
			return;
		}
		if (event->mode == OSEvent::EVENT_MODE::MODE_AUTO)
		{
			// in auto mode wake up one thread or if there is none then set signaled
			if (event->threadQueue.isEmpty())
				event->state = OSEvent::EVENT_STATE::STATE_SIGNALED;
			else
				event->threadQueue.wakeupEntireWaitQueue(true);
		}
		else
		{
			// in manual mode wake up all threads and set to signaled
			event->state = OSEvent::EVENT_STATE::STATE_SIGNALED;
			event->threadQueue.wakeupEntireWaitQueue(true);
		}
	}

	void OSSignalEventAll(OSEvent* event)
	{
		__OSLockScheduler();
		OSSignalEventAllInternal(event);
		__OSUnlockScheduler();
	}

	/************* OSRendezvous ************/

	SysAllocator<OSEvent> g_rendezvousEvent;

	void OSInitRendezvous(OSRendezvous* rendezvous)
	{
		__OSLockScheduler();
		rendezvous->userData = rendezvous;
		for (sint32 i = 0; i < PPC_CORE_COUNT; i++)
			rendezvous->coreHit[i] = 0;
		__OSUnlockScheduler();
	}

	bool OSWaitRendezvous(OSRendezvous* rendezvous, uint32 coreMask)
	{
		__OSLockScheduler();
		rendezvous->coreHit[OSGetCoreId()] = 1;
		OSSignalEventAllInternal(g_rendezvousEvent.GetPtr());
		while (true)
		{
			bool metAll = true;
			for(sint32 i=0; i<PPC_CORE_COUNT; i++)
			{ 
				if( (coreMask & (1<<i)) == 0 )
					continue; // core not required by core mask
				if (rendezvous->coreHit[i] == 0)
				{
					metAll = false;
					break;
				}
			}
			if (metAll)
				break;
			OSWaitEventInternal(g_rendezvousEvent.GetPtr());
		}
		__OSUnlockScheduler();
		return true;
	}

	/************* OSMutex ************/

	void OSInitMutexEx(OSMutex* mutex, void* userData)
	{
		mutex->magic = 'mUtX';
		mutex->userData = userData;
		mutex->ukn08 = 0;
		mutex->owner = nullptr;
		mutex->lockCount = 0;
		OSInitThreadQueueEx(&mutex->threadQueue, mutex);
	}

	void OSInitMutex(OSMutex* mutex)
	{
		OSInitMutexEx(mutex, nullptr);
	}

	void OSLockMutexInternal(OSMutex* mutex)
	{
		OSThread_t* currentThread = OSGetCurrentThread();
		int_fast32_t failedAttempts = 0;
		while (true)
		{
			if (mutex->owner == nullptr)
			{
				// acquire lock
				mutex->owner = currentThread;
				cemu_assert_debug(mutex->lockCount == 0);
				mutex->lockCount = 1;
				// cemu_assert_debug(mutex->next == nullptr && mutex->prev == nullptr); -> not zero initialized
				currentThread->mutexQueue.addMutex(mutex);
				break;
			}
			else if (mutex->owner == currentThread)
			{
				mutex->lockCount = mutex->lockCount + 1;
				break;
			}
			else
			{
				if (failedAttempts >= 0x800)
					cemuLog_log(LogType::Force, "Detected long-term contested OSLockMutex");
				currentThread->waitingForMutex = mutex;
				mutex->threadQueue.queueAndWait(currentThread);
				currentThread->waitingForMutex = nullptr;
				failedAttempts++;
			}
		}
	}

	void OSLockMutex(OSMutex* mutex)
	{
		__OSLockScheduler();
		OSTestThreadCancelInternal();
		OSLockMutexInternal(mutex);
		__OSUnlockScheduler();
	}

	bool OSTryLockMutex(OSMutex* mutex)
	{
		OSThread_t* currentThread = OSGetCurrentThread();
		__OSLockScheduler();
		OSTestThreadCancelInternal();
		if (mutex->owner == nullptr)
		{
			// acquire lock
			mutex->owner = currentThread;
			cemu_assert_debug(mutex->lockCount == 0);
			mutex->lockCount = 1;
			// cemu_assert_debug(mutex->next == nullptr && mutex->prev == nullptr); -> not zero initialized
			currentThread->mutexQueue.addMutex(mutex);
			// currentThread->cancelState = currentThread->cancelState | 0x10000;
		}
		else if (mutex->owner == currentThread)
		{
			mutex->lockCount = mutex->lockCount + 1;
		}
		else
		{
			__OSUnlockScheduler();
			return false;
		}
		__OSUnlockScheduler();
		return true;
	}

	void OSUnlockMutexInternal(OSMutex* mutex)
	{
		OSThread_t* currentThread = OSGetCurrentThread();
		if (mutex->lockCount == 0)
		{
			cemuLog_log(LogType::APIErrors, "OSUnlockMutex called on a mutex which is not locked");
			return;
		}
		if (mutex->owner != currentThread)
		{
			cemuLog_log(LogType::APIErrors, "OSUnlockMutex called by a different thread than the one owning it (owner=0x{:08x} currentThread=0x{:08x})", mutex->owner.GetMPTR(), MEMPTR<void>(currentThread).GetMPTR());
			return;
		}
		mutex->lockCount = mutex->lockCount - 1;
		if (mutex->lockCount == 0)
		{
			currentThread->mutexQueue.removeMutex(mutex);
			mutex->owner = nullptr;
			if (!mutex->threadQueue.isEmpty())
				mutex->threadQueue.wakeupSingleThreadWaitQueue(true, true);
		}
		// currentThread->cancelState = currentThread->cancelState & ~0x10000;
	}

	void OSUnlockMutex(OSMutex* mutex)
	{
		__OSLockScheduler();
		OSUnlockMutexInternal(mutex);
		__OSUnlockScheduler();
	}

	/************* OSCond ************/

	void OSInitCond(OSCond* cond)
	{
		cond->magic = 0x634e6456;
		cond->userData = nullptr;
		cond->ukn08 = 0;
		OSInitThreadQueueEx(&cond->threadQueue, cond);
	}

	void OSInitCondEx(OSCond* cond, void* userData)
	{
		OSInitCond(cond);
		cond->userData = userData;
	}

	void OSSignalCond(OSCond* cond)
	{
		OSWakeupThread(&cond->threadQueue);
	}

	void OSWaitCond(OSCond* cond, OSMutex* mutex)
	{
		// seen in Bayonetta 2
		// releases the mutex while waiting for the condition to be signaled
		__OSLockScheduler();
		OSThread_t* currentThread = OSGetCurrentThread();
		cemu_assert_debug(mutex->owner == currentThread);
		sint32 prevLockCount = mutex->lockCount;
		// unlock mutex
		mutex->lockCount = 0;
		currentThread->mutexQueue.removeMutex(mutex);
		mutex->owner = nullptr;
		if (!mutex->threadQueue.isEmpty())
			mutex->threadQueue.wakeupEntireWaitQueue(false);
		// wait on condition
		cond->threadQueue.queueAndWait(currentThread);
		// reacquire mutex
		OSLockMutexInternal(mutex);
		mutex->lockCount = prevLockCount;
		__OSUnlockScheduler();
	}

	/************* OSSemaphore ************/

	void OSInitSemaphoreEx(OSSemaphore* semaphore, sint32 initialCount, void* userData)
	{
		__OSLockScheduler();
		semaphore->magic = 0x73506852;
		semaphore->userData = userData;
		semaphore->ukn08 = 0;
		semaphore->count = initialCount;
		OSInitThreadQueueEx(&semaphore->threadQueue, semaphore);
		__OSUnlockScheduler();
	}

	void OSInitSemaphore(OSSemaphore* semaphore, sint32 initialCount)
	{
		OSInitSemaphoreEx(semaphore, initialCount, nullptr);
	}

	sint32 OSWaitSemaphoreInternal(OSSemaphore* semaphore)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		while (true)
		{
			sint32 prevCount = semaphore->count;
			if (prevCount > 0)
			{
				semaphore->count = prevCount - 1;
				return prevCount;
			}
			semaphore->threadQueue.queueAndWait(OSGetCurrentThread());
		}
	}

	sint32 OSWaitSemaphore(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 r = OSWaitSemaphoreInternal(semaphore);
		__OSUnlockScheduler();
		return r;
	}

	sint32 OSTryWaitSemaphore(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 prevCount = semaphore->count;
		if (prevCount > 0)
		{
			semaphore->count = prevCount - 1;
		}
		__OSUnlockScheduler();
		return prevCount;
	}

	sint32 OSSignalSemaphoreInternal(OSSemaphore* semaphore, bool reschedule)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		sint32 prevCount = semaphore->count;
		semaphore->count = prevCount + 1;
		semaphore->threadQueue.wakeupEntireWaitQueue(reschedule);
		return prevCount;
	}

	sint32 OSSignalSemaphore(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 r = OSSignalSemaphoreInternal(semaphore, true);
		__OSUnlockScheduler();
		return r;
	}

	sint32 OSGetSemaphoreCount(OSSemaphore* semaphore)
	{
		// seen in Assassin's Creed 4
		__OSLockScheduler();
		sint32 currentCount = semaphore->count;
		__OSUnlockScheduler();
		return currentCount;
	}

	/************* OSFastMutex ************/

	void OSFastMutex_Init(OSFastMutex* fastMutex, void* userData)
	{
		fastMutex->magic = 0x664d7458;
		fastMutex->userData = userData;
		fastMutex->owner = nullptr;
		fastMutex->lockCount = 0;
		fastMutex->contendedState = 0;
		fastMutex->threadQueueSmall.head = nullptr;
		fastMutex->threadQueueSmall.tail = nullptr;
		fastMutex->ownedLink.next = nullptr;
		fastMutex->ownedLink.prev = nullptr;
		fastMutex->contendedLink.next = nullptr;
		fastMutex->contendedLink.prev = nullptr;
	}

	// No lock of the host's (fast_mutex.c): the owner is taken and given back
	// with a compare-and-swap, and only a thread that must wait takes the
	// scheduler lock, which the wait queue needs anyway; contendedState (the
	// console's own contention marker) counts the threads about to wait or
	// waiting, so a giver wakes one only when there is one.
	static retro_atomic_int_t* _OSFastMutex_Owner(OSFastMutex* fastMutex)
	{
		return reinterpret_cast<retro_atomic_int_t*>(&fastMutex->owner);
	}

	static retro_atomic_int_t* _OSFastMutex_Waiters(OSFastMutex* fastMutex)
	{
		return reinterpret_cast<retro_atomic_int_t*>(&fastMutex->contendedState);
	}

	// the owner field's own bits for a thread (never 0)
	static int _OSFastMutex_Id(OSThread_t* thread)
	{
		MEMPTR<OSThread_t> p = thread;
		int raw;
		memcpy(&raw, &p, sizeof(raw));
		return raw;
	}

	void OSFastMutex_LockInternal(OSFastMutex* fastMutex)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		OSThread_t* currentThread = OSGetCurrentThread();
		const int me = _OSFastMutex_Id(currentThread);
		retro_atomic_int_t* owner = _OSFastMutex_Owner(fastMutex);
		retro_atomic_int_t* waiters = _OSFastMutex_Waiters(fastMutex);
		while (true)
		{
			if (fast_mutex_take(owner, me))
			{
				// acquire lock
				cemu_assert_debug(fastMutex->lockCount == 0);
				fastMutex->lockCount = 1;

				// todo - add to thread owned fast mutex queue
				return;
			}
			if (fast_mutex_owned(owner, me))
			{
				fastMutex->lockCount = fastMutex->lockCount + 1;
				return;
			}
			__OSLockScheduler();
			if (fast_mutex_wait_begin(owner, waiters, me))
			{
				// given back meanwhile
				__OSUnlockScheduler();
				cemu_assert_debug(fastMutex->lockCount == 0);
				fastMutex->lockCount = 1;
				return;
			}
			currentThread->waitingForFastMutex = fastMutex;
			fastMutex->threadQueueSmall.queueOnly(currentThread);
			PPCCore_switchToSchedulerWithLock();
			currentThread->waitingForFastMutex = nullptr;
			fast_mutex_wait_end(waiters);
			__OSUnlockScheduler();
		}
	}

	void OSFastMutex_Lock(OSFastMutex* fastMutex)
	{
		OSFastMutex_LockInternal(fastMutex);
	}

	bool OSFastMutex_TryLock(OSFastMutex* fastMutex)
	{
		OSThread_t* currentThread = OSGetCurrentThread();
		const int me = _OSFastMutex_Id(currentThread);
		if (fast_mutex_take(_OSFastMutex_Owner(fastMutex), me))
		{
			// acquire lock
			cemu_assert_debug(fastMutex->lockCount == 0);
			fastMutex->lockCount = 1;

			// todo - add to thread owned fast mutex queue
			return true;
		}
		if (fast_mutex_owned(_OSFastMutex_Owner(fastMutex), me))
		{
			fastMutex->lockCount = fastMutex->lockCount + 1;
			return true;
		}
		return false;
	}

	void OSFastMutex_UnlockInternal(OSFastMutex* fastMutex)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		const int me = _OSFastMutex_Id(OSGetCurrentThread());
		if (!fast_mutex_owned(_OSFastMutex_Owner(fastMutex), me))
		{
			// seen in Paper Mario Color Splash
			//cemuLog_log(LogType::Force, "OSFastMutex_Unlock() called on mutex which is not owned by current thread");
			return;
		}
		cemu_assert_debug(fastMutex->lockCount > 0);
		fastMutex->lockCount = fastMutex->lockCount - 1;
		if (fastMutex->lockCount != 0)
			return;
		// a waiter, or one about to be: woken under the scheduler lock, which
		// it holds until it is queued
		if (fast_mutex_give(_OSFastMutex_Owner(fastMutex), _OSFastMutex_Waiters(fastMutex), me))
		{
			__OSLockScheduler();
			if (!fastMutex->threadQueueSmall.isEmpty())
				fastMutex->threadQueueSmall.wakeupSingleThreadWaitQueue(false);
			__OSUnlockScheduler();
		}
	}

	void OSFastMutex_Unlock(OSFastMutex* fastMutex)
	{
		//__OSLockScheduler();
		OSFastMutex_UnlockInternal(fastMutex);
		//__OSUnlockScheduler();
	}

	/************* OSFastCond ************/

	void OSFastCond_Init(OSFastCond* fastCond, void* userData)
	{
		fastCond->magic = 0x664e6456;
		fastCond->userData = userData;
		fastCond->ukn08 = 0;
		OSInitThreadQueueEx(&fastCond->threadQueue, fastCond);
	}

	void OSFastCond_Wait(OSFastCond* fastCond, OSFastMutex* fastMutex)
	{
		// releases the mutex while waiting for the condition to be signaled
		__OSLockScheduler();
		cemu_assert_debug(fastMutex->owner == OSGetCurrentThread());
		sint32 prevLockCount = fastMutex->lockCount;
		// unlock mutex
		fastMutex->lockCount = 0;
		retro_atomic_cas_int(_OSFastMutex_Owner(fastMutex), _OSFastMutex_Id(OSGetCurrentThread()), 0);
		if (!fastMutex->threadQueueSmall.isEmpty())
			fastMutex->threadQueueSmall.wakeupEntireWaitQueue(false);
		// wait on condition
		fastCond->threadQueue.queueAndWait(OSGetCurrentThread());
		// reacquire mutex
		__OSUnlockScheduler();
		OSFastMutex_LockInternal(fastMutex);
		fastMutex->lockCount = prevLockCount;
	}

	void OSFastCond_Signal(OSFastCond* fastCond)
	{
		OSWakeupThread(&fastCond->threadQueue);
	}

	/************* init ************/

	void InitializeConcurrency()
	{
		OSInitEvent(g_rendezvousEvent.GetPtr(), OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_AUTO);

		// OSEvent
		cafeExportRegister("coreinit", OSInitEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitEventEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSResetEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitEventWithTimeout, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalEventAll, LogType::CoreinitThreadSync);

		// OSRendezvous
		cafeExportRegister("coreinit", OSInitRendezvous, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitRendezvous, LogType::CoreinitThreadSync);

		// OSMutex
		cafeExportRegister("coreinit", OSInitMutex, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitMutexEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSLockMutex, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSTryLockMutex, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSUnlockMutex, LogType::CoreinitThreadSync);

		// OSCond
		cafeExportRegister("coreinit", OSInitCond, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitCondEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalCond, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitCond, LogType::CoreinitThreadSync);

		// OSSemaphore
		cafeExportRegister("coreinit", OSInitSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitSemaphoreEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSTryWaitSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSGetSemaphoreCount, LogType::CoreinitThreadSync);

		// OSFastMutex
		cafeExportRegister("coreinit", OSFastMutex_Init, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastMutex_Lock, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastMutex_TryLock, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastMutex_Unlock, LogType::CoreinitThreadSync);

		// OSFastCond
		cafeExportRegister("coreinit", OSFastCond_Init, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastCond_Wait, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastCond_Signal, LogType::CoreinitThreadSync);
	}

};
