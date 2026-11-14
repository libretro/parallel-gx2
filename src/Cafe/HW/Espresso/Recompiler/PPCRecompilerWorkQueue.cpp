#include <cstdlib>
#include <cstring>

#include "Cafe/HW/Espresso/Recompiler/PPCRecompilerWorkQueue.h"

static constexpr size_t kCellBytes = PPCRecompilerWorkQueue::kCapacity * LF_RING_STRIDE(sizeof(uint32_t));

bool PPCRecompilerWorkQueue::Init()
{
	if (m_initialized)
		return true;
	// at the recompiler's start, not while a game plays
	m_cells = (unsigned char*)calloc(1, kCellBytes);
	if (!m_cells)
		return false;
	if (!retro_eventcount_init(&m_wake))
	{
		retro_eventcount_free(&m_wake);
		free(m_cells);
		m_cells = nullptr;
		return false;
	}
	lf_ring_init(&m_ring, m_cells, kCapacity, sizeof(uint32_t));
	m_initialized = true;
	return true;
}

void PPCRecompilerWorkQueue::Free()
{
	if (!m_initialized)
		return;
	retro_eventcount_free(&m_wake);
	free(m_cells);
	m_cells = nullptr;
	m_initialized = false;
}

bool PPCRecompilerWorkQueue::Push(uint32_t address)
{
	if (!m_initialized || !lf_ring_push(&m_ring, &address))
		return false;
	retro_eventcount_notify(&m_wake);
	return true;
}

bool PPCRecompilerWorkQueue::Pop(uint32_t& address)
{
	if (!m_initialized)
		return false;
	return lf_ring_pop(&m_ring, &address);
}

void PPCRecompilerWorkQueue::WaitForWork(const std::atomic_bool& stop)
{
	if (!m_initialized)
		return;
	for (;;)
	{
		if (stop.load(std::memory_order_acquire) || lf_ring_count(&m_ring) != 0)
			return;
		const int key = retro_eventcount_prepare_wait(&m_wake);
		if (stop.load(std::memory_order_acquire) || lf_ring_count(&m_ring) != 0)
		{
			retro_eventcount_cancel_wait(&m_wake);
			return;
		}
		retro_eventcount_commit_wait(&m_wake, key);
	}
}

void PPCRecompilerWorkQueue::Wake()
{
	if (m_initialized)
		retro_eventcount_notify(&m_wake);
}

void PPCRecompilerWorkQueue::Clear()
{
	if (!m_initialized)
		return;
	memset(m_cells, 0, kCellBytes);
	lf_ring_init(&m_ring, m_cells, kCapacity, sizeof(uint32_t));
}
