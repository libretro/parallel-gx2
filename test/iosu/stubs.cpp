// What iosu_kernel.cpp takes from the rest of the core: a stand-in for guest
// memory the dispatchable commands are allocated in, the log, and the reply
// path to the cores, which the test records
#include <atomic>
#include <cstdlib>
#include <string_view>

#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/IOSU/iosu_ipc_common.h"

uint8* memory_base = nullptr;
uint64 s_loggingFlagMask = 0;
static std::atomic<uint32> s_sysAreaTop{0x1000};

void TestInitGuestMemory()
{
	memory_base = (uint8*)std::calloc(1, 4 * 1024 * 1024);
}

MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment)
{
	uint32 top = s_sysAreaTop.load();
	for (;;)
	{
		uint32 at = (top + alignment - 1) & ~(alignment - 1);
		if (s_sysAreaTop.compare_exchange_weak(top, at + size))
			return at;
	}
}

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

void SetThreadName(const char* name)
{
}

// the test's
void TestOnResponse(uint32 ppcCoreIndex, IPCCommandBody* body);

namespace coreinit
{
	void IPCDriver_NotifyResponses(uint32 ppcCoreIndex, IPCCommandBody** responseArray, uint32 numResponses)
	{
		for (uint32 i = 0; i < numResponses; i++)
			TestOnResponse(ppcCoreIndex, responseArray[i]);
	}
}
