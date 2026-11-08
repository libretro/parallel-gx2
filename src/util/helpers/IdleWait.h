#pragma once

#include <atomic>
#include <chrono>

#include "util/helpers/EventWake.h"

// Work any number of threads go through at any time, and one thread that
// wants a moment when none of it is under way - without the work ever
// waiting for it. Enter/Leave cost an atomic each; the last one out also
// notifies (an atomic increment when nobody waits). The waiter sleeps on an
// eventcount until the work is idle or its own predicate says to stop, and
// the same wake serves its other waits (WaitFor).
//
// It is a moment, not a lock: work may start again right after WaitIdle
// returns. For the Vulkan pipeline cache's save, which the compile threads
// go first for: it is read while none is building a pipeline, and one that
// starts during the read goes ahead (the cache is internally synchronized).
class IdleWait
{
public:
	void Enter() { m_busy.fetch_add(1, std::memory_order_acq_rel); }

	void Leave()
	{
		if (m_busy.fetch_sub(1, std::memory_order_acq_rel) == 1)
			m_wake.Notify();
	}

	bool Idle() const { return m_busy.load(std::memory_order_acquire) == 0; }

	// Wakes the waiter to look at its predicate again (it changed)
	void Notify() { m_wake.Notify(); }

	// Until nothing is under way or stop() holds; false if stop() does
	template<typename TStop>
	bool WaitIdle(TStop stop)
	{
		m_wake.WaitUntil([&] { return stop() || Idle(); });
		return !stop();
	}

	// Until the deadline or stop() holds; true if stop() does
	template<typename TStop>
	bool WaitFor(TStop stop, std::chrono::steady_clock::duration duration)
	{
		return m_wake.WaitUntil(stop, std::chrono::steady_clock::now() + duration);
	}

private:
	std::atomic<uint32_t> m_busy{0};
	EventWake m_wake;
};
