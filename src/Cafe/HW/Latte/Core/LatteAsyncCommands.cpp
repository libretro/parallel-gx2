#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteGpuWake.h"
#include "util/helpers/DoneQueue.h"

void LatteThread_Exit();

// A GX2 surface copy a CPU thread hands the GPU thread and waits to see done
// (GX2CopySurface): no lock, and the copies in a fixed ring. Each thread that
// queues one waits for it, so a few are ever queued at once.
struct LatteAsyncCommand_t
{
	LatteSurfaceCopyParam src;
	LatteSurfaceCopyParam dst;
	LatteSurfaceCopyRect rect;
};

static DoneQueue<LatteAsyncCommand_t, 64> s_asyncCommands;

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	LatteAsyncCommand_t asyncCommand = {src, dst, rect};
	// full: asleep until the GPU thread has run some (it notifies)
	if (!s_asyncCommands.TryPush(asyncCommand))
		LatteGpuWake_WaitUntil([&] { return s_asyncCommands.TryPush(asyncCommand) || Latte_GetStopSignal(); });
	LatteGpuWake_Notify(); // the GPU thread, asleep for want of work
}

// Asleep until the GPU thread has run every copy queued (it notifies), rather
// than spinning on it - or until it is stopping, after which it never will.
void LatteAsyncCommands_waitUntilAllProcessed()
{
	LatteGpuWake_WaitUntil([] { return LatteAsyncCommands_isEmpty() || Latte_GetStopSignal(); });
}

bool LatteAsyncCommands_isEmpty()
{
	return s_asyncCommands.AllDone();
}

/*
 * Called by the GPU command processor frequently
 */
void LatteAsyncCommands_checkAndExecute()
{
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	LatteAsyncCommand_t asyncCommand;
	bool ran = false;
	while (s_asyncCommands.Take(asyncCommand))
	{
		LatteSurfaceCopy_copySurfaceNew(asyncCommand.src, asyncCommand.dst, asyncCommand.rect);
		s_asyncCommands.Done();
		ran = true;
	}
	if (ran)
		LatteGpuWake_Notify(); // LatteAsyncCommands_waitUntilAllProcessed, and a full queue's pusher
}
