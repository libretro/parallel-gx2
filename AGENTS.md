# Rules for coding agents

These apply to any AI agent working in this repository (Claude Code reads them through `CLAUDE.md`). They came out of testing the libretro cores with their testers.

## Branches and history

- The `libretro` branch is never deleted, and nothing that rewrites its history is run on it: no force-push, no rebase, no amending or squashing of commits that are already pushed. It is the fork's only long-lived branch.
- New features and experimental changes go on their own branch, and testers test builds of that branch. When testing is done, the related commits are squashed and merged into `libretro`, so the main branch does not collect commits that were superseded midway.
- A fix that users of the `libretro` builds need before the branch is merged is cherry-picked into `libretro`, in a way that does not break the later merge of the branch.

## Merging upstream Cemu

This repository builds the libretro core and nothing else. The standalone's parts - the wx GUI, its packaging and workflows, the input backends the frontend replaces (SDL, DSU, keyboard, real Wiimotes, GameCube adapter, XInput, DirectInput), the audio backends (cubeb, XAudio2, DirectSound), Discord RPC, GameMode and upstream's contributor docs - were deleted on purpose, and must not come back with a merge. The paths are listed in `.upstream-excluded`.

- Only full merges of upstream, no cherry-picked upstream commits.
- Right after `git merge` of upstream, before resolving anything else, run:
  `git rm -r -q --ignore-unmatch --pathspec-from-file=.upstream-excluded`
  This settles the modify/delete conflicts in favour of the deletion, and it also removes files upstream newly added under those paths, which git would otherwise bring in without any conflict.
- Never resolve a conflict on one of those paths by restoring the file.
- When upstream changes a CMake file around one of the removed options or backends, keep them removed and take the rest of the change.
- When something new is deleted for the same reason, add its path to `.upstream-excluded` in the same commit.
- After a merge, the core's version (`library_version` in `src/libretro/CemuLibretro.cpp` and `cemu_libretro.info`) is `git describe` of the upstream commit merged, and a new release is built.
- `CHANGELOG.md` is this fork's own. When a merge conflicts on it, keep this one.

## These files

- Only the developer writes to or deletes `AGENTS.md` and `CLAUDE.md`. An agent does not change them on its own; it proposes the change to the developer instead.

## Changelog and README

- `CHANGELOG.md` is updated in the same commit as the change it describes: what changed for someone using the core, newest first. Not a copy of the commit message.
- A change on a feature branch gets its entry on that branch, under "Unreleased"; it reaches `libretro` with the squash merge.
- `README.md` is kept true in the same way: when a change makes a sentence in it wrong (a limit lifted, an option added, a file moved), fix the sentence in that commit.

## Code

- Keep comments true. When a change makes a comment describe behaviour that no longer exists, fix or remove the comment in the same commit.
- Look at the big picture, not just the function being changed. For example, resetting the content and closing it have to release the game's resources through the same path; a reset is an unload and a load.
- Stop and join threads the way upstream Cemu does, not with methods made up for the libretro port. The ad-hoc ones are what caused most of the shutdown and reset bugs.
- Every core option has to be connected to something. Do not add an option the core does not read, and remove one that turns out to do nothing. Option defaults follow the standalone's defaults unless there is a reason, written down beside the option.
- `settings.xml` is not used by this core. If you need a setting, use the current `.opt` file (a core option) instead of Cemu's `settings.xml`.

## Shader compilation and stutter

