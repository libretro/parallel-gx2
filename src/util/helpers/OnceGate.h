#pragma once

#include <atomic>
#include <cstdint>

#include "util/helpers/EventWake.h"

// Work done once until it is undone: the first caller of Run does it, and any
// caller that comes while it is at it sleeps until it is done rather than
// going on without it; after that Run returns at once, until Reset says the
// work is undone and the next Run does it again. std::call_once without its
// lock, and with a way back: an atomic state and an eventcount.
//
// Reset is for when nothing can be in Run - the work's owner tearing it down.
class OnceGate
{
public:
	OnceGate() = default;
	OnceGate(const OnceGate&) = delete;
	OnceGate& operator=(const OnceGate&) = delete;

	// True for the caller that did the work
	template<typename TWork>
	bool Run(TWork work)
	{
		uint32_t state = kIdle;
		if (!m_state.compare_exchange_strong(state, kRunning, std::memory_order_acquire, std::memory_order_acquire))
		{
			if (state != kDone)
				m_wake.WaitUntil([this] { return m_state.load(std::memory_order_acquire) == kDone; });
			return false;
		}
		work();
		m_state.store(kDone, std::memory_order_release);
		m_wake.Notify();
		return true;
	}

	bool IsDone() const { return m_state.load(std::memory_order_acquire) == kDone; }

	void Reset() { m_state.store(kIdle, std::memory_order_release); }

private:
	static constexpr uint32_t kIdle = 0;
	static constexpr uint32_t kRunning = 1;
	static constexpr uint32_t kDone = 2;

	std::atomic<uint32_t> m_state{kIdle};
	EventWake m_wake;
};
