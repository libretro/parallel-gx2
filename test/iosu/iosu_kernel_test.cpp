// The IOSU kernel (Cafe/IOSU/kernel/iosu_kernel.cpp) without locks, as the
// services and the three cores use it at once:
//  - message queues: order, full and empty, any number of senders and
//    receivers blocking on a queue smaller than they are, a destroy waking
//    a blocked receiver, and a destroyed queue's handle staying dead;
//  - timers: repeating, one-shot, stopped, destroyed, and created and
//    destroyed from several threads;
//  - IPC: a service's device opened, used and closed by three cores at
//    once, one request at a time and many in flight, every reply to the
//    core and the request it belongs to.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "util/helpers/EventWake.h"

using namespace iosu::kernel;

void TestInitGuestMemory();
MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment);

static std::atomic<int> s_failures{0};
#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			s_failures++; \
		} \
	} while (0)

static void TestMessageQueues()
{
	{
		// three slots: in order, then full, then empty
		uint32be buf[3];
		IOSMsgQueueId q = IOS_CreateMessageQueue((IOSMessage*)buf, 3);
		CHECK((sint32)q > 0);
		for (uint32 lap = 0; lap < 5; lap++)
		{
			for (uint32 i = 0; i < 3; i++)
				CHECK(IOS_SendMessage(q, lap * 10 + i, 1) == IOS_ERROR_OK);
			CHECK(IOS_SendMessage(q, 99, 1) == IOS_ERROR_WOULD_BLOCK);
			for (uint32 i = 0; i < 3; i++)
			{
				IOSMessage m = 0;
				CHECK(IOS_ReceiveMessage(q, &m, 1) == IOS_ERROR_OK && m == lap * 10 + i);
			}
			IOSMessage m;
			CHECK(IOS_ReceiveMessage(q, &m, 1) == IOS_ERROR_NONE_AVAILABLE);
		}
		CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_OK);
		// dead from now on, and the slot's next queue has another handle
		CHECK(IOS_SendMessage(q, 1, 1) == IOS_ERROR_INVALID);
		CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_INVALID);
		IOSMsgQueueId q2 = IOS_CreateMessageQueue((IOSMessage*)buf, 3);
		CHECK(q2 != q && (q2 & 0xFFF) == (q & 0xFFF));
		CHECK(IOS_SendMessage(q, 1, 1) == IOS_ERROR_INVALID);
		CHECK(IOS_DestroyMessageQueue(q2) == IOS_ERROR_OK);
	}
	{
		// four senders and three receivers through five slots, all blocking
		constexpr uint32 SENDERS = 4, RECEIVERS = 3, PER_SENDER = 20000;
		uint32be buf[5];
		IOSMsgQueueId q = IOS_CreateMessageQueue((IOSMessage*)buf, 5);
		std::vector<std::atomic<uint8>> seen(SENDERS * PER_SENDER);
		std::vector<std::thread> threads;
		for (uint32 r = 0; r < RECEIVERS; r++)
			threads.emplace_back([&] {
				for (;;)
				{
					IOSMessage m;
					if (IOS_ReceiveMessage(q, &m, 0) != IOS_ERROR_OK)
					{
						s_failures++;
						return;
					}
					if (m == 0xFFFFFFFF)
						return;
					seen[m].fetch_add(1);
				}
			});
		for (uint32 s = 0; s < SENDERS; s++)
			threads.emplace_back([&, s] {
				for (uint32 i = 0; i < PER_SENDER; i++)
					if (IOS_SendMessage(q, s * PER_SENDER + i, 0) != IOS_ERROR_OK)
						s_failures++;
			});
		for (uint32 s = 0; s < SENDERS; s++)
			threads[RECEIVERS + s].join();
		for (uint32 r = 0; r < RECEIVERS; r++)
			CHECK(IOS_SendMessage(q, 0xFFFFFFFF, 0) == IOS_ERROR_OK);
		for (uint32 r = 0; r < RECEIVERS; r++)
			threads[r].join();
		bool once = true;
		for (auto& c : seen)
			once &= c.load() == 1;
		CHECK(once);
		CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_OK);
	}
	{
		// a destroy wakes the receiver blocked on the queue, and waits for it
		uint32be buf[2];
		IOSMsgQueueId q = IOS_CreateMessageQueue((IOSMessage*)buf, 2);
		std::atomic<int> got{1};
		std::thread receiver([&] {
			IOSMessage m;
			got = IOS_ReceiveMessage(q, &m, 0);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_OK);
		receiver.join();
		CHECK(got.load() == IOS_ERROR_INVALID);
	}
	{
		// a sender blocked on a full queue goes on once a message is taken
		uint32be buf[1];
		IOSMsgQueueId q = IOS_CreateMessageQueue((IOSMessage*)buf, 1);
		CHECK(IOS_SendMessage(q, 1, 0) == IOS_ERROR_OK);
		std::atomic<bool> sent{false};
		std::thread sender([&] {
			sent = IOS_SendMessage(q, 2, 0) == IOS_ERROR_OK;
		});
		IOSMessage m;
		CHECK(IOS_ReceiveMessage(q, &m, 0) == IOS_ERROR_OK && m == 1);
		CHECK(IOS_ReceiveMessage(q, &m, 0) == IOS_ERROR_OK && m == 2);
		sender.join();
		CHECK(sent.load());
		CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_OK);
	}
}

static void TestTimers()
{
	using namespace std::chrono;
	uint32be buf[64];
	IOSMsgQueueId q = IOS_CreateMessageQueue((IOSMessage*)buf, 64);
	// repeating every 2 ms
	IOS_ERROR t = IOS_CreateTimer(2000, 2000, q, 42);
	CHECK((sint32)t >= 0);
	auto start = steady_clock::now();
	for (int i = 0; i < 5; i++)
	{
		IOSMessage m = 0;
		CHECK(IOS_ReceiveMessage(q, &m, 0) == IOS_ERROR_OK && m == 42);
	}
	CHECK(steady_clock::now() - start >= milliseconds(8));
	// stopped: nothing after what was already on its way
	CHECK(IOS_StopTimer(t) == IOS_ERROR_OK);
	std::this_thread::sleep_for(milliseconds(20));
	IOSMessage m;
	while (IOS_ReceiveMessage(q, &m, 1) == IOS_ERROR_OK)
		;
	std::this_thread::sleep_for(milliseconds(30));
	CHECK(IOS_ReceiveMessage(q, &m, 1) == IOS_ERROR_NONE_AVAILABLE);
	CHECK(IOS_DestroyTimer(t) == IOS_ERROR_OK);
	CHECK(IOS_DestroyTimer(t) == IOS_ERROR_INVALID);
	CHECK(IOS_StopTimer(t) == IOS_ERROR_INVALID);
	CHECK(IOS_StopTimer(100000) == IOS_ERROR_INVALID);
	// one shot: once
	IOS_ERROR once = IOS_CreateTimer(1000, 0, q, 7);
	CHECK(IOS_ReceiveMessage(q, &m, 0) == IOS_ERROR_OK && m == 7);
	std::this_thread::sleep_for(milliseconds(20));
	CHECK(IOS_ReceiveMessage(q, &m, 1) == IOS_ERROR_NONE_AVAILABLE);
	CHECK(IOS_DestroyTimer(once) == IOS_ERROR_OK);
	// created and destroyed from four threads at once, each firing once
	std::atomic<int> created{0};
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back([&] {
			for (int j = 0; j < 100; j++)
			{
				IOS_ERROR id = IOS_CreateTimer(100, 0, q, 1000);
				if ((sint32)id < 0)
					continue;
				created++;
				std::this_thread::sleep_for(microseconds(300));
				if (IOS_DestroyTimer(id) != IOS_ERROR_OK)
					s_failures++;
			}
		});
	for (auto& th : threads)
		th.join();
	CHECK(created.load() == 400);
	std::this_thread::sleep_for(milliseconds(10));
	bool onlyTheirs = true;
	while (IOS_ReceiveMessage(q, &m, 1) == IOS_ERROR_OK)
		onlyTheirs &= m == 1000;
	CHECK(onlyTheirs);
	CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_OK);
}

// A request as a core puts it in: the body the kernel answers through
struct Request
{
	IPCCommandBody body;
	std::atomic<uint32> answeredBy{0xFFFFFFFF};
};

static EventWake s_answered;

void TestOnResponse(uint32 ppcCoreIndex, IPCCommandBody* body)
{
	Request* r = (Request*)body; // the body is the request's first member
	r->answeredBy.store(ppcCoreIndex, std::memory_order_release);
	s_answered.Notify();
}

static uint32 Wait(Request& r)
{
	s_answered.WaitUntil([&] { return r.answeredBy.load(std::memory_order_acquire) != 0xFFFFFFFF; });
	return r.body.result;
}

static void Submit(uint32 core, Request& r)
{
	r.answeredBy.store(0xFFFFFFFF);
	IPCSubmitFromCOS(core, &r.body);
}

static MPTR s_devPath;

static void Open(uint32 core, Request& r, const char* path)
{
	std::memset(&r.body, 0, sizeof(r.body));
	MPTR guestPath = coreinit_allocFromSysArea(32, 4);
	std::strcpy((char*)memory_base + guestPath, path);
	r.body.cmdId = IPCCommandId::IOS_OPEN;
	r.body.args[0] = guestPath;
	r.body.args[1] = (uint32)std::strlen(path) + 1;
	r.body.args[2] = 0;
	Submit(core, r);
}

static void Ioctl(uint32 core, Request& r, IOSDevHandle h, uint32 requestId)
{
	std::memset(&r.body, 0, sizeof(r.body));
	r.body.cmdId = IPCCommandId::IOS_IOCTL;
	r.body.devHandle = h;
	r.body.args[0] = requestId;
	Submit(core, r);
}

static void Close(uint32 core, Request& r, IOSDevHandle h)
{
	std::memset(&r.body, 0, sizeof(r.body));
	r.body.cmdId = IPCCommandId::IOS_CLOSE;
	r.body.devHandle = h;
	Submit(core, r);
}

