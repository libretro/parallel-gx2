#include <cstdlib>
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "PPCRecompilerWorkQueue.h"
#include "rec_store.h"
#include "util/helpers/lf_ring.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"

#define PPCREC_LOG_RECOMPILATION_RESULTS		0

// Buckets of the function store, by 0x2000 of guest code
static constexpr size_t kRecStoreBuckets = 7703;
// Functions ended by an invalidation, for the recompiler thread to take out
// of the store (it alone changes it)
static constexpr unsigned kRecDeadCap = 4096;

struct
{
	std::atomic_bool initialized{false};
	PPCRecompilerWorkQueue targetQueue; // pushed by any core, popped by the recompiler thread
	std::atomic_int_fast32_t recompilerEnableCount{0};
	// recompiler thread
	std::thread workerThread;
	std::atomic_bool workerThreadStopSignal{false};
	// The recompiled functions by the code they cover, and the ranges
	// invalidated lately (rec_store.h): no lock between the recompiler thread
	// and the cores that invalidate
	retro_atomic_ptr_t storeHeads[kRecStoreBuckets];
	rec_store_t store;
	rec_inval_t inval;
	lf_ring_t dead;
	alignas(8) unsigned char deadCells[kRecDeadCap * LF_RING_STRIDE(sizeof(PPCRecFunction_t*))];
	// the recompiler thread's: every function it put in the store
	PPCRecFunction_t* allFunctions;
}s_ppcRecompilerState;

// The jump table, which the recompiled code reads with plain loads, changed
// by any core and the recompiler thread at once
static inline PPCREC_JUMP_ENTRY* PPCRecompiler_jumpEntry(uint32 address)
{
	return &ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4];
}

static inline PPCREC_JUMP_ENTRY PPCRecompiler_jumpLoad(uint32 address)
{
	return __atomic_load_n(PPCRecompiler_jumpEntry(address), __ATOMIC_ACQUIRE);
}

static inline void PPCRecompiler_jumpStore(uint32 address, PPCREC_JUMP_ENTRY value)
{
	__atomic_store_n(PPCRecompiler_jumpEntry(address), value, __ATOMIC_SEQ_CST);
}

static inline bool PPCRecompiler_jumpSwap(uint32 address, PPCREC_JUMP_ENTRY expected, PPCREC_JUMP_ENTRY desired)
{
	return __atomic_compare_exchange_n(PPCRecompiler_jumpEntry(address), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

void PPCRecompiler_recompileAtAddress(uint32 address);

static bool PPCRecompiler_IsLibretroForceInterpreterEnabled()
{
	static int s_cached = -1;
	if (s_cached == -1)
	{
		const char* env = std::getenv("CEMU_LIBRETRO_FORCE_INTERPRETER");
		s_cached = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
	}
	return s_cached != 0;
}


// Never blocks: an address the interpreter ran is marked visited and queued
// for the recompiler thread, by whichever core claims it first
void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
	// quick read-only check
	if (PPCRecompiler_jumpLoad(enterAddress) != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	if (!PPCRecompiler_jumpSwap(enterAddress, PPCRecompiler_leaveRecompilerCode_unvisited, PPCRecompiler_leaveRecompilerCode_visited))
		return; // another core has it
	// queue full: unvisited again, offered again later
	if (!s_ppcRecompilerState.targetQueue.Push(enterAddress))
		PPCRecompiler_jumpSwap(enterAddress, PPCRecompiler_leaveRecompilerCode_visited, PPCRecompiler_leaveRecompilerCode_unvisited);
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	PPCRecompiler_visitAddressNoBlock(enterAddress);
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
#if BOOST_OS_WINDOWS
	uint32 prevCsr = _mm_getcsr();
	if (prevCsr & _MM_ROUND_MASK) [[unlikely]]
		_mm_setcsr(prevCsr & ~_MM_ROUND_MASK);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	if (prevCsr & _MM_ROUND_MASK) [[unlikely]]
		_mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | (prevCsr & _MM_ROUND_MASK));
	// debug recompiler exit - useful to find frequently executed functions which couldn't be recompiled
	#ifdef CEMU_DEBUG_ASSERT
	if (hCPU->remainingCycles > 0 && GetAsyncKeyState(VK_F4))
	{
		auto t = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
		cemuLog_log(LogType::Force, "Recompiler exit: 0x{:08x} LR: 0x{:08x} Timestamp {}.{:04}", hCPU->instructionPointer, hCPU->spr.LR, dur / 1000LL, (dur % 1000LL));
	}
	#endif
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif
	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	auto funcPtr = PPCRecompiler_jumpLoad(enterAddress);
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	if (hCPU->remainingCycles <= 0)
		return;
	auto funcPtr = PPCRecompiler_jumpLoad(enterAddress);
	if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompiler_visitAddressNoBlock(enterAddress);
	}
	else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		// enter
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}
bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);

PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker)
{
	if (range.startAddress >= PPC_REC_CODE_AREA_END)
	{
		cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
		return nullptr;
	}
	uint32 codeGenRangeStart;
	uint32 codeGenRangeSize = 0;
	coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
	if (codeGenRangeSize != 0)
	{
		if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
		{
			if (coreinit::codeGenShouldAvoid())
			{
				return nullptr;
			}
		}
	}

	PPCRecFunction_t* ppcRecFunc = new PPCRecFunction_t();
	ppcRecFunc->ppcAddress = range.startAddress;
	ppcRecFunc->ppcSize = range.length;

#if PPCREC_LOG_RECOMPILATION_RESULTS
	BenchmarkTimer bt;
	bt.Start();
#endif

	// generate intermediate code
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	bool compiledSuccessfully = PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker);
	if (compiledSuccessfully == false)
	{
		delete ppcRecFunc;
		return nullptr;
	}

	uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
	uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();

	if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
	{
		if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
		{
			delete ppcRecFunc;
			return nullptr;
		}
	}

	// apply passes
	if (!PPCRecompiler_ApplyIMLPasses(ppcImlGenContext))
	{
		delete ppcRecFunc;
		return nullptr;
	}

#if defined(ARCH_X86_64)
	// emit x64 code
	bool x64GenerationSuccess = PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext);
	if (x64GenerationSuccess == false)
	{
		return nullptr;
	}
#elif defined(__aarch64__)
	bool aarch64GenerationSuccess = PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext);
	if (aarch64GenerationSuccess == false)
	{
		return nullptr;
	}
#endif
	if (ActiveSettings::DumpRecompilerFunctionsEnabled())
	{
		FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
		if (fs)
		{
			fs->writeData(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
			delete fs;
		}
	}

	// collect list of PPC-->x64 entry points
	entryPointsOut.clear();
	for(IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
	{
		if (imlSegment->isEnterable == false)
			continue;

		uint32 ppcEnterOffset = imlSegment->enterPPCAddress;
		uint32 x64Offset = imlSegment->x64Offset;

		entryPointsOut.emplace_back(ppcEnterOffset, x64Offset);
	}

#if PPCREC_LOG_RECOMPILATION_RESULTS
	bt.Stop();
	uint32 codeHash = 0;
	for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
	{
		codeHash = _rotr(codeHash, 3);
		codeHash += ((uint8*)ppcRecFunc->x86Code)[i];
	}
	cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> x64: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}", (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code, bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

	return ppcRecFunc;
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLRegisterAllocatorParameters raParam;

	for (auto& it : ppcImlGenContext.mappedRegs)
		raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);

#if defined(ARCH_X86_64)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R12);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);

	// add XMM registers, except XMM15 which is the temporary register
	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 0);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 1);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 2);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 3);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 4);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 5);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 6);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 7);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 8);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 9);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 10);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 11);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 12);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 13);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 14);
#elif defined(__aarch64__)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
	{
		if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
			continue; // Skip reserved platform register
		gprPhysPool.SetAvailable(i);
	}

	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
		fprPhysPool.SetAvailable(i);
#endif

	IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
	// isolate entry points from function flow (enterable segments must not be the target of any other segment)
	// this simplifies logic during register allocation
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);

	// merge certain float load+store patterns
	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	// delay byte swapping for certain load+store patterns
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);

	IMLOptimizer_OptimizeSinglePrecisionStores(ppcImlGenContext);

	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);

	return true;
}

