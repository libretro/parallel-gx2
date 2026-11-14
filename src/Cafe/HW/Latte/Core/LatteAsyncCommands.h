#pragma once

struct LatteSurfaceCopyParam;
struct LatteSurfaceCopyRect;

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect);
void LatteAsyncCommands_waitUntilAllProcessed();
bool LatteAsyncCommands_isEmpty(); // every copy queued has run

void LatteAsyncCommands_checkAndExecute();
