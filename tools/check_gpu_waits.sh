#!/bin/sh
# The GPU thread, with a thread of its own, sleeps when it has nothing to do
# rather than spinning: LatteCP_readU32Deprc, with the command ring empty,
# waits on LatteGpuWake until the CPU submits (TCLSubmitToRing notifies),
# queues an async command, flips an OSScreen, the pause gate is asked for or
# the next timed vsync is due. Upstream spins there, a host core kept busy for
# as long as the emulated CPU takes to send the next command.
#
# The frontend's thread, waiting for the GPU thread to reach the pause gate,
# park there or finish handing the context back (context_destroy), sleeps
# until the thread says so rather than a millisecond at a time; and
# RetroArch's audio thread, pulling from an empty ring (Audio Thread), sleeps
# until AX feeds it, not a fixed millisecond.
#
# And the waits around the title list and the IOSU: the title scan the load
# waits for (100 ms polls), the ioctl workers' shutdown (1 ms), the boss
# daemon with nothing to run (100 ms), its stop (1 ms) and the task waits
# games make (100 ms each), and nim's waits for the accounts and its package
# list (500 and 200 ms) - all asleep on an EventWake until notified.
#
# And the log: log.txt is written without a lock (LogFileSink.h), so no
# thread that logs - the GPU thread, the emulated CPU's, a crash handler -
# queues on a mutex for it, or waits on one a crashed thread holds.
#
# Nor do the per-frame handoffs between retro_run and the other threads: the
# frontend's Vulkan queue lock (LibretroVkQueue, every submit and every
# frame), the GamePad's motion and its rumble requests, and AX's use of the
# audio devices every 3 ms (AudioDevicesGate.h); and the two the emulated CPU
# hits hardest, both spinlocks before: every guest time read on every core
# (PPCTimerClock.h) and every data cache flush it reports to the GPU, down to
# each dcbf/dcbst (AtomicPageSet.h); the Vulkan renderer's destruction queue
# and the pipelines each shader and framebuffer is used by, which the pipeline
# cache's compile threads all register at once while a title loads
# (AtomicPtrBag.h); and the worker queues - GX2's event callbacks, the
# thread deallocators, curl's workers, the pipeline cache writer (FixedQueue.h)
# and the two pipeline compile pools (MpmcQueue.h) - instead of
# ConcurrentQueue's mutex and condition variable; and the shader compile pool,
# whose queue a shader is claimed from rather than taken out of
# (ClaimTicket.h), and whose compile state is atomic. Online, the nex thread
# and the logins sleep in select() on their sockets and a wake socket
# (nexWait.h) instead of polling every millisecond - or, registering for
# secure access, spinning. And the emulated controllers the game reads every
# frame - which controllers, their mappings (RcuCell.h), their state and
# settings (SeqlockValue.h) - instead of a shared_mutex, a mutex and a
# spinlock per read. The IOSU ioctl queues (FixedQueue.h, a mutex, a ring and a
# counting semaphore before) and the shader cache's async writer likewise.
# The IOSU kernel every IPC passes through - message queues, timers, device
# handles, the command pool, the replies - and each core's IPC buffers.
# The semaphores the idle cores wait on their run queue with, and the H.264
# decoder, the IOSU services and the Vulkan pipeline cache (Semaphore.h).
#
# The waits on the way from loading a title to its first frame sleep until
# what they wait for has happened (LatteGpuWake, ThreadStartedFlag), not by
# the clock. Each of them used to check every 1 to 50 ms, and the boot paid for
# every one: the GPU thread's start, the title starting, the graphic packs,
# the GPU's init, GX2Init, the IOSU services' threads. The GPU thread's pause
# gate went the same way, from a mutex and a condition variable to the wake.
#
# Checked in the source as the libretro build sees it (includes dropped, so
# only the file's own conditionals decide): no sleep_for in any of these
# functions, and the wake each one waits on is there.
cd "$(dirname "$0")/.." || exit 1
CXX=${CXX:-c++}
fail=0

