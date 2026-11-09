// Pipelines made ahead of the draw or copy that needs them
// (Cafe/HW/Latte/Renderer/Vulkan/PreparedPipeline.h): queued as tickets for
// compile threads, taken by the GPU thread. The surface copies' pipelines go
// this way, queued when a texture that can be drawn to is created. Builds are
// counted, not compiled:
//  - made ahead: once the compile threads are through, every take finds its
//    pipeline built and builds nothing;
//  - takes racing the compile threads: each built exactly once, by one side,
//    and every take gets that one;
//  - retired before a compile thread got to it: never built, and the ticket
//    still in the queue finds nothing; retired while one builds it: waits for
//    it, and what it built is there to take;
//  - tickets dropped with the queue, unrun: nothing leaks (LeakSanitizer).
#include "Cafe/HW/Latte/Renderer/Vulkan/PreparedPipeline.h"
#include "util/helpers/MpmcQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

struct Entry
{
	std::atomic<int> builds{0};
	std::atomic<bool> hold{false}; // a build waits while set
	std::atomic<bool> started{false};
	uint64_t id = 0;
	// last: destroyed first, before what its build reads
	PreparedPipeline prepared{[](void* self) {
		Entry* e = (Entry*)self;
		e->started = true;
		while (e->hold.load())
			std::this_thread::yield();
		e->builds++;
		return (VkPipeline)(uintptr_t)(e->id + 1);
	}, this};
};

// the compile thread pool's shape (VulkanPipelineCompiler.cpp): nullptr stops a thread
struct Pool
{
	MpmcQueue<PreparedPipeline::Ticket*> queue;
	std::vector<std::thread> threads;
	void Start(int n)
	{
		for (int i = 0; i < n; i++)
			threads.emplace_back([this] {
				for (;;)
				{
					PreparedPipeline::Ticket* t = queue.pop();
					if (!t)
						return;
					PreparedPipeline::Run(t);
				}
			});
	}
	void Stop()
	{
		for (size_t i = 0; i < threads.size(); i++)
			queue.push(nullptr);
		for (auto& t : threads)
			t.join();
		threads.clear();
		PreparedPipeline::Ticket* t;
		while (queue.peek2(t))
			if (t)
				PreparedPipeline::Drop(t);
	}
};

static void MadeAhead()
{
	std::vector<std::unique_ptr<Entry>> entries;
	Pool pool;
	for (int i = 0; i < 64; i++)
	{
		entries.emplace_back(new Entry());
		entries.back()->id = i;
		pool.queue.push(entries.back()->prepared.Queue());
	}
	pool.Start(4);
	pool.Stop(); // every ticket run
	int builtHere = 0;
	for (auto& e : entries)
	{
		bool here = false;
		const VkPipeline p = e->prepared.Take(&here);
		builtHere += here;
		CHECK(p == (VkPipeline)(uintptr_t)(e->id + 1));
		CHECK(e->builds == 1);
	}
	CHECK(builtHere == 0);
}

static void Racing()
{
	Pool pool;
	pool.Start(4);
	int builtHere = 0;
	for (int round = 0; round < 50; round++)
	{
		std::vector<std::unique_ptr<Entry>> entries;
		for (int i = 0; i < 40; i++)
		{
			entries.emplace_back(new Entry());
			entries.back()->id = round * 100 + i;
			pool.queue.push(entries.back()->prepared.Queue());
		}
		// from the back: some still queued, some being built, some built
		for (int i = 39; i >= 0; i--)
		{
			bool here = false;
			const VkPipeline p = entries[i]->prepared.Take(&here);
			builtHere += here;
			CHECK(p == (VkPipeline)(uintptr_t)(entries[i]->id + 1));
			CHECK(entries[i]->prepared.Take() == p);
		}
		for (auto& e : entries)
			CHECK(e->builds == 1);
	}
	pool.Stop();
	std::printf("racing: %d of 2000 taken before a compile thread built them\n", builtHere);
}

static void Retired()
{
	// before a compile thread got to it
	{
		Pool pool;
		Entry* e = new Entry();
		pool.queue.push(e->prepared.Queue());
		delete e;
		pool.Start(2); // the ticket finds nothing
		pool.Stop();
	}
	// while one builds it
	{
		Pool pool;
		auto e = std::make_unique<Entry>();
		e->hold = true;
		pool.queue.push(e->prepared.Queue());
		pool.Start(1);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!e->started && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		CHECK(e->started); // a compile thread builds it
		if (!e->started)
		{
			e->prepared.Retire();
			pool.Stop();
			return;
		}
		std::thread release([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			e->hold = false;
		});
		e->prepared.Retire(); // waits for the build
		CHECK(e->prepared.IsBuilt());
		CHECK(e->builds == 1);
		CHECK(e->prepared.Take() == (VkPipeline)(uintptr_t)1);
		release.join();
		pool.Stop();
	}
}

static void Dropped()
{
	Pool pool; // never started: every ticket dropped with the queue
	std::vector<std::unique_ptr<Entry>> entries;
	for (int i = 0; i < 8; i++)
	{
		entries.emplace_back(new Entry());
		pool.queue.push(entries.back()->prepared.Queue());
	}
	pool.Stop();
	for (auto& e : entries)
		CHECK(e->builds == 0);
	entries.clear();
}

int main()
{
	MadeAhead();
	Racing();
	Retired();
	Dropped();
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("prepared_pipeline: all passed\n");
	return 0;
}
