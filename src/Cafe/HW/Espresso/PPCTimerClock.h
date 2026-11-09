#pragma once

#include <atomic>
#include <cstdint>

#include "util/helpers/SeqlockValue.h"

#if !defined(__SIZEOF_INT128__) && defined(_MSC_VER)
#include <intrin.h>
#endif

// The guest's time base, from the host's TSC, without a lock.
//
// Guest time is the TSC elapsed since an epoch, at the console's clock over the
// TSC's rate, times 8 >> the timer shift (the speed setting), plus the guest
// time at the epoch. The epoch, the rate and the shift it was taken with are
// one SeqlockValue; a read takes them and does the arithmetic, and nothing is
// written on the way. Only when the rate has been refined (the calibration
// thread publishes it) or the shift changed does a read start a new epoch at
// its own TSC, with the guest time the old one gives there, so no time already
// counted moves - and only one read does: the others go on with the old epoch
// rather than wait for it. A last-returned value, raised with a compare and
// exchange only when a read is ahead of it, keeps the time from ever going
// backwards across cores, whose TSCs can be read a little out of order.
//
// It replaces an accumulator every read added to under a spinlock - every
// guest time read on every core.
//
// Two things make it the frontend's clock rather than the wall's. A cap: the
// time stops there - the end of the frame the frontend has asked for - and
// only moves on when the cap is raised, from where it stopped. And a skip:
// with every core and the GPU idle, the time jumps to the next thing due (an
// alarm, AX's next frame, the vsync), no further than the cap, rather than
// waiting it out. So a frame takes as long as its work, and the frontend's
// calls set the speed - fast-forward, slow motion, frame advance - as in the
// cores that count cycles. Both take effect at the next read that sees them,
// with a new epoch made by the one read that wins m_rebasing, as a rate
// change does.
class PPCTimerClock
{
public:
	static constexpr uint64_t kConsoleClock = 1243125000ull; // Espresso::CORE_CLOCK

	static constexpr uint64_t kNoCap = ~0ull;

	// Guest time 0 at this TSC, no cap; nobody may be reading (the title starts)
	void Restart(uint64_t tsc)
	{
		m_epoch.Store(Epoch{tsc, 0, 0, 0, kNoCap});
		m_cap.store(kNoCap, std::memory_order_relaxed);
		m_skip.store(0, std::memory_order_relaxed);
		m_last.store(0, std::memory_order_relaxed);
	}

	// Where the time stops; kNoCap for nowhere. Only ever raised.
	void SetCap(uint64_t cap) { m_cap.store(cap, std::memory_order_release); }
	uint64_t Cap() const { return m_cap.load(std::memory_order_acquire); }

	// Jump to @target, or as far toward it as the cap lets
	void SkipTo(uint64_t target)
	{
		uint64_t cur = m_skip.load(std::memory_order_relaxed);
		while (target > cur && !m_skip.compare_exchange_weak(cur, target, std::memory_order_release, std::memory_order_relaxed))
			;
	}

	uint64_t Read(uint64_t tsc, uint64_t freq, uint8_t shift)
	{
		Epoch e = m_epoch.Load();
		const uint64_t cap = m_cap.load(std::memory_order_acquire);
		uint64_t skip = m_skip.load(std::memory_order_acquire);
		const bool rate = freq != 0 && (freq != e.freq || shift != e.shift);
		if ((rate || cap != e.cap || skip != 0) && !m_rebasing.exchange(true, std::memory_order_acquire))
		{
			e = m_epoch.Load(); // as it is now, with the flag held
			uint64_t at = At(e, tsc);
			const uint64_t last = m_last.load(std::memory_order_relaxed);
			// Not past the cap: the time that passed since the last read was
			// the old epoch's, and a cap set in it - always ahead of the time
			// then - is where it should have stopped. Unless a read already
			// gave more (the cap was set behind it): never backwards.
			if (at > cap)
				at = cap;
			if (at < last)
				at = last;
			// a skip is for the moment it was asked: taken now, as far as the
			// cap lets, and not again once the cap is raised
			skip = m_skip.exchange(0, std::memory_order_acq_rel);
			if (skip > at)
				at = skip < cap ? skip : (cap > at ? cap : at);
			e = Epoch{tsc, at, freq != 0 ? freq : e.freq, freq != 0 ? shift : e.shift, cap};
			m_epoch.Store(e);
			m_rebasing.store(false, std::memory_order_release);
		}
		const uint64_t t = At(e, tsc);
		uint64_t last = m_last.load(std::memory_order_relaxed);
		while (t > last)
		{
			if (m_last.compare_exchange_weak(last, t, std::memory_order_relaxed))
				return t;
		}
		return last;
	}

private:
	struct Epoch
	{
		uint64_t tsc;
		uint64_t guest;
		uint64_t freq; // 0: no rate yet, the time stands still
		uint64_t shift;
		uint64_t cap;  // the cap the epoch was made with: the time stops there
	};

	static uint64_t At(const Epoch& e, uint64_t tsc)
	{
		if (e.freq == 0 || tsc <= e.tsc || e.guest >= e.cap)
			return e.guest;
		const uint64_t elapsed = MulDiv(tsc - e.tsc, kConsoleClock, e.freq);
		const uint64_t t = e.guest + ((elapsed << 3) >> e.shift);
		return t < e.cap ? t : e.cap;
	}

	// a * b / d with a 128-bit product; the quotient fits for centuries
	static uint64_t MulDiv(uint64_t a, uint64_t b, uint64_t d)
	{
#if defined(__SIZEOF_INT128__)
		return (uint64_t)(((unsigned __int128)a * b) / d);
#else
		uint64_t high;
		const uint64_t low = _umul128(a, b, &high);
		uint64_t remainder;
		return _udiv128(high, low, d, &remainder);
#endif
	}

	SeqlockValue<Epoch> m_epoch;
	std::atomic<uint64_t> m_cap{kNoCap};
	std::atomic<uint64_t> m_skip{0};
	std::atomic<uint64_t> m_last{0};
	std::atomic_bool m_rebasing{false};
};