pp()
{
	sed '/^[[:space:]]*#[[:space:]]*include/d' "$1" |
		$CXX -E -P -DENABLE_LIBRETRO -DENABLE_VULKAN -x c++ - 2>/dev/null
}

# the body of the function whose definition line matches $2, in file $1
body()
{
	pp "$1" | awk -v start="$2" '
		!on && $0 ~ start { on = 1 }
		on {
			print
			n += gsub(/\{/, "{"); n -= gsub(/\}/, "}")
			if (seen && n == 0) exit
			if (n > 0) seen = 1
		}'
}

# check FILE FUNCTION-REGEX MUST-CONTAIN
check()
{
	b=$(body "$1" "$2")
	if [ -z "$b" ]; then
		echo "$1: $2 not found"; fail=1; return
	fi
	if echo "$b" | grep -q 'sleep_for'; then
		echo "$1: $2 sleeps by the clock:"; echo "$b" | grep -n 'sleep_for'; fail=1
	fi
	if ! echo "$b" | grep -q "$3"; then
		echo "$1: $2 does not wait on $3"; fail=1
	fi
}

L=src/Cafe/HW/Latte/Core/LatteThread.cpp
check $L '^void Latte_Start\(\)' LatteGpuWake_WaitUntil
check $L '^int Latte_ThreadEntry\(\)' LatteGpuWake_WaitUntil
check $L '^void Latte_GpuPauseGate\(\)' LatteGpuWake_WaitUntil
check src/Cafe/GraphicPack/GraphicPack2.cpp '^void GraphicPack2::WaitUntilReady\(\)' LatteGpuWake_WaitUntil
check src/Cafe/CafeSystem.cpp '^void cemu_initForGame\(\)' LatteGpuWake_WaitUntil
check src/libretro/CemuLibretro.cpp 'libretro_prepare_and_launch_title\(\)$' LatteGpuWake_WaitUntil
check src/Cafe/IOSU/nn/iosu_nn_service.cpp 'void IPCSimpleService::Start\(\)' 'm_threadInitialized.Wait()'
check src/Cafe/IOSU/nn/iosu_nn_service.cpp 'void IPCService::Start\(\)' 'm_threadInitialized.Wait()'
check src/Cafe/IOSU/ODM/iosu_odm.cpp 'void Initialize\(\)' 's_threadInitialized.Wait()'
check src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp '^uint32 LatteCP_readU32Deprc\(\)' LatteCP_waitForWork
check src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp '^static void LatteCP_waitForWork\(\)' LatteGpuWake_WaitUntil
check src/libretro/CemuLibretro.cpp '^static void libretro_gpu_wait_until\(' LatteGpuWake_WaitUntil
check src/libretro/CemuLibretro.cpp 'static void RETRO_CALLCONV libretro_audio_thread_pull\(\)' WaitForFeed
check src/libretro/CemuLibretro.cpp 'static void RETRO_CALLCONV libretro_audio_thread_state\(' WakeFeedWaiter
check src/libretro/LibretroAudioAPI.cpp '^void LibretroAudioAPI::WaitForFeed\(' WaitUntil
check src/Cafe/TitleList/TitleList.cpp '^void CafeTitleList::WaitForScan\(\)' 'sTLScanDone->WaitUntil'
check src/Cafe/TitleList/TitleList.cpp '^void CafeTitleList::WaitForMandatoryScan\(\)' WaitForScan
check src/libretro/CemuLibretro.cpp '^static void libretro_rescan_titles\(\)' WaitForScan
check src/Cafe/IOSU/legacy/iosu_ioctl.cpp '^bool iosuIoctl_waitForWorkersToStop\(' 'sIoctlWorkersStopped->WaitUntil'
check src/Cafe/IOSU/legacy/iosu_ioctl.cpp '^IosuIoctlWorkerScope::~IosuIoctlWorkerScope\(\)' 'sIoctlWorkersStopped->Notify'
check src/Cafe/IOSU/legacy/iosu_nim.cpp 'void iosuNim_waitUntilPackageListReady\(\)' 'sNimPackageListReady->WaitUntil'
check src/Cafe/IOSU/legacy/iosu_nim.cpp 'void nim_backgroundThread\(\)' iosuAct_waitForAccountData
check src/Cafe/IOSU/legacy/iosu_act.cpp '^void iosuAct_loadAccounts\(\)' 'sActAccountDataLoaded->Notify'
check src/Cafe/IOSU/nn/boss/boss_service.cpp 'void BossDaemonThread\(\)' 'BossWake().WaitUntil'
check src/Cafe/IOSU/nn/boss/boss_service.cpp 'void Stop\(\)' 'BossWake().WaitUntil'
check src/Cafe/IOSU/nn/boss/boss_service.cpp 'void SetStateLocked\(' 'BossWake().Notify'
if pp src/Cafe/IOSU/nn/boss/boss_service.cpp | grep -q 'sleep_for'; then
	echo "boss_service.cpp still sleeps by the clock"; fail=1
