#pragma once

#include <cstdint>

// The read ahead's own decisions (LatteShaderAhead.cpp), apart from the
// reads, the shaders and the GPU so that test/shader_ahead checks them: where
// a read got to, when its registers are offered for the guest's buffer, how
// many buffers the ring's reads pass over, and which pipelines are made ahead.

// The ring's words a read takes after its buffer: of peeked words copied,
// whole those making whole packets; the write index they end at (endAt,
// from the peek) where they are everything the ring held, noEnd otherwise
inline uint32_t LatteAhead_RingEnd(uint32_t peeked, uint32_t whole, uint32_t endAt, uint32_t noEnd)
{
	return whole == peeked ? endAt : noEnd;
}

// Where a read of the ring's buffer seq got to, its scans done: the buffer
// (bufferDone: read to its end), then, where it was, ringCount words of the
// ring after it (ringDone: read to their end; ringCalls the buffers they call,
// called those of them read to their end)
struct LatteAheadReadEnd
{
	uint32_t readTo; // the last of the ring's buffers read to its end; 0: none
	bool reachedEnd; // read through everything it was given
};
inline LatteAheadReadEnd LatteAhead_ReadEnd(uint32_t seq, bool bufferDone, uint32_t ringCount, bool ringDone, uint32_t ringCalls,
	uint32_t called)
{
	if (!bufferDone)
		return {0, false};
	if (ringCount == 0)
		return {seq, true};
	return {seq + (ringDone ? ringCalls : called), ringDone};
}

// A read's registers offered for the buffer the guest writes next: where it
// read through everything the ring held, and the guest has submitted nothing
// since (the ring's write index now still the one the read ended at)
inline bool LatteAhead_OfferRingEnd(bool reachedEnd, uint32_t ringEnd, uint32_t writeNow, uint32_t noEnd)
{
	return reachedEnd && ringEnd != noEnd && writeNow == ringEnd;
}

// How many of the ring's buffers are passed over before the next read, as a
// power of two (LatteAhead_Skip): back to none once a read finds anything,
// one more for each that finds nothing, up to kMaxBackoff. A read of the
// guest's buffer (seq 0), read again from its start as it grows, moves it
// neither way.
constexpr uint32_t kLatteAheadMaxBackoff = 6;
inline uint32_t LatteAhead_Backoff(uint32_t backoff, uint32_t seq, uint32_t found)
{
	if (seq == 0)
		return backoff;
	if (found)
		return 0;
	return backoff < kLatteAheadMaxBackoff ? backoff + 1 : kLatteAheadMaxBackoff;
}
inline uint32_t LatteAhead_Skip(uint32_t backoff)
{
	return (1u << backoff) - 1;
}

// Whether a draw has its pipeline made ahead, by what the read knows of its
// shaders: both the GPU thread's (registered) - queued where neither the GPU
// thread has the pipeline nor a read queued it (known, ahead: of its key,
// worked out then) - or one or both just made ahead by this read (vsNew,
// psNew) with the other there by the time: registered, or new too
inline bool LatteAhead_QueuePipeline(bool vsNew, bool psNew, bool vsRegistered, bool psRegistered, bool known, bool ahead)
{
	if (!vsNew && !psNew)
		return vsRegistered && psRegistered && !known && !ahead;
	return (vsNew || vsRegistered) && (psNew || psRegistered);
}

// Whether a pipeline is made as its read's slot starts, its key worked out
// then: not where another read has it, unless it is the key this one was
// queued with - and never where the GPU thread has it (known), queued with
// it or not: its draw came first and compiles it in the lane before the
// slot's, ahead of all it holds, so the slot's would come after, made again
inline bool LatteAhead_MakePipeline(uint64_t queuedHash, uint64_t hash, bool known, bool ahead)
{
	return !known && (queuedHash == hash || !ahead);
}

// A shader program a read copied from guest memory for a draw, as the draw
// before it had it: the same address and size are the same program - the
// copy still in the read's buffer, its hash the one worked out then - as
// the GPU thread takes them (_calculateShaderProgramHash); forgotten as
// each read starts, and where a copy fails
struct LatteAheadProgramMemo
{
	uint32_t addr = 0, size = 0;
	uint64_t h1 = 0, h2 = 0;
	bool valid = false;

	bool Same(uint32_t a, uint32_t s) const { return valid && a == addr && s == size; }
	void Note(uint32_t a, uint32_t s, uint64_t x1, uint64_t x2)
	{
		addr = a;
		size = s;
		h1 = x1;
		h2 = x2;
		valid = true;
	}
	void Forget() { valid = false; }
};
