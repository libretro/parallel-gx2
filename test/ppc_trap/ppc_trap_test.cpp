// The interpreter's tw, run through the interpreter
// (src/Cafe/HW/Espresso/Interpreter/PPCInterpreterImpl.cpp) as a title runs
// it: not taken, on to the next instruction, whatever its condition and
// registers. tw 0 with r1 called a GDB stub the core never makes (a null
// pointer), with r2 entered a debugger the core does not have, and with any
// other register ran again forever.
#include "Cafe/HW/Espresso/PPCState.h"

#include <cstdio>
#include <vector>

void PPCInterpreterSlim_executeInstruction(PPCInterpreter_t* hCPU);

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static constexpr uint32 kCode = 0x02000000;
static std::vector<uint8> s_ram(0x10000);

// what the interpreter needs of the rest of the core (stubs.cpp)
extern uint8* memory_base;

static uint32 tw(uint32 to, uint32 rA, uint32 rB)
{
	return (31u << 26) | (to << 21) | (rA << 16) | (rB << 11) | (4u << 1);
}

static void put(uint32 address, uint32 v)
{
	uint8* p = memory_base + address;
	p[0] = (uint8)(v >> 24); p[1] = (uint8)(v >> 16); p[2] = (uint8)(v >> 8); p[3] = (uint8)v;
}

int main()
{
	static PPCInterpreter_t cpu;
	const uint32 cases[][3] = {
		{0, 1, 0},  // the GDB stub's breakpoint
		{0, 2, 0},  // the debugger's
		{0, 5, 7},  // trap never, any register
		{31, 0, 0}, // trap always
		{4, 3, 4},  // trap if equal
	};
	memory_base = s_ram.data() - kCode;
	for (const auto& c : cases)
	{
		put(kCode, tw(c[0], c[1], c[2]));
		cpu.instructionPointer = kCode;
		cpu.gpr[3] = cpu.gpr[4] = 9;
		PPCInterpreterSlim_executeInstruction(&cpu);
		CHECK(cpu.instructionPointer == kCode + 4);
	}
	// and an instruction beside it still runs: addi r3, r0, 0x1234
	put(kCode, (14u << 26) | (3u << 21) | 0x1234);
	cpu.instructionPointer = kCode;
	PPCInterpreterSlim_executeInstruction(&cpu);
	CHECK(cpu.gpr[3] == 0x1234 && cpu.instructionPointer == kCode + 4);
	if (failures)
	{
		std::fprintf(stderr, "ppc_trap_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("ppc_trap_test: all passed\n");
	return 0;
}
