#pragma once

#include <atomic>
#include <cstdint>

#include <rthreads/retro_eventcount.h>

#include "util/helpers/lf_ring.h"

// The recompiler's to-do list: guest addresses the interpreter has run once and
// the recompiler thread should translate. Upstream kept them in a std::queue
// under the recompiler's spinlock and had the thread look every 10 ms, so a hot
// function ran interpreted for 5 ms on average before its translation even
// began, and each push could allocate.
//
// Now a fixed ring and an eventcount: a push wakes the thread at once, and
// nothing allocates after Init. Any core pushes, with a compare-and-swap
// (lf_ring.c), with no lock; the recompiler thread pops.
class PPCRecompilerWorkQueue
{
public:
	// Enough for every function a title touches in its first seconds; when it
	// is full a push is refused and the address stays unvisited, so the
	// interpreter offers it again later rather than anything being lost.
	static constexpr unsigned kCapacity = 16384;

	bool Init();
	void Free();
	// Any thread: false when full
	bool Push(uint32_t address);
	// Consumer: false when empty
	bool Pop(uint32_t& address);
	// Consumer: returns when there is work or stop is set. No timeout: the
	// only things that end the wait are a push and Wake().
	void WaitForWork(const std::atomic_bool& stop);
	// Anyone: lets a waiting consumer out (to see a stop, or other work)
	void Wake();
	// Neither side running: drop whatever is queued
	void Clear();

private:
	lf_ring_t m_ring{};
	unsigned char* m_cells = nullptr;
	retro_eventcount_t m_wake{};
	bool m_initialized = false;
};
