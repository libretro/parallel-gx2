#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

// the unsupported calls already logged, by a hash of their name: a fixed
// set any thread adds to with a compare-and-swap. Full, a call is logged
// again each time
static constexpr size_t UNSUPPORTED_SEEN_SLOTS = 512;
static std::atomic<uint64> s_unsupportedSeen[UNSUPPORTED_SEEN_SLOTS];

// true the first time name is seen
static bool FirstUnsupported(std::string_view name)
{
	uint64 h = 0xcbf29ce484222325ull;
	for (char c : name)
		h = (h ^ (uint8)c) * 0x100000001b3ull;
	if (h == 0)
		h = 1; // 0 is a free slot
	for (size_t i = 0; i < UNSUPPORTED_SEEN_SLOTS; i++)
	{
		std::atomic<uint64>& slot = s_unsupportedSeen[(h + i) & (UNSUPPORTED_SEEN_SLOTS - 1)];
		uint64 v = slot.load(std::memory_order_acquire);
		if (v == h)
			return false;
		if (v == 0)
		{
			uint64 expected = 0;
			if (slot.compare_exchange_strong(expected, h, std::memory_order_acq_rel))
				return true;
			if (expected == h)
				return false;
		}
	}
	return true;
}

void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	const char* libFuncName = "<unknown>";
	if (memory_isAddressRangeAccessible(hCPU->instructionPointer + 8, 1))
		libFuncName = (char*)memory_getPointerFromVirtualOffset(hCPU->instructionPointer + 8);
	if (FirstUnsupported(libFuncName))
		cemuLog_log(LogType::UnsupportedAPI, "Unsupported lib call: {}", libFuncName);
	// Return a failure result and immediately return to the caller.
	// Advancing to the next instruction inside the trampoline (e.g. BLR) can lead to stalls if the interpreter/recompiler
	// re-enters the unsupported stub. Returning via LR matches how other HLE calls return.
	hCPU->gpr[3] = 0;
	hCPU->instructionPointer = hCPU->spr.LR;
}

// Registered from any thread, read by every one running guest code: an
// index taken with an atomic add, the call published into it after. Two
// threads registering one call at once may each get an index; both work.
// No lock - where the readers read it with none while it was written.
static constexpr size_t HLE_TABLE_CAPACITY = PPC_HLE_TABLE_CAPACITY;
static std::atomic<HLECALL> s_ppcHleTable[HLE_TABLE_CAPACITY]{};
static std::atomic<sint32> s_ppcHleTableWriteIndex{0};

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string_view /*hleName*/)
{
	const sint32 count = std::min<sint32>(s_ppcHleTableWriteIndex.load(std::memory_order_acquire), (sint32)HLE_TABLE_CAPACITY);
	for (sint32 i = 0; i < count; i++)
	{
		if (s_ppcHleTable[i].load(std::memory_order_acquire) == hleCall)
			return i;
	}
	const sint32 funcIndex = s_ppcHleTableWriteIndex.fetch_add(1, std::memory_order_acq_rel);
	if (funcIndex >= (sint32)HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
		return 0;
	}
	s_ppcHleTable[funcIndex].store(hleCall, std::memory_order_release);
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex].load(std::memory_order_acquire);
}

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
		hleCall(hCPU);
	}
}