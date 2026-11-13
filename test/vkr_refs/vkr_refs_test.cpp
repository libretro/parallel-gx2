// The references Vulkan objects hold on one another (VKRBase.h's
// VKRMoveableRefCounter over VKRRefList): any number added - in the object,
// in a block from the pool, on the heap - each target's count raised once
// and dropped once when the holder goes, a target told when its count
// reaches zero, the list moved whole with its object; and the objects and
// their blocks made and dropped from several threads at once (run under
// ThreadSanitizer and AddressSanitizer by CI).
#include <vulkan/vulkan.h>
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

struct Target : VKRMoveableRefCounter
{
	int zeroes = 0;
	int Count() const { return m_refCount.load(); }
	void RefCountReachedZero() override { zeroes++; }
};

struct Holder : VKRMoveableRefCounter
{
};

static void TestCounts()
{
	// none, a few (in the object), past them (a block), past the block (heap)
	for (int n : {0, 1, 4, 5, 63, 64, 65, 200})
	{
		std::vector<Target> targets(n);
		{
			Holder* h = new Holder();
			for (auto& t : targets)
				h->addRef(&t);
			bool counted = true;
			for (auto& t : targets)
				counted = counted && t.Count() == 1 && t.zeroes == 0;
			CHECK(counted);
			delete h;
		}
		bool dropped = true;
		for (auto& t : targets)
			dropped = dropped && t.Count() == 0 && t.zeroes == 1;
		CHECK(dropped);
	}
	// reserved first, then added: the same
	{
		std::vector<Target> targets(40);
		Holder* h = new Holder();
		h->reserveRefs(40);
		for (auto& t : targets)
			h->addRef(&t);
		h->addRef(&targets[0]);
		CHECK(targets[0].Count() == 2);
		delete h;
		CHECK(targets[0].Count() == 0 && targets[0].zeroes == 1 && targets[39].zeroes == 1);
	}
}

static void TestMove()
{
	for (int n : {3, 30, 100})
	{
		std::vector<Target> targets(n);
		Holder a;
		for (auto& t : targets)
			a.addRef(&t);
		{
			Holder b(std::move(a));
			bool kept = true;
			for (auto& t : targets)
				kept = kept && t.Count() == 1;
			CHECK(kept);
		}
		// b dropped them; a, moved from, holds none
		bool dropped = true;
		for (auto& t : targets)
			dropped = dropped && t.Count() == 0 && t.zeroes == 1;
		CHECK(dropped);
	}
}

static void TestThreads()
{
	std::atomic<int> bad{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++)
		threads.emplace_back([&, t] {
			std::vector<Target> targets(80);
			for (int r = 0; r < 3000; r++)
			{
				const int n = (r * 7 + t) % 80;
				Holder* h = new Holder();
				for (int i = 0; i < n; i++)
					h->addRef(&targets[i]);
				delete h;
			}
			for (auto& tg : targets)
				if (tg.Count() != 0)
					bad++;
		});
	for (auto& th : threads)
		th.join();
	CHECK(bad.load() == 0);
}

int main()
{
	TestCounts();
	TestMove();
	TestThreads();
	if (s_failures)
	{
		std::fprintf(stderr, "vkr_refs_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("vkr_refs_test: all passed\n");
	return 0;
}
