// What PPCInterpreterHLE.cpp calls in the rest of the core, for
// hle_table_test: the unsupported-call path it also holds, never reached.
#include "Cafe/HW/Espresso/PPCState.h"

bool memory_isAddressRangeAccessible(MPTR, uint32)
{
	return false;
}

uint8* memory_getPointerFromVirtualOffset(uint32)
{
	return nullptr;
}

bool cemuLog_log(LogType, std::string_view)
{
	return false;
}

uint64 s_loggingFlagMask = 0;