// Ends a recompiled function, from any thread: its entries in the jump table
// go back to unvisited (those still its own), once, and the recompiler thread
// is handed it to take out of the store
static void PPCRecompiler_endFunction(PPCRecFunction_t* func)
{
	if (func->dead.exchange(true))
		return;
	for (auto& entrypoint : func->jumpTableEntries)
		PPCRecompiler_jumpSwap(entrypoint.ppcAddr, (PPCREC_JUMP_ENTRY)entrypoint.hostEntrypoint, PPCRecompiler_leaveRecompilerCode_unvisited);
	// taken out with the recompiler thread's next work; full, it stays in
	// the store, ended, until the recompiler shuts down
	lf_ring_push(&s_ppcRecompilerState.dead, &func);
}

// The recompiler thread's: func's ranges in the store, or out of it
static void PPCRecompiler_storeFunction(PPCRecFunction_t* func)
{
	for (auto& r : func->list_ranges)
	{
		const size_t n = rec_store_nodes(&s_ppcRecompilerState.store, r.ppcAddress, r.ppcAddress + r.ppcSize);
		rec_node_t* nodes = new rec_node_t[n];
		rec_store_insert(&s_ppcRecompilerState.store, nodes, func, r.ppcAddress, r.ppcAddress + r.ppcSize);
		r.storedRange = nodes;
	}
	func->nextFunction = s_ppcRecompilerState.allFunctions;
	s_ppcRecompilerState.allFunctions = func;
}

static void PPCRecompiler_unstoreFunction(PPCRecFunction_t* func)
{
	for (auto& r : func->list_ranges)
	{
		if (!r.storedRange)
			continue;
		rec_node_t* nodes = (rec_node_t*)r.storedRange;
		rec_store_remove(&s_ppcRecompilerState.store, nodes, nodes[0].group);
		r.storedRange = nullptr;
	}
}

static void PPCRecompiler_freeNodes(rec_node_t* first, void*)
{
	delete[] first;
}

// The recompiler thread's: the functions ended since it last looked taken
// out of the store, and the nodes no look can still be on freed
static void PPCRecompiler_sweepEnded()
{
	PPCRecFunction_t* func;
	while (lf_ring_pop(&s_ppcRecompilerState.dead, &func))
		PPCRecompiler_unstoreFunction(func);
	rec_store_collect(&s_ppcRecompilerState.store, PPCRecompiler_freeNodes, nullptr);
}

static bool PPCRecompiler_rangesInvalidatedSince(PPCRecFunction_t* func, int since)
{
	for (auto& r : func->list_ranges)
	{
		if (rec_inval_since(&s_ppcRecompilerState.inval, since, r.ppcAddress, r.ppcAddress + r.ppcSize))
			return true;
	}
	return false;
}

// With no lock against the cores invalidating code: the function goes in the
// store before the log of invalidations is read, and an invalidation goes in
// the log before it looks in the store, so one of the two sees the other. One
// that comes after the jump table is written ends the function itself.
bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints, int invalSince)
{
	// check if the initial entrypoint is still flagged for recompilation
	// its possible that the range has been invalidated during the time it took to translate the function
	if (PPCRecompiler_jumpLoad(initialEntryPoint) != PPCRecompiler_leaveRecompilerCode_visited)
		return false;

	// the entries it will take, settled before anyone can end it
	cemu_assert_debug(ppcRecFunc->jumpTableEntries.empty());
	for (auto& itr : entryPoints)
		ppcRecFunc->jumpTableEntries.emplace_back(itr.first, ((uint8*)ppcRecFunc->x86Code + itr.second));

	PPCRecompiler_storeFunction(ppcRecFunc);

	// invalidated while it was translated: out again, never used
	if (PPCRecompiler_rangesInvalidatedSince(ppcRecFunc, invalSince))
	{
		ppcRecFunc->dead.store(true);
		PPCRecompiler_unstoreFunction(ppcRecFunc);
		return false;
	}

	// update jump table
	for (auto& e : ppcRecFunc->jumpTableEntries)
		PPCRecompiler_jumpStore(e.ppcAddr, (PPCREC_JUMP_ENTRY)e.hostEntrypoint);

	// due to inlining, some entrypoints can get optimized away
	// therefore we reset all addresses that are still marked as visited (but not recompiled)
	// we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
	// if they are reachable, the interpreter will queue them again
	for (uint32 v = range.startAddress; v < (range.startAddress + range.length); v += 4)
		PPCRecompiler_jumpSwap(v, PPCRecompiler_leaveRecompilerCode_visited, PPCRecompiler_leaveRecompilerCode_unvisited);

	// ended by an invalidation that found it in the store before the jump
	// table was written: those entries go again
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (ppcRecFunc->dead.load())
	{
		for (auto& e : ppcRecFunc->jumpTableEntries)
			PPCRecompiler_jumpSwap(e.ppcAddr, (PPCREC_JUMP_ENTRY)e.hostEntrypoint, PPCRecompiler_leaveRecompilerCode_unvisited);
		return false;
	}
	return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(PPCRecompiler_jumpLoad(address) == PPCRecompiler_leaveRecompilerCode_visited);

	// invalidations from here on can touch what is about to be read
	const int invalSince = rec_inval_now(&s_ppcRecompilerState.inval);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	std::set<uint32> entryAddresses;
	entryAddresses.emplace(address);

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	PPCRecFunction_t* func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries);
	if (!func)
		return; // recompilation failed
	PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints, invalSince);
}

