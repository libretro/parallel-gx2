// What the interpreter calls in the rest of the core, for ppc_trap_test:
// none of it is reached by the instructions the test runs, but the
// interpreter's object names it.
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"

#include <cstdlib>

uint8* memory_base;
uint64 ppcMainThreadDECCycleValue;
uint64 ppcMainThreadDECCycleStart;

void PPCInterpreter_nextInstruction(PPCInterpreter_t* hCPU) { hCPU->instructionPointer += 4; }
void PPCInterpreter_jumpToInstruction(PPCInterpreter_t* hCPU, uint32 ip) { hCPU->instructionPointer = ip; }
uint32 PPCInterpreter_getXER(PPCInterpreter_t* hCPU) { return hCPU->spr.XER; }
void PPCInterpreter_setXER(PPCInterpreter_t* hCPU, uint32 v) { hCPU->spr.XER = v; }
uint64 PPCInterpreter_getMainCoreCycleCounter() { return 0; }
void PPCInterpreter_virtualHLE(PPCInterpreter_t*, unsigned int) { abort(); }
void PPCRecompiler_attemptEnter(PPCInterpreter_t*, uint32) {}
void LatteBufferCache_notifyDCFlush(uint32, uint32) {}
uint8* memory_getPointerFromVirtualOffset(uint32 offset) { return memory_base + offset; }
namespace coreinit { void codeGenHandleICBI(uint32) {} }
bool cemuLog_log(LogType, std::string_view) { return true; }
