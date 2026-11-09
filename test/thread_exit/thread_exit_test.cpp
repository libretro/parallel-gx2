// Ending a thread from deep inside what it runs (util/helpers/ThreadExit.h),
// the way the GPU thread ends on a close (LatteThread_Exit). Linked as the
// core links: the C++ runtime statically (Makefile). There, the thread's end
// by pthread_exit was abort() in the runtime's thread entry, every time a
// title was closed:
//  - the thread ends, its function returning, from three calls down;
//  - every destructor on the way runs, innermost first, and nothing after
//    the call does;
//  - a thread that returns without it is unaffected;
//  - sixteen threads at once, each ending its own way, under TSan in CI.
#include "util/helpers/ThreadExit.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

struct Trail
{
	std::atomic<int>* order;
	int depth;
	int* seen;
	~Trail() { seen[depth] = order->fetch_add(1) + 1; }
};

static void Deep(int depth, bool leave, std::atomic<int>& order, int* seen, std::atomic<int>& after)
{
	Trail trail{&order, depth, seen};
	if (depth == 2)
	{
		if (leave)
			ThreadExit_Unwind();
		return;
	}
	Deep(depth + 1, leave, order, seen, after);
	after++; // only on the way back of a return
}

int main()
{
	// from three calls down
	{
		std::atomic<int> order{0}, after{0};
		int seen[3] = {};
		bool returned = false;
		std::thread t([&] {
			ThreadExit_Run([&] { Deep(0, true, order, seen, after); });
			returned = true; // the thread goes on from ThreadExit_Run
		});
		t.join();
		CHECK(returned);
		CHECK(order == 3);
		// innermost first
		CHECK(seen[2] == 1 && seen[1] == 2 && seen[0] == 3);
		CHECK(after == 0);
	}
	// a thread that returns
	{
		std::atomic<int> order{0}, after{0};
		int seen[3] = {};
		std::thread t([&] { ThreadExit_Run([&] { Deep(0, false, order, seen, after); }); });
		t.join();
		CHECK(order == 3 && after == 2);
	}
	// sixteen at once, half of them leaving
	{
		std::vector<std::thread> threads;
		std::atomic<int> ended{0};
		std::vector<int> seen(16 * 3);
		std::vector<std::atomic<int>> orders(16);
		std::atomic<int> after{0};
		for (int i = 0; i < 16; i++)
			threads.emplace_back([&, i] {
				for (int round = 0; round < 100; round++)
					ThreadExit_Run([&] { Deep(0, (i & 1) != 0, orders[i], &seen[i * 3], after); });
				ended++;
			});
		for (auto& t : threads)
			t.join();
		CHECK(ended == 16);
		for (int i = 0; i < 16; i++)
			CHECK(orders[i] == 300);
		CHECK(after == 8 * 100 * 2);
	}
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("thread_exit: all passed\n");
	return 0;
}
