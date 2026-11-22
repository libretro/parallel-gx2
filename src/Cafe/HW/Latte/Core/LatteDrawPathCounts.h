#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// What the first draw of each new pipeline met (docs/SHADER-STUTTER.md, M2:
// none may wait for a compile), and whether a read had queued that pipeline
// ahead (LatteShaderAhead.cpp) - and made it, handed over whole: linked or
// loaded from what was there, skipped
// until a compile thread made it, or made while it waited (Async Shader
// Compile off, or a draw it may not skip). Then the draws skipped for a
// pipeline not made yet, and the waits of those that may not be skipped for
// the compile thread making theirs. Logged as the title stops, the numbers a
// tester's log gives to judge M2 by. The GPU thread's alone: plain counters.
struct LatteDrawPathCounts
{
	enum Outcome
	{
		Ready,
		Skipped,
		Waited,
		kOutcomes
	};

	uint64_t pipelines[kOutcomes]{};
	uint64_t ahead[kOutcomes]{}; // of those, the ones a read queued ahead
	uint64_t handedOver{}; // ready ones a read made, taken whole
	uint64_t handedOverLate{}; // skipped ones a read made, taken whole after
	uint64_t skippedDraws{};
	uint64_t awaited{};
	uint64_t waitedUs{}, longestUs{}; // every wait above, made or awaited

	void NotePipeline(Outcome outcome, bool queuedAhead, uint64_t us)
	{
		pipelines[outcome]++;
		if (queuedAhead)
			ahead[outcome]++;
		if (outcome == Waited)
			NoteWait(us);
	}

	void NoteAwait(uint64_t us)
	{
		awaited++;
		NoteWait(us);
	}

	void NoteSkippedDraw() { skippedDraws++; }

	void NoteHandedOver() { handedOver++; }

	void NoteHandedOverLate() { handedOverLate++; }

	bool Any() const
	{
		return pipelines[Ready] || pipelines[Skipped] || pipelines[Waited] || skippedDraws || awaited;
	}

	// the log line, cut to room; false with nothing to tell
	bool Describe(char* to, size_t room) const
	{
		if (!Any() || room == 0)
			return false;
		snprintf(to, room,
			"new pipelines at their first draw: %llu ready (%llu queued ahead, %llu of them made by the read), %llu skipped until made (%llu queued ahead, %llu of them then made by the read), "
			"%llu made while their draw waited (%llu queued ahead); %llu draws skipped, %llu waited for a compile thread; "
			"%llu.%03llu ms waited, the longest %llu.%03llu ms",
			ULL(pipelines[Ready]), ULL(ahead[Ready]), ULL(handedOver), ULL(pipelines[Skipped]), ULL(ahead[Skipped]), ULL(handedOverLate),
			ULL(pipelines[Waited]), ULL(ahead[Waited]), ULL(skippedDraws), ULL(awaited),
			ULL(waitedUs / 1000), ULL(waitedUs % 1000), ULL(longestUs / 1000), ULL(longestUs % 1000));
		return true;
	}

	void Reset() { *this = LatteDrawPathCounts{}; }

private:
	static unsigned long long ULL(uint64_t v) { return (unsigned long long)v; }

	void NoteWait(uint64_t us)
	{
		waitedUs += us;
		if (us > longestUs)
			longestUs = us;
	}
};
