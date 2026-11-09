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

In: `test/pso` gates the compile corpus (each module's SPIR-V instruction
count, and the corpus' modules, words and instructions) and the file
cache's load path (allocations and bytes allocated per entry read), with
rslc's time per shader reported; `PSO_TRACE=1` traces the draw path in the
core. The pipelines a replayed draw sequence makes are counted by the
renderer's own key (`VulkanPipelineKey_Graphics`) for each set of state the
device can leave to the draw, and gated; the state those sets leave out is
drawn on lavapipe by `test/dynstate`. Still to come under M0: the Latte
programs generated from a seed.

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
  builds; `PsoTrace.h`): every decompile, every shader compile or wait for
  one and every pipeline creation a draw waits for, logged as
  `PSO_TRACE frame N <what> <key> <ms>`, and every draw skipped while its
  pipeline compiles, so a stutter a tester reports is attributed to a
  cause, not guessed at.

### M1: fewer pipelines (cut the permutations)

Done: the stencil reference, compare mask and write mask are core dynamic
state on every device, set by the draw (`draw_updateStencilState`) and out
of the key (`VulkanPipelineKey.cpp`); `test/pipeline_key` replays 1024
stencil states (one key, where there were as many as values) and checks
that each state a pipeline still bakes in keeps a key of its own.
Pipelines are built from the state they are handed, never the live
registers (`tools/check_pipeline_registers.sh`).

Done with `VK_EXT_extended_dynamic_state` (enabled on the core's device
where the GPU has it): cull mode, front face, depth test, write and compare
and the stencil test and operations are set by the draw
(`draw_updateDepthStencilCull`) from the same function the pipeline
compiler uses (`VulkanPipelineKey_DepthStencilCull`), and out of the key;
`test/pipeline_key` replays 4096 such states (one key) and checks the
values against what a pipeline baked in.

Done with `VK_EXT_extended_dynamic_state3` where the GPU has its color
blend enable, equation and write mask features: the blend state is set by
the draw (`draw_updateBlend`, from `VulkanPipelineKey_Blend`, which the
pipeline compiler uses too) and out of the key, all but the logic op;
`test/pipeline_key` replays 4096 blend states (one key).

Done with `VK_EXT_extended_dynamic_state2`: the polygon offset enable and
rasterizer discard (with the GX2SetSpecialState exception for the
viewport's X offset) are set by the draw (`draw_updateRasterizer`, from
`VulkanPipelineKey_Rasterizer`, which the pipeline compiler uses too), the
depth bias values are dynamic in every such pipeline, and both enables are
out of the key; `test/pipeline_key` replays the 64 combinations (one key)
and 1024 states with all three extensions' state varied (one key). Its
primitive restart is not used: restart follows the topology, which the
pipeline keys anyway. The discard bit also sat in the part of the key the
fast draws compare (`VulkanPipelineKey_Minimal`), which split the
pipelines anyway; it is out of that part too since vertex input went in.

Done with `VK_EXT_vertex_input_dynamic_state`: the vertex buffer strides
and attribute offsets are set by the draw (`draw_updateVertexInput`, from
`VulkanPipelineKey_VertexInput`, which the pipeline compiler uses too) and
are out of the key. The attribute formats and locations stay the vertex
shader's, which is compiled for the fetch shader's layout; fetch shaders
that differ only in offsets, the case the note in
`draw_calculateGraphicsPipelineHash` describes, now share pipelines.
`test/pipeline_key` replays 256 vertex layouts (one key).

Done with `VK_EXT_extended_dynamic_state` and `_2` together: the primitive
topology and restart are set by the draw (`draw_updateInputAssembly`, from
`VulkanPipelineKey_InputAssembly`, which the pipeline compiler uses too),
and the key keeps the primitive type's class only (points, lines,
triangles; line strips with adjacency and rects each their own, rects for
their geometry shader or fill mode): a dynamic topology has to be of the
pipeline's class. Lists, strips, fans, quads and quad strips of the same
shaders share a pipeline.

The renderer's whole key is `VulkanPipelineKey_Graphics`, and `test/pso`
replays 2000 draws from a seed with a frame's shape (40 shader sets in a
scene, shadow and post pass; materials, stencil references, decals,
depth-only draws, strips and fans, two attribute offset layouts and five
strides) through it, gating the pipelines for each set of state left to
the draw: 1511 with nothing, 1484 with extended dynamic state, 1314 with
_3's blending, 1199 with _2, 302 with vertex input, 119 with topology.
The sequence is synthetic. A title's own draws would replace it as a
private corpus; `PSO_TRACE` logs what a draw waited for, not the state it
drew with, so recording them is still to do.

`test/dynstate` draws 400 register states from a seed on lavapipe, each
with a pipeline that bakes the depth, stencil, cull, blend, polygon offset,
discard state, vertex input and topology in and one that takes it from the
draw (through the same setters the renderer uses,
`VulkanPipelineKey_SetDepthStencilCull`, `_SetBlend`, `_SetRasterizer`,
`_SetVertexInput` and `_SetInputAssembly`): quads of four vertices drawn
as a list, a strip or a fan, read from a vertex buffer in a layout from
the seed (buffer slots, semantics, stride, offset, a per-instance
attribute), and requires the same picture to the byte. Next: recording a
title's draws for the gate, then graphics pipeline libraries (M2).

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

- Done: `FileCache` reads an entry stored as is straight into the
  caller's buffer, and a compressed one into a buffer the reading thread
  keeps, inflating from there into the caller's: one allocation per entry
  read, the caller's own (the gate counted two before). Finding an entry by
  name, a free entry and free space no longer scans the file table (a
  hashed index, a free-entry hint and a list of gaps, built on open); the
  gate counts the table entries looked at to write and to find 1000
  entries (9035857 and 501500 before; 6510 and 1321 now). The table grows
  by doubling, and its old copy is given up only once the header points
  past it.
- Still to do: map the cache file once (VFS where the frontend gives one,
  a native mapping otherwise) and inflate from the mapped span, with no
  read buffer at all; read without the cache's mutex (positional reads,
  the table published RCU).
- Done: the `VkPipelineCache` file is handed to `vkCreatePipelineCache`
  mapped (`MappedFile`) and unmapped once the driver has its copy. It is
  saved when its contents changed (a CRC, not its size), through a file
  that then takes its name. The compile threads no longer take a
  reader-writer lock around `vkCreateGraphicsPipelines` for the save's
  sake, which held every compile, the GPU thread's synchronous ones
  included, behind a save; the save waits for a moment none is building a
  pipeline (`IdleWait`) and nothing waits for it.
- The boot load of the transferable shader and pipeline caches: done, each
  entry rebuilt its 256 KiB register file in a new, zeroed allocation (and
  a pipeline whose shader was missing leaked it, on Vulkan and Metal); a
  thread's one register file is reused now (`LoadGPURegisterStateScratch`,
  gated: no allocation per 1000 rebuilt), and the shader load reads every
  entry into one buffer. Still to do: the entries' programs are inflated
  into vectors of their own.
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
