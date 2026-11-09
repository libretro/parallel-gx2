#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <rthreads/retro_eventcount.h>

// Lock-free ring buffer for audio samples.
// Single producer (Cemu AX audio thread), single consumer (retro_run thread,
// or RetroArch's audio thread in Audio Thread mode).
//
// Both sides work in place: the producer converts AX's block straight into the
// ring (Reserve/Commit) and the consumer hands the frontend a pointer into it
// (Peek/Consume), so a sample is written once and read once on its way out.
// Positions only ever move by whole frames of 2 or 6 samples and the size is a
// multiple of both, so a frame never straddles the end and every span handed
// out is whole frames.
class LibretroAudioRingBuffer
{
public:
	// ~341 ms at 48 kHz 5.1 (a second in stereo). It has to hold everything AX may make between
	// two retro_runs: a frame of the guest's time, two at most (the cap runs
	// at most two frames ahead, PPCTimerClock.h), with room to spare. Drained
	// every retro_run, so the size adds no latency.
	static constexpr size_t kBufferFrames = 16384;
	// Up to 5.1; the ring holds whole frames of the output's channels, 2 or
	// 6, both of which divide its size
	static constexpr size_t kChannels = 6;
	static constexpr size_t kBufferSamples = kBufferFrames * kChannels;

	LibretroAudioRingBuffer() = default;

	size_t GetReadAvailableSamples() const
	{
		const size_t w = m_write_pos.load(std::memory_order_acquire);
		const size_t r = m_read_pos.load(std::memory_order_relaxed);
		return (w >= r) ? (w - r) : (kBufferSamples - r + w);
	}

	// Producer: room for up to maxSamples in one piece, at *ptr. What is
	// written there becomes visible with Commit.
	size_t Reserve(int16_t** ptr, size_t maxSamples)
	{
		const size_t w = m_write_pos.load(std::memory_order_relaxed);
		const size_t r = m_read_pos.load(std::memory_order_acquire);
		const size_t used = (w >= r) ? (w - r) : (kBufferSamples - r + w);
		// One frame kept free, so full and empty are told apart
		const size_t room = kBufferSamples - used - kChannels;
		size_t n = room < maxSamples ? room : maxSamples;
		if (n > kBufferSamples - w)
			n = kBufferSamples - w;
		*ptr = m_buffer.data() + w;
		return n;
	}

	void Commit(size_t sampleCount)
	{
		const size_t w = m_write_pos.load(std::memory_order_relaxed);
		m_write_pos.store((w + sampleCount) % kBufferSamples, std::memory_order_release);
	}

	size_t Write(const int16_t* data, size_t sampleCount)
	{
		size_t written = 0;
		while (written < sampleCount)
		{
			int16_t* dst;
			const size_t n = Reserve(&dst, sampleCount - written);
			if (n == 0)
				break;
			std::memcpy(dst, data + written, n * sizeof(int16_t));
			Commit(n);
			written += n;
		}
		return written;
	}

	// Consumer: the oldest buffered samples that lie in one piece, at most
	// maxSamples, at *ptr. They stay valid until Consume.
	size_t Peek(const int16_t** ptr, size_t maxSamples) const
	{
		const size_t w = m_write_pos.load(std::memory_order_acquire);
		const size_t r = m_read_pos.load(std::memory_order_relaxed);
		size_t n = (w >= r) ? (w - r) : (kBufferSamples - r);
		if (n > maxSamples)
			n = maxSamples;
		*ptr = m_buffer.data() + r;
		return n;
	}

	// Drops the oldest samples, from the reading side, so it is safe while
	// the writer goes on (Reset() is not)
	void Consume(size_t sampleCount)
	{
		const size_t w = m_write_pos.load(std::memory_order_acquire);
		const size_t r = m_read_pos.load(std::memory_order_relaxed);
		const size_t available = (w >= r) ? (w - r) : (kBufferSamples - r + w);
		const size_t toSkip = (sampleCount <= available) ? sampleCount : available;
		m_read_pos.store((r + toSkip) % kBufferSamples, std::memory_order_release);
	}

	void Discard(size_t sampleCount)
	{
		Consume(sampleCount);
	}

	size_t Read(int16_t* dest, size_t maxSamples)
	{
		size_t done = 0;
		while (done < maxSamples)
		{
			const int16_t* src;
			const size_t n = Peek(&src, maxSamples - done);
			if (n == 0)
				break;
			std::memcpy(dest + done, src, n * sizeof(int16_t));
			Consume(n);
			done += n;
		}
		return done;
	}

	void Reset()
	{
		m_write_pos.store(0, std::memory_order_relaxed);
		m_read_pos.store(0, std::memory_order_relaxed);
	}

private:
	std::array<int16_t, kBufferSamples> m_buffer{};
	std::atomic<size_t> m_write_pos{0};
	std::atomic<size_t> m_read_pos{0};
};

// What the consumer sleeps on between pulls that found the ring empty (Audio
// Thread mode), instead of a fixed sleep: the producer notifies after a block
// goes in, so the wait ends when the audio arrives. No lock, and a notify with
// nobody waiting is an atomic increment and nothing else.
class LibretroAudioFeedWake
{
public:
	LibretroAudioFeedWake() { m_ok = retro_eventcount_init(&m_ec); }
	~LibretroAudioFeedWake() { if (m_ok) retro_eventcount_free(&m_ec); }
	LibretroAudioFeedWake(const LibretroAudioFeedWake&) = delete;
	LibretroAudioFeedWake& operator=(const LibretroAudioFeedWake&) = delete;

	void Notify()
	{
		if (m_ok)
			retro_eventcount_notify(&m_ec);
	}

	// Until ready() or the deadline - or, with anyWakeEnds, until the first
	// wake, ready or not. Returns ready().
	template<typename TReady>
	bool WaitUntil(TReady ready, std::chrono::steady_clock::time_point deadline, bool anyWakeEnds)
	{
		for (;;)
		{
			if (ready())
				return true;
			const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
				deadline - std::chrono::steady_clock::now()).count();
			if (left <= 0 || !m_ok)
				return false;
			const int key = retro_eventcount_prepare_wait(&m_ec);
			if (ready())
			{
				retro_eventcount_cancel_wait(&m_ec);
				return true;
			}
			retro_eventcount_commit_wait_timeout(&m_ec, key, left);
			if (anyWakeEnds)
				return ready();
		}
	}

private:
	retro_eventcount_t m_ec{};
	bool m_ok;
};
