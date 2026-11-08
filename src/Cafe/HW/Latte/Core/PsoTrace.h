#pragma once

// The shader/PSO trace (docs/SHADER-STUTTER.md): with PSO_TRACE=1 on the
// make line, every piece of shader or pipeline work done on the GPU thread
// while a draw waits is logged with its frame, what it was, its key and how
// long it took - so a stutter is attributed to a cause rather than guessed
// at. Release builds have none of it.
#ifdef PSO_TRACE
#include <chrono>
#include "Cafe/HW/Latte/Core/Latte.h"

class PsoTraceScope
{
public:
	PsoTraceScope(const char* what, uint64 key) : m_what(what), m_key(key), m_start(std::chrono::steady_clock::now()) {}
	~PsoTraceScope()
	{
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
		cemuLog_log(LogType::Force, "PSO_TRACE frame {} {} {:016x} {:.3f} ms", LatteGPUState.frameCounter, m_what, m_key, ms);
	}

private:
	const char* m_what;
	uint64 m_key;
	std::chrono::steady_clock::time_point m_start;
};

#define PSO_TRACE_CONCAT2(a, b) a##b
#define PSO_TRACE_CONCAT(a, b) PSO_TRACE_CONCAT2(a, b)
// what was done, and its key, from here to the end of the scope
#define PSO_TRACE_SCOPE(what, key) PsoTraceScope PSO_TRACE_CONCAT(psoTraceScope, __LINE__)(what, (uint64)(key))
// something that happened at once (a draw skipped while its pipeline compiles)
#define PSO_TRACE_EVENT(what, key) cemuLog_log(LogType::Force, "PSO_TRACE frame {} {} {:016x}", LatteGPUState.frameCounter, what, (uint64)(key))
#else
#define PSO_TRACE_SCOPE(what, key) ((void)0)
#define PSO_TRACE_EVENT(what, key) ((void)0)
#endif
