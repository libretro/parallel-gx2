#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// A fixed pool's free slots, by index, without a lock: a stack whose head
// carries a count of the pops so far beside the index, so a pop that read
// the head before another thread popped that slot and pushed it back fails
// its exchange instead of linking the stack through a slot that is in use.
// Any thread pushes and pops. For the IPC pools: the IOSU kernel's
// dispatchable commands and each core's IPC resource buffers.
template<size_t N>
class IndexFreeList
{
	static_assert(N > 0 && N < 0xFFFFFFFF);
	static constexpr uint32_t EMPTY = 0xFFFFFFFF;

public:
	IndexFreeList() { Clear(); }
	IndexFreeList(const IndexFreeList&) = delete;
	IndexFreeList& operator=(const IndexFreeList&) = delete;

	// Not concurrently with anything else
	void Clear() { m_head.store(Pack(EMPTY, 0), std::memory_order_release); }

	// Every slot free, 0 on top. Not concurrently with anything else
	void Fill()
	{
		for (size_t i = 0; i < N; i++)
			m_next[i].store(i + 1 < N ? (uint32_t)(i + 1) : EMPTY, std::memory_order_relaxed);
		m_head.store(Pack(0, 0), std::memory_order_release);
	}

	void Push(size_t index)
	{
		uint64_t head = m_head.load(std::memory_order_relaxed);
		for (;;)
		{
			m_next[index].store(Index(head), std::memory_order_relaxed);
			if (m_head.compare_exchange_weak(head, Pack((uint32_t)index, Tag(head)), std::memory_order_release, std::memory_order_relaxed))
				return;
		}
	}

	// A free slot, or false if there is none
	bool Pop(size_t& index)
	{
		uint64_t head = m_head.load(std::memory_order_acquire);
		for (;;)
		{
			const uint32_t top = Index(head);
			if (top == EMPTY)
				return false;
			const uint32_t next = m_next[top].load(std::memory_order_relaxed);
			if (m_head.compare_exchange_weak(head, Pack(next, Tag(head) + 1), std::memory_order_acquire, std::memory_order_acquire))
			{
				index = top;
				return true;
			}
		}
	}

private:
	static uint64_t Pack(uint32_t index, uint32_t tag) { return ((uint64_t)tag << 32) | index; }
	static uint32_t Index(uint64_t head) { return (uint32_t)head; }
	static uint32_t Tag(uint64_t head) { return (uint32_t)(head >> 32); }

	std::atomic<uint64_t> m_head;
	std::atomic<uint32_t> m_next[N];
};
