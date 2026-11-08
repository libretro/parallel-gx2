#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "util/helpers/EventWake.h"

// The counting semaphores and the state flag the emulator's host threads wait
// on, without a lock (a mutex and a condition variable each before): the
// count is an atomic, taken with a compare-exchange, and a waiter sleeps on
// an eventcount every change notifies - which costs an atomic increment when
// nobody is asleep. The idle cores wait on their run queue's count here.

class Semaphore
{
public:
	void notify()
	{
		m_count.fetch_add(1, std::memory_order_release);
		m_wake.Notify();
	}

	void wait()
	{
		while (!try_wait())
			m_wake.WaitUntil([this] { return m_count.load(std::memory_order_acquire) != 0; });
	}

	bool try_wait()
	{
		uint64_t count = m_count.load(std::memory_order_acquire);
		while (count != 0)
		{
			if (m_count.compare_exchange_weak(count, count - 1, std::memory_order_acquire, std::memory_order_acquire))
				return true;
		}
		return false;
	}

	void reset()
	{
		m_count.store(0, std::memory_order_release);
	}

private:
	std::atomic<uint64_t> m_count{0};
	EventWake m_wake;
};

class CounterSemaphore
{
public:
	void reset()
	{
		m_count.store(0, std::memory_order_release);
		m_wake.Notify();
	}

	void increment()
	{
		m_count.fetch_add(1, std::memory_order_release);
		m_wake.Notify();
	}

	void decrement()
	{
		if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
			m_wake.Notify(); // for waitUntilZero
	}

	// decrement only if non-zero
	// otherwise wait
	void decrementWithWait()
	{
		while (!TryDecrement())
			m_wake.WaitUntil([this] { return m_count.load(std::memory_order_acquire) > 0; });
	}

	// decrement only if non-zero
	// otherwise wait, at most ms milliseconds
	bool decrementWithWaitAndTimeout(uint32_t ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		while (!TryDecrement())
		{
			if (!m_wake.WaitUntil([this] { return m_count.load(std::memory_order_acquire) > 0; }, deadline))
				return false;
		}
		return true;
	}

	void waitUntilZero()
	{
		m_wake.WaitUntil([this] { return m_count.load(std::memory_order_acquire) == 0; });
	}

	void waitUntilNonZero()
	{
		m_wake.WaitUntil([this] { return m_count.load(std::memory_order_acquire) != 0; });
	}

	bool isZero() const
	{
		return m_count.load(std::memory_order_acquire) == 0;
	}

private:
	bool TryDecrement()
	{
		int64_t count = m_count.load(std::memory_order_acquire);
		while (count > 0)
		{
			if (m_count.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel, std::memory_order_acquire))
				return true;
		}
		return false;
	}

	std::atomic<int64_t> m_count{0};
	EventWake m_wake;
};

template<typename T>
class StateSemaphore
{
public:
	StateSemaphore(T initialState) : m_state(initialState) {};

	T getValue()
	{
		return m_state.load(std::memory_order_acquire);
	}

	bool hasState(T state)
	{
		return m_state.load(std::memory_order_acquire) == state;
	}

	void setValue(T newState)
	{
		m_state.store(newState, std::memory_order_release);
		m_wake.Notify();
	}

	// once the state is expectedValue, from it to newState
	void setValue(T newState, T expectedValue)
	{
		for (;;)
		{
			T expected = expectedValue;
			if (m_state.compare_exchange_strong(expected, newState, std::memory_order_acq_rel, std::memory_order_acquire))
				break;
			m_wake.WaitUntil([this, expectedValue] { return m_state.load(std::memory_order_acquire) == expectedValue; });
		}
		m_wake.Notify();
	}

	void waitUntilValue(T state)
	{
		m_wake.WaitUntil([this, state] { return m_state.load(std::memory_order_acquire) == state; });
	}

private:
	std::atomic<T> m_state;
	EventWake m_wake;
};
