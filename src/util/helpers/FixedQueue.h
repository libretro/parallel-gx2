#pragma once

#include <chrono>
#include <cstddef>
#include <type_traits>

#include "util/helpers/EventWake.h"
#include "util/helpers/lf_ring.h"

// A queue of at most N items any thread pushes to and takes from, with no
// lock and nothing allocated: a C ring (lf_ring.h) in the queue's own
// storage. A push that finds it full says so. A pop with nothing to give
// sleeps on an eventcount the push notifies.
template<typename T, size_t N = 1024>
class FixedQueue
{
	static_assert(std::is_trivially_copyable<T>::value, "items are copied as octets");
	static_assert(N && (N & (N - 1)) == 0, "a power of two");

public:
	FixedQueue() { lf_ring_init(&m_ring, m_cells, (unsigned)N, (unsigned)sizeof(T)); }
	FixedQueue(const FixedQueue&) = delete;
	FixedQueue& operator=(const FixedQueue&) = delete;

	// false when full
	bool push(const T& item)
	{
		if (!lf_ring_push(&m_ring, &item))
			return false;
		m_wake.Notify();
		return true;
	}

	// Takes the oldest item if there is one; never waits
	bool peek2(T& item) { return lf_ring_pop(&m_ring, &item); }

	// Takes the oldest item, asleep until there is one
	void pop(T& item)
	{
		bool got = false;
		m_wake.WaitUntil([&] { return got || (got = peek2(item)); });
	}

	T pop()
	{
		T item;
		pop(item);
		return item;
	}

	// As pop, but back by the deadline whether or not there was anything;
	// true if it took an item
	bool pop_until(T& item, std::chrono::steady_clock::time_point deadline)
	{
		bool got = false;
		m_wake.WaitUntil([&] { return got || (got = peek2(item)); }, deadline);
		return got;
	}

	void clear()
	{
		T item;
		while (peek2(item))
			;
	}

	size_t size() { return lf_ring_count(&m_ring); }
	bool empty() { return size() == 0; }

private:
	alignas(8) unsigned char m_cells[N * LF_RING_STRIDE(sizeof(T))]{};
	lf_ring_t m_ring;
	EventWake m_wake;
};