void PPCRecompiler_thread()
{
	SetThreadName("PPCRecompiler");

	while (true)
	{
		// Asleep until an address is pushed or the thread is stopped: a push
		// is answered at once instead of at the next 10 ms poll.
		s_ppcRecompilerState.targetQueue.WaitForWork(s_ppcRecompilerState.workerThreadStopSignal);
		if (s_ppcRecompilerState.workerThreadStopSignal)
			return;
		PPCRecompiler_sweepEnded();
		// asynchronous recompilation:
		// 1) take address from queue
		// 2) check if address is still marked as visited
		// 3) if yes -> calculate size, gather all entry points, recompile and update jump table
		uint32 enterAddress;
		while (s_ppcRecompilerState.targetQueue.Pop(enterAddress))
		{
			// A stale answer either skips an address the interpreter will
			// offer again, or translates one that makeRecompiledFunctionActive
			// then refuses.
			if (PPCRecompiler_jumpLoad(enterAddress) != PPCRecompiler_leaveRecompilerCode_visited)
				continue; // only recompile functions if marked as visited
			PPCRecompiler_recompileAtAddress(enterAddress);
			PPCRecompiler_sweepEnded();
			if(s_ppcRecompilerState.workerThreadStopSignal)
				return;
		}
	}
}

#define PPC_REC_ALLOC_BLOCK_SIZE	(4*1024*1024) // 4MB

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

// Recompiled code reads ppcRecompilerDirectJumpTable[address/4] on every branch
// it cannot resolve at compile time, and nothing checks that the entry is there.
// A fault inside the table therefore says which guest address was branched to
// and whether that part of the table was ever mapped - worth spelling out in a
// crash log, because the address on its own looks like any other wild pointer.
bool PPCRecompiler_lookupTableFaultInfo(uintptr_t address, PPCRecLookupTableFault& infoOut)
{
	if (!ppcRecompilerInstanceData)
		return false;
	const uintptr_t tableStart = (uintptr_t)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
	const uintptr_t tableEnd = tableStart + sizeof(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable);
	if (address < tableStart || address >= tableEnd)
		return false;
	const uint64 entryIndex = (address - tableStart) / sizeof(PPCREC_JUMP_ENTRY);
	infoOut.ppcAddress = (uint32)(entryIndex * 4);
	const uint32 blockIndex = infoOut.ppcAddress / PPC_REC_ALLOC_BLOCK_SIZE;
	infoOut.blockReserved = blockIndex < PPCRecompiler_GetNumAddressSpaceBlocks() && ppcRecompiler_reservedBlockMask[blockIndex];
	return true;
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

extern "C" DLLEXPORT uintptr_t* PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

static void PPCRecompiler_endInRange(void* obj, uint32, uint32, void*)
{
	PPCRecompiler_endFunction((PPCRecFunction_t*)obj);
}

// From any core: the recompiled functions that cover [startAddr, endAddr)
// ended, and one being translated from it refused when it is done
void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (!s_ppcRecompilerState.initialized)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);
	rec_inval_add(&s_ppcRecompilerState.inval, startAddr, endAddr);
	rec_store_find(&s_ppcRecompilerState.store, startAddr, endAddr, PPCRecompiler_endInRange, nullptr);
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_constDouble2p149[0] = 0x1p149;
	ppcRecompilerInstanceData->_x64XMM_constDouble2p149[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;

	// mxcsr
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn = 0x1F80 | 0x8000;
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
	ppcRecompilerInstanceData->_x64_pextSingleMask = 0xC7FFFFFFE0000000ULL; // sign, exponent bit 62, then bits 58-29
}
#else
void PPCRecompiler_initPlatform()
{

}
#endif