fi
if pp src/Cafe/IOSU/nn/boss/boss_service.cpp | grep -q 'm_taskState = TaskState::'; then
	echo "boss_service.cpp: a task changes state without notifying"; fail=1
fi
for f in src/Cemu/Logging/CemuLogging.cpp src/Cemu/Logging/LogFileSink.h \
	src/libretro/LibretroVkQueue.cpp src/libretro/LibretroVkQueue.h \
	src/util/helpers/SeqlockValue.h src/util/helpers/MpscList.h \
	src/audio/AudioDevicesGate.h src/audio/IAudioAPI.h src/audio/IAudioAPI.cpp \
	src/Cafe/OS/libs/snd_core/ax_out.cpp \
	src/Cafe/HW/Espresso/PPCTimer.cpp src/Cafe/HW/Espresso/PPCTimerClock.h \
	src/Cafe/HW/Latte/Core/LatteBufferCache.cpp src/util/helpers/AtomicPageSet.h \
	src/Cafe/HW/Latte/Renderer/Vulkan/CachedFBOVk.h src/Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h \
	src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.cpp \
	src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h src/util/helpers/AtomicPtrBag.h \
	src/util/helpers/FixedQueue.h src/util/helpers/lf_ring.c src/util/helpers/MpmcQueue.h \
	src/Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.cpp src/util/helpers/ClaimTicket.h \
	src/Cemu/nex/nex.cpp src/Cemu/nex/nex.h src/Cemu/nex/nexThread.cpp src/Cemu/nex/nexWait.h \
	src/input/emulated/EmulatedController.cpp src/input/emulated/EmulatedController.h \
	src/input/api/Controller.cpp src/input/api/Controller.h src/input/api/ControllerState.h \
	src/util/helpers/RcuCell.h src/Cafe/IOSU/legacy/iosu_ioctl.cpp \
	src/Cafe/IOSU/kernel/iosu_kernel.cpp src/Cafe/OS/libs/coreinit/coreinit_IPC.cpp \
	src/util/helpers/IndexFreeList.h src/util/helpers/Semaphore.h; do
	if pp $f | grep -qE 'std::[a-z_]*mutex|std::(shared|unique|scoped)_lock|lock_guard|condition_variable|sleep_for|FSpinlock'; then
		echo "$f: takes a lock or sleeps again"; fail=1
	fi
done
for f in src/Cafe/OS/libs/gx2/GX2_Event.cpp src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp \
	src/Cafe/OS/libs/nlibcurl/nlibcurl.cpp src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.cpp \
	src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.cpp src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h; do
	if pp $f | grep -qE '(^|[^C])ConcurrentQueue<'; then
		echo "$f: a worker queue is back on ConcurrentQueue's mutex"; fail=1
	fi
done
for f in src/Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.cpp src/Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h; do
	if pp $f | grep -qE 'StateSemaphore|CounterSemaphore'; then
		echo "$f: the shader compile pool waits on a mutex again"; fail=1
	fi
done
if pp src/Cemu/FileCache/file_cache.c | grep -qE 'condition_variable|scond_|slock_'; then
	echo "src/Cemu/FileCache/file_cache.c: the async writer waits on a condition variable or a lock again"; fail=1
