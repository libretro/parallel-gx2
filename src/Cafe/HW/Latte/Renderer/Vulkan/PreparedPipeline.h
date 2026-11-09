#pragma once

#include <atomic>
#include <vulkan/vulkan.h>

#include "util/helpers/ClaimTicket.h"
#include "util/helpers/EventWake.h"

// A pipeline made ahead of the draw or copy that needs it, on a compile
// thread (AGENTS.md: never compile a pipeline on the draw path). It is queued
// once, as a ticket, and whoever takes the ticket's item first builds it: a
// compile thread, or the GPU thread when it needs the pipeline before any
// compile thread got to it. The GPU thread waits only for a build already
// under way, never for the queue. No lock.
class PreparedPipeline
{
public:
	// onCompileThread: built by a compile thread, not by the thread taking it
	using BuildFn = VkPipeline (*)(void* context, bool onCompileThread);
	using Ticket = ClaimTicket<PreparedPipeline>;

	PreparedPipeline(BuildFn build, void* context) : m_build(build), m_context(context) {}
	~PreparedPipeline() { Retire(); }
	PreparedPipeline(const PreparedPipeline&) = delete;
	PreparedPipeline& operator=(const PreparedPipeline&) = delete;

	// The ticket for the compile threads' queue; once
	Ticket* Queue()
	{
		m_ticket = new Ticket(this);
		return m_ticket;
	}

	// A compile thread's: builds it unless it was taken
	static void Run(Ticket* ticket)
	{
		if (PreparedPipeline* p = ticket->Claim())
			p->BuildHere(true);
		ticket->Release();
	}
	// A ticket destroyed with the queue, unrun
	static void Drop(Ticket* ticket) { ticket->Release(); }

	// The pipeline, built: by a compile thread, here when none had started
	// on it (builtHere), or waited for while one builds it
	VkPipeline Take(bool* builtHere = nullptr)
	{
		if (!IsBuilt())
		{
			if (!m_ticket || m_ticket->Claim())
			{
				BuildHere(false);
				if (builtHere)
					*builtHere = true;
			}
			else
				Wake().WaitUntil([this] { return IsBuilt(); });
		}
		return m_pipeline;
	}

	bool IsBuilt() const { return m_built.load(std::memory_order_acquire); }

	// From here on no compile thread builds it, and none is: for the owner,
	// before what the build reads goes. A pipeline a compile thread built is
	// still there to take.
	void Retire()
	{
		if (!m_ticket)
			return;
		if (!m_ticket->Claim())
			Wake().WaitUntil([this] { return IsBuilt(); });
		m_ticket->Release();
		m_ticket = nullptr;
	}

private:
	void BuildHere(bool onCompileThread)
	{
		m_pipeline = m_build(m_context, onCompileThread);
		m_built.store(true, std::memory_order_release);
		// not this object's: the GPU thread may destroy it as soon as it sees
		// it built, while this thread is still in the notify
		Wake().Notify();
	}
	static EventWake& Wake()
	{
		static ImmortalEventWake s_wake;
		return s_wake.Get();
	}

	BuildFn m_build;
	void* m_context;
	Ticket* m_ticket{};
	VkPipeline m_pipeline{VK_NULL_HANDLE};
	std::atomic<bool> m_built{false};
};