The first play of a game, with no shader, pipeline or driver cache on disk, has to be as smooth as playing it with all of them full: no compile is ever seen in gameplay. The plan, what the core does today and how to measure it are in `docs/SHADER-STUTTER.md`. These rules apply to every change that touches the shader or pipeline path - the decompiler (`LatteSHRC`, `LegacyShaderDecompiler`), shader compilation (`RendererShaderVk`, `RslcGlsl.h`, libretro-common's `gfx/shaders`), pipelines (`VulkanPipelineCompiler`, `VulkanPipelineStableCache`, the draw path in `VulkanRendererCore.cpp`), the caches (`LatteShaderCache`, `FileCache`, the `VkPipelineCache` blob) and the file I/O under them.

- Shader compilation only gets better. The shader/PSO gate (`test/pso/run_tests.sh`, in both CIs) fails when a number in `test/pso/baseline.txt` gets worse, and also when one gets better without the baseline being updated in the same commit (`make -C test/pso baseline`). A number that has to get worse for correct output is the developer's call, stated in the commit message. Put the before and after numbers in the commit message. What the gate does not count yet - pipelines and synchronous creations on the draw path, compile times - is measured by hand as `docs/SHADER-STUTTER.md` describes, with a `PSO_TRACE=1` build for the draw path, and gets added to the gate as the milestones bring it in; it may not get worse either.
- Never compile a pipeline on the draw path. The GPU thread does not compile a pipeline, or the shaders it is made of, while a draw waits for it: not for a new state, not for a draw judged essential, not with Async Shader Compile off, not on a device without `VK_EXT_pipeline_creation_cache_control`. Pipelines are compiled ahead on the workers; at the draw there is at most a link of parts already compiled (graphics pipeline libraries, linked without optimization). The draw paths that still compile are listed in `docs/SHADER-STUTTER.md` ("Compiles on the draw path"); removing them is the top priority of the shader and pipeline work and comes before any other change to it, and no change adds one.
- Never block a draw on a compile. No new synchronous shader compile, pipeline creation, link or wait for a worker on the GPU thread for a state that can be drawn another way. Skipping the draw is not another way; a fallback draws exactly what the game asked for.
- The case that counts is the cold one: no caches of any kind, the driver's included. Measure cold and warm separately; a change that only helps warm runs does not meet the goal.
- Count before timing, and attribute a stutter before fixing it: which frame, which stage, which state, how long. Timings are medians, p95, p99 and max over repeated runs in alternating order on an otherwise idle machine, and the emulated work and the pictures must be the same.
- Every key change is traced to every consumer of the key, and a change to what a cached entry means bumps that cache's version. The transferable shader cache and the pipeline cache stay in the format standalone Cemu reads and writes; anything new the core persists goes in its own file.
- The shader path's file I/O is zero-copy: map or borrow instead of reading a whole file into a buffer, decompress or build straight into the final destination, move compiled SPIR-V instead of copying it. Every borrow names its owner and how long it lives, up to the last CPU and GPU reader. No per-entry allocations on a cache load path.
- Work for later runs on the workers, handed over through the lock-free queues in `util/helpers` and sleeping on an eventcount; optional work goes in the frame's slack. Never a lock, a sleep or a poll on this path.
- Changes to rslc go to RetroArch's libretro-common, never to the core's copy. A shader that compiles faster but runs slower on the GPU is a regression too: SPIR-V instruction counts are part of the gate.

## Testing

- `log.txt` names the commit it was built from, under "Loaded title". Ask testers for `log.txt` and the RetroArch log, and check which commit a log came from before drawing conclusions from it.
- Compare with standalone Cemu at the same upstream version before calling something a core bug; when standalone fails the same way, it is upstream's.

## Libretro pitfalls already hit in the other cores

Each of these was a bug in at least one of the cemu, rpcs3, vita3k or xenia cores. Check new code against them before asking testers.

- Audio: each retro_run hands the frontend exactly one frame's worth (sample rate / declared fps), never "whatever is queued". The game's audio thread waits once about 64 ms is queued; no bigger buffer on the core's side. Several streams (audio ports, clients, a music player) are mixed, not appended one after another, and each is taken in its own format and sample rate.
- Geometry: the size the frame is handed over at goes to the frontend: base geometry at load, SET_GEOMETRY when it changes, SET_SYSTEM_AV_INFO when it would exceed the max. The max has to cover the highest internal resolution.
- Pacing: the guest's vblank follows retro_run, and the core declares the guest's refresh rate; the frontend paces display and audio, the core does not sleep to pace itself.
- Files: everything the core writes goes under system/<core>/ (or saves/); nothing next to the RetroArch executable. The emulator's own log goes there too, and its warnings and errors also go to RetroArch's log.
- No window: the standalone's UI (on-screen keyboard, notifications, message boxes, profile dialogs) has no window in a core. Every path that reaches for the window or its UI thread needs a headless branch that answers the way the user most likely would.
- Unload: never end the process (no exit, no TerminateTitle-style shutdown). Stop and join every thread in upstream's order, and never wait without a timeout on a fence, event or thread the frontend has to drive. No vkQueueWaitIdle/vkDeviceWaitIdle while RetroArch holds its queue lock.
- Libraries that pin themselves (statically linked OpenSSL) keep the core loaded after dlclose; build them so they do not.
- No downloads at run time; data files ship with the core.
