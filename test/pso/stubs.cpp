// What FileCache.cpp takes from the rest of the core
#include <chrono>
#include <string_view>

#include "Cemu/Logging/CemuLogging.h"

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

void SetThreadName(const char* name)
{
}

#ifndef _WIN32
uint32_t GetTickCount()
{
	return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif
