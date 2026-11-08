#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <utility>

#include "util/helpers/EventWake.h"
#include "util/helpers/MpscList.h"

// ConcurrentQueue for the queues that have one consumer, without its mutex
// and condition variable: any thread pushes through an MpscList, and the one
// thread that pops takes everything pushed so far into a queue of its own,
// oldest first, and hands it out from there. A pop with nothing to give
// sleeps on an eventcount the push notifies - which costs the push an atomic
// increment when nobody is asleep. The same names as ConcurrentQueue, so a
// queue moves over by its declaration alone.
//
// One consumer: pop, peek2 and clear on one thread at a time. size and empty
// may be asked from anywhere and are as current as the moment they are read.
template<typename T>
class MpscQueue
{
public:
	MpscQueue() = default;
	MpscQueue(const MpscQueue&) = delete;
	MpscQueue& operator=(const MpscQueue&) = delete;

	size_t push(T item)
	{
		const size_t count = m_count.fetch_add(1, std::memory_order_relaxed) + 1;
		m_in.Push(std::move(item));
		m_wake.Notify();
		return count;
	}


	// Takes the oldest item if there is one; never waits
	bool peek2(T& item)
	{
		if (m_out.empty())
			m_in.TakeAll([this](T&& value) { m_out.push_back(std::move(value)); });
		if (m_out.empty())
			return false;
		item = std::move(m_out.front());
		m_out.pop_front();
		m_count.fetch_sub(1, std::memory_order_relaxed);
		return true;
	}

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

	size_t size() const
	{
		const ptrdiff_t n = (ptrdiff_t)m_count.load(std::memory_order_relaxed);
		return n > 0 ? (size_t)n : 0;
	}

	bool empty() const { return size() == 0; }

private:
	MpscList<T> m_in;
	std::deque<T> m_out; // the consumer's alone
	std::atomic<size_t> m_count{0};
	EventWake m_wake;
};
