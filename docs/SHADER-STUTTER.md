# Shader compilation and stutter: the plan

The goal: the first time a game is played, with no shader cache, pipeline
cache or driver cache on disk, it plays as smoothly as it does with all of
them full. No shader or pipeline compile is ever seen in gameplay. A Wii U
loads microcode the GPU runs as it is; this core should come as close to
that as a PC allows, by doing less work, doing it before it is needed, and
never making a draw wait for it.

And it only ever gets better. Every change that touches the shader or
pipeline path is measured against the last, and one that makes any of the
numbers below worse does not land (AGENTS.md, "Shader compilation and
stutter").

This is a plan, not a description of the core today. Where it says what the
core does now, that is as of `495ded87`.

## Where the time goes today

A draw with state the renderer has not seen goes through, on the GPU
thread:

1. **Decompile.** `LatteSHRC` decompiles the guest's vertex, geometry and
   pixel programs to GLSL the first time each (base hash, aux hash) is seen.
2. **Compile.** `RendererShaderVk` compiles the GLSL to SPIR-V with rslc
   (`RslcGlsl.h`, libretro-common `gfx/shaders`) and makes the
   `VkShaderModule`. The compile pool can take it off the GPU thread, but
   `PipelineCompiler::Compile` calls `PreponeCompilation` and does it there
   when the draw needs it now.
3. **Pipeline.** `draw_createGraphicsPipeline` hashes the full state and
   calls `vkCreateGraphicsPipelines` for it. The only dynamic state is the
   blend constants, depth bias and the feedback loop; blend, depth, stencil,
   cull, primitive topology, vertex input and the render pass are all baked
   in, so every combination of them with the same shaders is a new pipeline
   and a new driver compile.
4. **What "Async Shader Compile" does.** With the option on (the default),
   a draw the heuristics in `IsAsyncPipelineAllowed` call non-essential is
   **skipped** while its pipeline compiles on a worker. That hides the
   stall by dropping geometry. Off, the draw waits for the compile. Neither
   is acceptable as the end state: one is wrong output, the other is the
   stutter.

What is persisted: the transferable shader cache (`shaderCache/transferable`,
`FileCache`, entries in the format standalone Cemu reads), the pipeline
stable cache (`VulkanPipelineStableCache`, also standalone's format), and the
driver's `VkPipelineCache` blob. At boot `LatteShaderCache_Load` compiles
everything in them behind a progress screen. That moves the stall to boot;
it does not remove it, and it does nothing for a first play.

## Rules every step follows

- **Measure first, then change, then measure again.** Counts (compiles,
  pipelines created on the draw path, bytes copied, allocations) are exact
  and go in CI. Times are taken locally with the method below. A change
  that improves a count but leaves the times flat is reported as flat.
- **Correct before fast.** A fallback draws what the game asked for. A
  skipped draw, a guessed state or a placeholder shader is not a fallback.
  A validator that passes is not proof: GPU results are compared (lavapipe
  in CI, a real GPU locally).
- **Never block the draw.** No synchronous compile, link or wait for a
  worker on the GPU thread for a state that has a fallback. A miss that
  has none is counted, attributed and becomes the next thing to fix.
- **Zero-copy with a proven owner.** Map or borrow instead of slurping a
  file into a buffer; decompress or build in the final destination; move
  ownership of compiled words instead of copying them. Every borrow names
  who owns the bytes and until when (the last CPU and GPU reader). A copy
  that protects a lifetime stays.
- **No locks, no clock polling.** Workers are handed jobs through the
  lock-free queues already in `util/helpers`; a worker with nothing to do
  sleeps on an eventcount. Optional work runs in the slack the frontend
  leaves, never by stretching a frame.
- **Cache formats stay standalone's.** The transferable shader cache and
  the pipeline cache are shared with standalone Cemu. Anything new the core
  persists goes in its own file with its own version.

## Milestones

Each is one or more patches, each patch with its tests and the gate numbers
in its commit message.

### M0: the gate (first; nothing else lands before it)

`test/pso`, run in both CIs on every commit:

- **Corpus.** Latte programs the decompiler is fed: the decompiler shapes
  `test/shaders` already covers, programs generated deterministically from
  a seed (ALU, texture, control flow, exports, every sampler kind), and the
  graphic pack shaders. No game data is committed; the developer's own
  caches run as a private corpus with the same tool.
- **Counted, exact, gated.** Per corpus: decompiles, rslc compiles, SPIR-V
  words and instructions, `VkShaderModule`s and `VkPipeline`s created,
  pipelines created synchronously on the draw path by a replayed draw
  sequence (state changes that hit the same shaders with new blend, depth,
  cull and render pass state), allocations and bytes copied on the cache
  load path. Pipelines are real: lavapipe in CI.
- **Baseline.** `test/pso/baseline.txt` is checked in. A number that goes
  up fails the job. A commit that makes one go down updates the file in
  the same commit. A number that has to go up (more correct output costs
  more) is for the developer to accept, stated in the commit message.
- **Timed, locally.** The same tool times rslc per shader, module creation
  and pipeline creation (CPU time around each driver call), median, p95,
  p99 and max, on the developer's machines: fixed build, no other load,
  warm-up excluded, A/B/B/A order with repeats. Cold (no caches of any
  kind, driver cache included) and warm are separate runs.
- **In the core**, behind a build flag (`PSO_TRACE=1`, nothing in release
  builds): every synchronous compile or pipeline creation on the GPU thread
  logged with the frame, the duration, the stage and the reason (new
  shader, new state, no fallback ready), so a stutter a tester reports is
  attributed to a cause, not guessed at.

### M1: fewer pipelines (cut the permutations)

- Make the state that only multiplies pipelines dynamic where the device
  has it: `VK_EXT_extended_dynamic_state` (cull, front face, topology,
  depth test/write/compare, stencil), `_2` (depth bias enable, primitive
  restart), `_3` where present (blend enable/equation, color write mask),
  and dynamic vertex input. Key only on what the pipeline still bakes in;
  with the capability missing, the key and the pipeline are exactly as
  today.
- Every field dropped from the key is traced to every consumer first;
  dropping state a pipeline still consumes is a correctness bug.
- Gate: pipelines per replayed sequence go down; GPU results unchanged.

### M2: a first draw that never waits

- **Graphics pipeline libraries** where the device has them
  (`VK_EXT_graphics_pipeline_library`): vertex input, pre-rasterization
  (vertex/geometry) and fragment shader libraries built once per shader on
  workers as soon as the shader is known; fragment output per render pass
  format. A new state links them on the GPU thread (fast-link, no
  optimization: tens of microseconds) and draws; an optimized pipeline for
  the same state is compiled on a worker and swapped in when it lands.
- **Without GPL**: the full pipeline is compiled on a worker; until it
  lands, the draw uses the closest pipeline already built for the same
  shaders whose differing state is dynamic (M1). Only a draw with nothing
  usable compiles synchronously, and is counted.
- **Shaders**: decompile and rslc run on the workers as soon as the guest
  sets the shader registers, ahead of the draw that needs them; the draw
  takes a finished module. rslc's own time per shader is in the gate.
- Then "Async Shader Compile" no longer skips draws, and its default, its
  description or its existence is reconsidered on the gate's numbers.
- Gate: synchronous creations on the draw path, per replayed sequence, go
  to zero on GPL devices.

### M3: cold equals warm

- With M1 and M2 in, measure the cold first play against the warm one on
  the developer's titles. The difference left is what to remove next.
- Learned state (which shaders and states a title used) is what makes boot
  preparation possible without a cache; persist the recipes, not driver
  binaries, in the core's own small file, and prepare them on workers
  during the boot screen and in frame slack.
- When cold and warm are indistinguishable in the gate's times and in
  testers' hands, the developer decides whether the shader caches stay on
  by default; they stay available for low-end machines.

### M4: zero-copy I/O for everything the shader path reads and writes

Now (as of `495ded87`):

- `FileCache::_getFileDataInternal` allocates a buffer per entry, reads the
  compressed entry into it, then allocates the output and inflates into it.
  Map the cache file once (VFS where the frontend gives one, a native
  mapping otherwise), inflate from the mapped span straight into the
  caller's destination, and keep one bounded fallback read for what cannot
  be mapped.
- The `VkPipelineCache` blob is read with `std::ifstream` into a vector and
  copied again by the driver. Map it for `vkCreatePipelineCache` and unmap
  after. Saving gets the blob from the driver once per save; save only
  when its contents changed (not when its size did).
- The boot load of the transferable shader and pipeline caches copies every
  entry before deserializing it; deserialize from the borrowed bytes.
- Compiled SPIR-V moves from rslc to the module without another copy;
  reflection and layout data borrow it while it lives.
- Gate: allocations and bytes copied on the load path, per entry.

### M5: shader quality

rslc replaced glslang. glslang's front end does some optimization rslc may
not, so a shader can be slower on the GPU even though it compiles faster.

- The gate records SPIR-V instruction counts per corpus shader; a change
  that grows them is a regression unless it is a correctness fix.
- Reports of "this game got slower" with the same settings are checked
  against the shaders it uses: compare rslc's module with glslang's for
  the same GLSL (offline), and bring the missing optimization into rslc in
  RetroArch's libretro-common, never in the core's copy.

## How to measure without fooling yourself

- One machine, one build, nothing else running; record GPU, driver, OS and
  the capabilities the core enabled.
- Cold means nothing on disk: the core's caches, the pipeline cache and the
  driver's own shader cache (`MESA_SHADER_CACHE_DISABLE=true`,
  `__GL_SHADER_DISK_CACHE=0` and the driver's equivalents). Use a private
  system directory; never empty a tester's.
- Count first, time second. A count that moves is exact; a time needs
  repeats in alternating order and the median, p95, p99 and max, with the
  frame numbers of the outliers.
- Make sure the emulated work is the same: the same title, the same save
  state, the same frames, and the same pictures. Doing less emulation looks
  faster.
- No readbacks, captures or validation layers in a timing run.
- Attribute a hitch before fixing it: the `PSO_TRACE` line for that frame,
  not its look.