static void TestIPC()
{
	// the service: answers an open with its own handle, an ioctl with the
	// request number plus one, a close with 0
	uint32be buf[128];
	IOSMsgQueueId q = IOS_CreateMessageQueue((IOSMessage*)buf, 128);
	CHECK(IOS_RegisterResourceManager("/dev/test", q) == IOS_ERROR_OK);
	CHECK(IOS_RegisterResourceManager("/dev/test", q) == IOS_ERROR_INVALID);
	std::atomic<int> opens{0}, closes{0};
	std::thread service([&] {
		for (;;)
		{
			IOSMessage m;
			if (IOS_ReceiveMessage(q, &m, 0) != IOS_ERROR_OK || m == 0)
				return;
			IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(m).GetPtr();
			switch ((IPCCommandId)cmd->cmdId)
			{
			case IPCCommandId::IOS_OPEN:
				opens++;
				IOS_ResourceReply(cmd, (IOS_ERROR)0x55);
				break;
			case IPCCommandId::IOS_IOCTL:
				if ((uint32)cmd->devHandle != 0x55)
					s_failures++;
				IOS_ResourceReply(cmd, (IOS_ERROR)((uint32)cmd->args[0] + 1));
				break;
			case IPCCommandId::IOS_CLOSE:
				closes++;
				IOS_ResourceReply(cmd, IOS_ERROR_OK);
				break;
			default:
				s_failures++;
				IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
			}
		}
	});

	std::vector<std::thread> cores;
	std::atomic<int> answeredElsewhere{0};
	for (uint32 core = 0; core < 3; core++)
		cores.emplace_back([&, core] {
			// a handle, a request at a time on it, then 25 in flight on 25
			// handles at once, then closed
			Request r;
			Open(core, r, "/dev/test");
			IOSDevHandle h = Wait(r);
			if (r.answeredBy != core || (sint32)h < 0)
				answeredElsewhere++;
			for (uint32 i = 0; i < 2000; i++)
			{
				Ioctl(core, r, h, core * 100000 + i);
				if (Wait(r) != core * 100000 + i + 1 || r.answeredBy != core)
					answeredElsewhere++;
			}
			Close(core, r, h);
			if (Wait(r) != 0)
				answeredElsewhere++;
			std::vector<Request> many(25);
			std::vector<IOSDevHandle> handles(25);
			for (auto& m : many)
				Open(core, m, "/dev/test");
			for (size_t i = 0; i < many.size(); i++)
				handles[i] = Wait(many[i]);
			for (int round = 0; round < 40; round++)
			{
				for (size_t i = 0; i < many.size(); i++)
					Ioctl(core, many[i], handles[i], (uint32)(round * 1000 + i));
				for (size_t i = 0; i < many.size(); i++)
					if (Wait(many[i]) != (uint32)(round * 1000 + i + 1) || many[i].answeredBy != core)
						answeredElsewhere++;
			}
			for (size_t i = 0; i < many.size(); i++)
				Close(core, many[i], handles[i]);
			for (size_t i = 0; i < many.size(); i++)
				Wait(many[i]);
			// every handle different from the others this core had
			std::set<IOSDevHandle> distinct(handles.begin(), handles.end());
			if (distinct.size() != handles.size())
				answeredElsewhere++;
		});
	for (auto& c : cores)
		c.join();
	CHECK(answeredElsewhere.load() == 0);
	CHECK(opens.load() == 3 * 26 && closes.load() == 3 * 26);

	// a closed handle is refused, without reaching the service
	{
		Request r;
		Open(0, r, "/dev/test");
		IOSDevHandle h = Wait(r);
		Close(0, r, h);
		Wait(r);
		Ioctl(0, r, h, 1);
		CHECK((sint32)Wait(r) == IOS_ERROR_INVALID);
	}
	// unregistered: an open is refused
	CHECK(IOS_UnregisterResourceManager("/dev/test") == IOS_ERROR_OK);
	CHECK(IOS_UnregisterResourceManager("/dev/test") == IOS_ERROR_INVALID);
	{
		Request r;
		Open(1, r, "/dev/test");
		CHECK((sint32)Wait(r) == IOS_ERROR_INVALID);
	}
	CHECK(IOS_SendMessage(q, 0, 0) == IOS_ERROR_OK);
	service.join();
	CHECK(IOS_DestroyMessageQueue(q) == IOS_ERROR_OK);
}

int main()
{
	TestInitGuestMemory();
	SysAllocatorContainer::GetInstance().Initialize();
	// twice, as a core that is unloaded and loaded again
	for (int launch = 0; launch < 2; launch++)
	{
		GetModule()->SystemLaunch();
		TestMessageQueues();
		TestTimers();
		TestIPC();
		GetModule()->SystemExit();
	}
	if (s_failures.load())
	{
		std::printf("%d check(s) failed\n", s_failures.load());
		return 1;
	}
	std::printf("ok: IOSU kernel queues, timers and IPC\n");
	return 0;
}
