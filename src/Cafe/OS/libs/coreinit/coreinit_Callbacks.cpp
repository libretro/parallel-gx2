#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/async_cb.h"

SysAllocator<OSThread_t> g_coreinitCallbackThread;
SysAllocator<uint8, 1024*64> _g_coreinitCallbackThreadStack;
SysAllocator<coreinit::OSSemaphore> g_asyncCallbackAsync;
SysAllocator<char, 32> _g_coreinitCBThreadName;

void _coreinitCallbackThread(PPCInterpreter_t* hCPU)
{
	while (coreinit::OSWaitSemaphore(g_asyncCallbackAsync.GetPtr()))
	{
		// the semaphore was signalled after the push it counts
		async_cb_t cb;
		if (!async_cb_pop(&cb))
		{
			cemuLog_log(LogType::Force, "AsyncCallbackQueue is empty. Unexpected behavior");
			continue;
		}
		PPCCoreCallback(cb.func, cb.params[0], cb.params[1], cb.params[2], cb.params[3], cb.params[4], cb.params[5], cb.params[6], cb.params[7]);
	}
}

void coreinitAsyncCallback_addWithLock(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
{
	cemu_assert_debug(numParameters <= 8);
	if (functionMPTR >= 0x10000000)
	{
		cemuLog_log(LogType::Force, fmt::format("Suspicious callback address {0:08x} params: {1:08x} {2:08x} {3:08x} {4:08x}", functionMPTR, r3, r4, r5, r6));
		cemuLog_waitForFlush();
	}
	const async_cb_t cb = { functionMPTR, numParameters, { r3, r4, r5, r6, r7, r8, r9, r10 } };
	if (!async_cb_push(&cb))
	{
		cemuLog_log(LogType::Force, "Async callback queue full: callback {0:08x} dropped", functionMPTR);
		return;
	}
	__OSLockScheduler();
	coreinit::OSSignalSemaphoreInternal(g_asyncCallbackAsync.GetPtr(), false);
	__OSUnlockScheduler();
}

void coreinitAsyncCallback_add(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
{
	cemu_assert_debug(__OSHasSchedulerLock() == false); // do not call when holding scheduler lock
	coreinitAsyncCallback_addWithLock(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10);
}

void InitializeAsyncCallback()
{
	// what the last title queued is not run in this one
	async_cb_reset();
	coreinit::OSInitSemaphore(g_asyncCallbackAsync.GetPtr(), 0);

	coreinit::OSCreateThreadType(g_coreinitCallbackThread.GetPtr(), PPCInterpreter_makeCallableExportDepr(_coreinitCallbackThread), 0, nullptr, _g_coreinitCallbackThreadStack.GetPtr() + _g_coreinitCallbackThreadStack.GetByteSize(), (sint32)_g_coreinitCallbackThreadStack.GetByteSize(), 0, 7, OSThread_t::THREAD_TYPE::TYPE_IO);
	coreinit::OSResumeThread(g_coreinitCallbackThread.GetPtr());

	strcpy(_g_coreinitCBThreadName.GetPtr(), "Callback Thread");
	coreinit::OSSetThreadName(g_coreinitCallbackThread.GetPtr(), _g_coreinitCBThreadName.GetPtr());
}