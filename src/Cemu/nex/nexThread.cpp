#include "prudp.h"
#include "nex.h"
#include "nexThread.h"

#include <atomic>
#include "util/helpers/FixedQueue.h"
#include "util/helpers/EventWake.h"

#include "nexWait.h"

// Services handed to the nex thread, from whichever thread registers them: no
// lock, the nex thread takes them all at the top of each pass
static FixedList<nexService*, 64> s_queuedServices;
static std::atomic<uint32> s_queuedServiceCount{0};
static ImmortalEventWake s_servicesWake; // the nex thread with no services sleeps on it

std::vector<nexService*> list_activeNexServices; // the nex thread's alone

static nexWait::Wake& GetNexWake()
{
	static nexWait::Wake s_wake; // made once, by whichever thread gets here
	return s_wake;
}

void nexThread_wake()
{
	GetNexWake().Signal();
}

bool nexWaitForSockets(const SOCKET* sockets, size_t count, uint32 timeoutMs, bool withWake)
{
	return nexWait::WaitForSockets(sockets, count, timeoutMs, withWake ? &GetNexWake() : nullptr);
}

// How long the nex thread sleeps with nothing arriving: the protocol's own
// clocks - retries after 1.2 and 2.3 s, pings every 1.5 s, request timeouts
// after 10 s - only need looking at this often. Anything that arrives, or a
// wake, ends the wait at once.
static constexpr uint32 kNexIdleWaitMs = 100;

static thread_local bool t_isNexThread = false;

void nexThread_run()
{
	t_isNexThread = true;
	std::vector<SOCKET> sockets;
	while (true)
	{
		// check for new services
		s_queuedServices.TakeAll([](nexService*&& service) {
			list_activeNexServices.push_back(service);
			s_queuedServiceCount.fetch_sub(1, std::memory_order_relaxed);
		});
		// if service list is empty, asleep until one is registered (it
		// wakes this) rather than checking five times a second
		if (list_activeNexServices.empty())
		{
			s_servicesWake->WaitUntil([] { return s_queuedServiceCount.load(std::memory_order_acquire) != 0; });
			continue;
		}
		// update
		for (auto& it : list_activeNexServices)
		{
			it->update();
		}
		// delete services marked for destruction
		sint32 idx = 0;
		sint32 listSize = (sint32)list_activeNexServices.size();
		while (idx < listSize)
		{
			if (list_activeNexServices[idx]->isMarkedForDestruction())
			{
				list_activeNexServices[idx]->destroy();
				listSize--;
				list_activeNexServices[idx] = list_activeNexServices[listSize];
			}
			idx++;
		}
		if (listSize != list_activeNexServices.size())
		{
			list_activeNexServices.resize(listSize);
		}
		// Asleep until a packet arrives on any of them, a request is queued
		// or a service registered (both wake this), or the protocol's clocks
		// need looking at - instead of upstream's millisecond poll
		sockets.clear();
		for (auto& it : list_activeNexServices)
		{
			if (prudpClient* con = it->getPRUDPConnection())
				sockets.push_back(con->GetSocket());
		}
		nexWaitForSockets(sockets.data(), sockets.size(), kNexIdleWaitMs, true);
	}
}

static std::atomic_bool s_nexThreadLaunched{false};

void nexThread_init()
{
	if (s_nexThreadLaunched.exchange(true))
		return;
	std::thread t(nexThread_run);
	t.detach();
}

void nexThread_registerService(nexService* service)
{
	nexThread_init();
	s_queuedServiceCount.fetch_add(1, std::memory_order_release);
	s_queuedServices.Push(service);
	s_servicesWake->Notify();
	nexThread_wake();
}

bool nexThread_isCurrentThread()
{
	return t_isNexThread;
}