fi
# InputManager keeps upstream's mouse and touch locks, which the libretro
# build never takes; the controller lists are lock-free
if pp src/input/InputManager.cpp | grep -q 'lock(m_mutex)'; then
	echo "src/input/InputManager.cpp: the controller lists take a lock again"; fail=1
fi
# VulkanRenderer still has a reader-writer lock around vkCreateGraphicsPipelines
# for the cache saver, so only the spinlock is checked for there
for f in src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp; do
	if pp $f | grep -q 'FSpinlock\|m_spinlockDestructionQueue'; then
		echo "$f: the destruction queue takes a lock again"; fail=1
	fi
done
if pp src/libretro/CemuLibretro.cpp | grep -qE 's_motion_mutex|s_vpad_rumble_mutex'; then
	echo "CemuLibretro.cpp: the motion or rumble handoff takes a lock again"; fail=1
fi
if pp src/libretro/CemuLibretro.cpp | grep -q 'libretro_gpu_wait_step'; then
	echo "CemuLibretro.cpp: a GPU wait still steps a millisecond at a time"; fail=1
fi
check src/Cafe/HW/Latte/Core/LatteAsyncCommands.cpp '^void LatteAsyncCommands_waitUntilAllProcessed\(\)' LatteGpuWake_WaitUntil

if pp $L | grep -q 'condition_variable'; then
	echo "$L: the pause gate is back on a condition variable"; fail=1
fi

# What wakes them: every one of these has to notify, or a wait above lasts
# until something else happens to.
need()
{
	if ! pp "$1" | grep -A2 -- "$2" | grep -q "$3"; then
		echo "$1: $2 is not followed by $3"; fail=1
	fi
}
need src/Cafe/CafeSystem.cpp 'sSystemRunning = true;' LatteGpuWake_Notify
need $L 'sLatteThreadFinishedInit = true;' LatteGpuWake_Notify
need $L 'g_isGPUInitFinished = true;' LatteGpuWake_Notify
need $L 'sLatteThreadAlive.store(false' LatteGpuWake_Notify
need $L 'sGpuPauseRequested.store(false' LatteGpuWake_Notify
need src/Cafe/GraphicPack/GraphicPack2.cpp 's_isReady = true;' LatteGpuWake_Notify
need src/Cafe/OS/libs/gx2/GX2_Misc.cpp 'gx2InitCalled++;' LatteGpuWake_Notify
need src/Cafe/OS/libs/coreinit/coreinit_OSScreen.cpp 'flipRequestCount++;' LatteGpuWake_Notify
need src/Cafe/HW/Latte/Core/LatteAsyncCommands.cpp 'LatteAsyncCommandQueue.push' LatteGpuWake_Notify
need $L 'sGpuAtPauseGate.store(true' LatteGpuWake_Notify
need $L 'sGpuParked.store(true' LatteGpuWake_Notify
need $L 'sGpuHandingContextBack.store(false' LatteGpuWake_Notify
if ! body src/libretro/LibretroAudioAPI.cpp '^bool LibretroAudioAPI::FeedBlock\(' | grep -q 'GetFeedWake().Notify()'; then
	echo "FeedBlock does not wake RetroArch's audio thread"; fail=1
fi
if ! body src/Cafe/HW/Latte/Core/LatteAsyncCommands.cpp '^void LatteAsyncCommands_checkAndExecute\(\)' | tail -3 | grep -q LatteGpuWake_Notify; then
	echo "LatteAsyncCommands_checkAndExecute does not wake the queue's waiter"; fail=1
fi
if ! body src/Cafe/OS/libs/TCL/TCL.cpp 'int TCLSubmitToRing\(' | grep -q LatteGpuWake_Notify; then
	echo "TCLSubmitToRing does not wake the GPU thread"; fail=1
fi

[ $fail -eq 0 ] && echo "GPU, boot, title scan and IOSU waits sleep until woken, none by the clock or spinning; logging and the per-frame handoffs take no lock"
exit $fail
