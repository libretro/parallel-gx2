// The HLE call table (src/Cafe/HW/Espresso/Interpreter/PPCInterpreterHLE.cpp),
// which RPLLoader_MakePPCCallable hands out a stub per index from: guest
// threads registering the same calls at once each get an index whose entry
// is that call, a call registered again gets its index back, and every index
// handed out fits a stub (below PPC_HLE_TABLE_CAPACITY).
#include "Cafe/HW/Espresso/PPCState.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

// distinct functions: a template instance each
template<int N>
static void Call(PPCInterpreter_t*) {}

template<int... Ns>
static constexpr std::array<HLECALL, sizeof...(Ns)> MakeCalls(std::integer_sequence<int, Ns...>)
{
	return {&Call<Ns>...};
}
static constexpr auto kCalls = MakeCalls(std::make_integer_sequence<int, 256>{});

int main()
{
	// one thread: the same index again
	const HLEIDX first = PPCInterpreter_registerHLECall(kCalls[0]);
	CHECK(first >= 0 && first < (HLEIDX)PPC_HLE_TABLE_CAPACITY);
	CHECK(PPCInterpreter_registerHLECall(kCalls[0]) == first);
	CHECK(PPCInterpreter_getHLECall(first) == kCalls[0]);

	// several threads registering all of them at once, in different orders
	constexpr int THREADS = 4;
	std::atomic<bool> go{false};
	std::atomic<int> bad{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < THREADS; t++)
		threads.emplace_back([&, t] {
			while (!go.load())
				;
			for (size_t k = 0; k < kCalls.size(); k++)
			{
				const size_t i = (k * (2 * t + 1)) % kCalls.size();
				const HLEIDX idx = PPCInterpreter_registerHLECall(kCalls[i]);
				if (idx < 0 || idx >= (HLEIDX)PPC_HLE_TABLE_CAPACITY || PPCInterpreter_getHLECall(idx) != kCalls[i])
					bad++;
			}
		});
	go = true;
	for (auto& th : threads)
		th.join();
	CHECK(bad.load() == 0);
	// once all are in, each gets one stable index back
	for (size_t i = 0; i < kCalls.size(); i++)
	{
		const HLEIDX idx = PPCInterpreter_registerHLECall(kCalls[i]);
		CHECK(PPCInterpreter_getHLECall(idx) == kCalls[i]);
		CHECK(PPCInterpreter_registerHLECall(kCalls[i]) == idx);
	}
	CHECK(PPCInterpreter_getHLECall(-1) == nullptr);
	CHECK(PPCInterpreter_getHLECall((HLEIDX)PPC_HLE_TABLE_CAPACITY) == nullptr);
	if (failures)
	{
		std::fprintf(stderr, "hle_table: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("hle_table: ok\n");
	return 0;
}