void PPCRecompiler_init()
{
	s_ppcRecompilerState.recompilerEnableCount = 0;
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		cemuLog_log(LogType::Force, "Using singlecore interpreter");
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}
	if (PPCRecompiler_IsLibretroForceInterpreterEnabled())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. CEMU_LIBRETRO_FORCE_INTERPRETER is set");
		return;
	}
	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
		// The committed blocks of the lookup table went with the reservation, so
		// nothing may still be marked as reserved: a block left marked here is
		// one that PPCRecompiler_reserveLookupTableBlock will skip, leaving that
		// part of the table unmapped in the new reservation while recompiled
		// code reads it anyway. That is a fault at
		// ppcRecompilerDirectJumpTable[address/4] on the first branch into the
		// range - which is the whole of a title's own code, on the second title
		// a process runs.
		ppcRecompiler_reservedBlockMask.reset();
	}
	cemuLog_logDebug(LogType::Force, "Reserving {}MB for recompiler instance data", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    PPCRecompiler_allocateRange(0, 0x1000); // the first entry is used for fallback to interpreter
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

    PPCRecompiler_initPlatform();

	cemuLog_log(LogType::Force, "Recompiler initialized");

	s_ppcRecompilerState.initialized = true;
	s_ppcRecompilerState.recompilerEnableCount = 1; // enabled

	// launch recompilation thread
    s_ppcRecompilerState.workerThreadStopSignal = false;
    rec_store_init(&s_ppcRecompilerState.store, s_ppcRecompilerState.storeHeads, kRecStoreBuckets, 0x2000);
    rec_inval_init(&s_ppcRecompilerState.inval);
    memset(s_ppcRecompilerState.deadCells, 0, sizeof(s_ppcRecompilerState.deadCells));
    lf_ring_init(&s_ppcRecompilerState.dead, s_ppcRecompilerState.deadCells, kRecDeadCap, sizeof(PPCRecFunction_t*));
    s_ppcRecompilerState.allFunctions = nullptr;
    if (!s_ppcRecompilerState.targetQueue.Init())
        cemuLog_log(LogType::Force, "Recompiler: no work queue, everything stays interpreted");
    s_ppcRecompilerState.workerThread = std::thread(PPCRecompiler_thread);
}

void PPCRecompiler_Shutdown()
{
    // Nothing to take down, and the memory below has not been mapped yet. The
    // worker thread only exists once initialized is set, so this also means
    // there is no thread outstanding.
    if(!s_ppcRecompilerState.initialized)
        return;
    // shut down recompiler thread
    s_ppcRecompilerState.workerThreadStopSignal = true;
    s_ppcRecompilerState.targetQueue.Wake();
    if(s_ppcRecompilerState.workerThread.joinable())
        s_ppcRecompilerState.workerThread.join();
    // clean up queues
    s_ppcRecompilerState.targetQueue.Clear();
    s_ppcRecompilerState.targetQueue.Free();
    // the store emptied: nothing looks in it with the cores stopped. The
    // functions themselves stay, as their code does (never freed).
    {
        PPCRecFunction_t* func;
        while (lf_ring_pop(&s_ppcRecompilerState.dead, &func))
            ;
        for (func = s_ppcRecompilerState.allFunctions; func; func = func->nextFunction)
            PPCRecompiler_unstoreFunction(func);
        s_ppcRecompilerState.allFunctions = nullptr;
        rec_store_collect(&s_ppcRecompilerState.store, PPCRecompiler_freeNodes, nullptr);
    }
    // clean up memory
    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for(uint32 i=0; i<numBlocks; i++)
    {
        if(!ppcRecompiler_reservedBlockMask[i])
            continue;
        // deallocate
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        // mark as unmapped
        ppcRecompiler_reservedBlockMask[i] = false;
    }
	s_ppcRecompilerState.recompilerEnableCount = 0;
	s_ppcRecompilerState.initialized = false;
}

// For each Enable call, Disable needs to be called once and vice versa
void PPCRecompiler_Enable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount++;
}

void PPCRecompiler_Disable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount--;
}