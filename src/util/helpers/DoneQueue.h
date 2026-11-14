#pragma once

#include <atomic>
#include <cstddef>

#include "util/helpers/FixedQueue.h"

// Work any thread hands to one worker, with no lock and nothing allocated,
// where the handing thread can tell when all of it has been done - not only
// taken: an item counts as done once the worker says so after running it.
// A push that finds the queue full says so, for the caller to wait and try
// again; the waking is the caller's, as is what to sleep on.
template<typename T, size_t N>
class DoneQueue
{
public:
	DoneQueue() = default;
	DoneQueue(const DoneQueue&) = delete;
	DoneQueue& operator=(const DoneQueue&) = delete;

	// Any thread: false, with nothing queued, when full
	bool TryPush(const T& item)
	{
		// counted before it can be taken, so AllDone is false until it is done
		m_queued.fetch_add(1, std::memory_order_acq_rel);
		if (m_queue.push(item))
			return true;
		m_queued.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}

	// The worker: the oldest item, if there is one
	bool Take(T& item) { return m_queue.peek2(item); }
	// The worker, once an item taken has run
	void Done() { m_done.fetch_add(1, std::memory_order_acq_rel); }

	// Any thread: everything pushed before this call has run. Done is read
	// first: it never passes queued, so the two equal means no item was
	// pushed in between and every one before has run.
	bool AllDone() const
	{
		const size_t done = m_done.load(std::memory_order_acquire);
		return done == m_queued.load(std::memory_order_acquire);
	}

private:
	FixedQueue<T, N> m_queue;
	std::atomic<size_t> m_queued{0};
	std::atomic<size_t> m_done{0};
};
