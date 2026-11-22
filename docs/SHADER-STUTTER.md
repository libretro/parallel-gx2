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
   A shader the SPIR-V emitter covers (`LatteDecompilerEmitSPIRV.cpp`:
   pixel and vertex shaders of ALU clauses, control flow, texture samples,
   shadow compares, gathers, texel fetches, texture queries, 1D textures,
   gradients, cube maps, MOVA, relative reads and writes, attributes,
   window-space positions, point sizes, geometry shaders and the vertex
   and pixel shaders of their draws, streamout, uniform buffers
   and exports so far, more as it grows) is
   written as SPIR-V by the decompiler and skips rslc; only its module is
   made. It mirrors what rslc makes of the GLSL - the same operations,
   folding and branches - and `test/decompiler` holds the two to the same
   pixels and the same shape.
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

## Compiles on the draw path

The top priority (AGENTS.md: never compile a pipeline on the draw path).
Where the core still does, as of `204bbde5`:

- **Async Shader Compile off**: every new pipeline is compiled on the GPU
  thread while its draw waits (`draw_createGraphicsPipeline`,
  `Compile(true, ...)`), its shaders with it (`PreponeCompilation`). With
  graphics pipeline libraries, only the parts no pipeline before it had
  (below); a new combination of parts already made is linked, not
  compiled.
- **Async on (the default), a draw judged essential**
  (`IsAsyncPipelineAllowed`: no depth buffer, six indices or fewer, the
  1600x1600 targets): the same, libraries included.
- **Async on, any other draw**: not compiled on the draw path, but the
  draw is skipped until the pipeline lands - not a fallback either. A later
  draw of the same pipeline that may not be skipped (essential, or with
  Async Shader Compile turned off since) is not skipped with it: it
  compiles the pipeline itself when no compile thread has started on it,
  as it would a new one, or waits for the compile thread that has
  (`draw_awaitAsyncPipeline`, `AsyncPipelineCompile`; traced as `pipeline
  async, taken by an essential draw`). It was skipped as well, which could
  leave what it draws wrong for good.
- **Without graphics pipeline libraries**, this is where it stays until
  pipelines are made ahead of their first draw (M3's learned state): a
  draw that may be skipped is, rather than wait for its compile, and one
  that may not compiles or waits, rather than be skipped. Neither is a
  fallback; there is none to draw with yet.
- **No `VK_EXT_pipeline_creation_cache_control`**: no longer. Every
  compile was forced synchronous there (`PipelineCompiler::Compile`), so
  the async try of a draw that may be skipped compiled each new pipeline
  on the GPU thread. A try now takes only what is made - parts already
  made, linked, and otherwise nothing - and the draw is skipped until a
  compile thread lands the pipeline, as on a device that has the extension
  for one its cache does not hold (`VulkanPipeline_Try`; `test/pipeline_key`,
  and `tools/check_compile_try.sh` against upstream's forcing coming back).
  A pipeline known to be in the driver's cache - made through it this
  title, by the pipeline cache's load at boot or on any thread since
  (`PipelineCompiler::MadeBefore`, noted where a compile succeeds; a
  pipeline whose recipe was recorded but whose compile never finished is
  not) - is made by the try all the same, its shaders compiled: a hit in
  that cache, the lookup a device with the extension makes, rather than a
  skipped draw until a compile thread made the same lookup, at every
  pipeline's first draw with a warm cache.
- **Rect emulation geometry shaders** (RECTS where the device has no
  `VK_NV_fill_rectangle`): no longer. Each pipeline compiled its own on the
  thread making it, the GPU thread included, before its draw. One is now
  made for each source written and shared by the pipelines alike
  (`rectsEmulationGS_generate`, `SharedByKey.h`), queued for the compile
  threads and waited for only where the pipeline is: a draw async may skip
  skips until it lands, one that may not compiles it then, as it does its
  other shaders. `test/shared_by_key` checks the sharing, eight threads
  racing for 600 keys (each made once; TSan in CI).
- **Surface copies** (color to depth and back, `VulkanSurfaceCopy.cpp`):
  no longer. The copy shaders are compiled at init, and the pipeline that
  copies into a format is queued for the compile threads when the first
  texture that can be drawn to in it is created (`surfaceCopy_prepare`,
  `PreparedPipeline.h`), ahead of any copy into such a texture. A copy that
  comes before a compile thread started on it builds it itself, one that
  comes while one builds it waits for that build; either is traced
  (`copy pipeline not made ahead`). `test/prepared_pipeline` checks the
  hand-over: taken after the compile threads, nothing is built on the taking
  thread; racing them, each is built once.

M2 removes them; it comes before anything else in this document. What is
left is what no read ahead reaches in time: the reads make shaders, their
parts and whole pipelines before their draws (M2, below) and hand the
pipelines to them, and a draw that comes first is skipped (or, essential,
compiles) until then. How often each happens in a game is in its log at
title stop (`[ShaderAhead]`: shaders taken, pipelines ready, made by the
read, skipped, waited for), which decides what to work on next.

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
The sequence is synthetic; a title's own draws are a private corpus. A
`PSO_TRACE=1` build with `PSO_RECORD=FILE` in its environment appends each
draw state that makes a key of its own with nothing left to the draw - the
finest key, so every set's count follows from it - to FILE
(`VulkanPipelineRecord.h`: the shaders, the render pass, the fetch
shader's buffers and offsets, the register state compacted as the pipeline
cache keeps it; about 100 bytes a state), and `pso_gate --replay FILE`
counts the pipelines each set makes. Sessions append to one file; each
state counts once. The gate round-trips its own draws through the
recording and requires the same counts.

`test/dynstate` draws 400 register states from a seed on lavapipe, each
with a pipeline that bakes the depth, stencil, cull, blend, polygon offset,
discard state, vertex input and topology in and one that takes it from the
draw (through the same setters the renderer uses,
`VulkanPipelineKey_SetDepthStencilCull`, `_SetBlend`, `_SetRasterizer`,
`_SetVertexInput` and `_SetInputAssembly`): quads of four vertices drawn
as a list, a strip or a fan, read from a vertex buffer in a layout from
the seed (buffer slots, semantics, stride, offset, a per-instance
attribute), and requires the same picture to the byte. Next: graphics
pipeline libraries (M2), measured on titles' recordings.

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

### M2: a first draw that never waits (the top priority)

Done: graphics pipeline libraries where the device has
`VK_EXT_graphics_pipeline_library` with fast linking and `_2`
(`VulkanPipelineLibrary.h`; enabled on the core's device). Every pipeline
is linked from four parts - vertex input, pre-rasterization shaders,
fragment shader, fragment output - made from the create info the compiler
fills, each keyed by its shaders' hashes, the render pass's compatibility
and its own state without the fields its dynamic states make the draw's,
and kept in an insert-only lock-free cache the GPU thread and the compile
threads share (one thread claims a key and makes the part, the others find
it pending). On the GPU thread a pipeline whose parts are all made is
linked as they are - a link, not a compile - and a compile thread links
them optimized, which takes the fast-linked one's place at the next draw
(the old one released after the command buffers that may use it). The
layouts' sets are independent, each part with its own shaders' sets. The
parts outlive every pipeline linked from them: lavapipe draws wrong with a
pipeline whose parts were destroyed, so a part made without a claim is
adopted by the cache, and all go with the renderer, after the last
pipeline. `test/dynstate` links every state's pipeline from the cache's
parts, fast and optimized, and requires the baked pipeline's picture (one
part of each kind for 400 states); under the Khronos validation layer in
both CIs, where any error fails. `test/pipeline_key` checks which part
each dynamic state goes to, what moves each part's key, and the cache with
eight threads racing for 4000 keys (each made once; under TSan in CI).

Parts are also made before a pipeline needs them. The GPU thread keeps
the pre-rasterization and fragment shader state of the pipelines it sets
up as templates (`VulkanPipelinePartTemplates`, the 64 most recently used,
each keeping its render pass alive), and the first time a shader is in a
pipeline, a compile thread makes its part for every other template seen so
far - from its module, once compiled, and the template - so that its first
pipeline with another render pass or state links what is made
(`PipelineCompiler::PrepareAheadParts`). A template a job holds is never
replaced; a shader that goes retires its job. `test/pipeline_key` checks
that a part made from a template is keyed as its pipeline looks it up, for
random state, and the table (four threads reading what they hold while it
is recorded over, under TSan); `test/dynstate` makes a new shader pair's
parts ahead on lavapipe, and its first pipeline then makes no part and
draws what the baked pipeline draws, under the validation layer.

A draw skipped until its pipeline lands waits only for the parts: the
compile thread links them fast as soon as they are made, as the GPU thread
does, and queues the optimized link to take its place, where it linked
them optimized first - a whole compile's time more of skipped draws. And
a part another thread is making is waited for, never made a second time:
before, a compile that found its part pending made it again and kept both.
`test/pipeline_key` checks both: the link chosen for a waiting draw and
for none, and eight threads waiting for 200 parts another makes (each the
one published, a failed one missing, none made twice; TSan in CI).

A draw that waits for its pipeline - essential, or with Async Shader
Compile off - and finds parts missing has them made at once rather than one
after the other: the shader parts but the first queued for the compile
threads while the GPU thread makes the first and the cheap ones, then each
taken - made on the GPU thread as well where no compile thread has started
on it (`PreparedPipeline::BuildTogether`). Its wait is for the slowest part,
not their sum. A draw skipped until its pipeline lands has the compile
thread making it do the same, the other compile threads helping, so it is
skipped for that much less. `test/prepared_pipeline` checks the builds overlap with
compile threads free and run one after the other here with none.

A draw whose vertex and pixel shaders are both new has them decompiled
together: the pixel shader's handed to a compile thread while the GPU
thread does the vertex shader's, then taken as the parts are
(`LatteShader_DecompileTogether`), so the wait is for the longer decompile
rather than their sum. The decompiler runs on any thread at once with any
other - its lists and writer each thread's own, made as the thread starts
and freed as it ends, what threads share lock-free; `test/decompiler`
decompiles on four threads at once under TSan, each shader the same as
alone.

Pixel shaders are decompiled ahead of the draw that first uses them. As
the GPU thread starts a command buffer from the ring, a compile thread
reads it ahead (`LattePM4Scan.cpp`) from a copy of the registers as they
are then - its register writes applied as the command processor applies
them, the buffers it calls followed, stopping at a wait the command
processor would wait at - and
at each draw whose pixel shader the GPU thread has never seen it
decompiles that shader from the copy (`LatteShaderAhead.cpp`). The draw
takes it only where the base and aux hashes and every register a pixel
shader's decompile reads (`LattePM4_PixelShaderInputs`) are what its own
decompile would have had, and decompiles it itself otherwise - a
compile thread not done included, never waited for. Nothing is allocated
in play: four register copies and 64 slots are made at the renderer's
start; a read that finds nothing doubles the command buffers passed over
before the next, up to 63. `test/shader_ahead` checks the read against
the command processor's packets, the registers compared, and the
hand-over, four producers racing a consumer (TSan in CI).

Vertex shaders are decompiled ahead the same way, each with its fetch
shader parsed on the compile thread from the read's registers into an
object of its own, never registered (`LatteFetchShader_Parse`). The draw
takes one where the registers its decompile reads
(`LattePM4_VertexShaderInputs`), its hashes and the draw's own fetch
shader - which the GPU thread may have parsed for other registers before
- match (`LatteFetchShader::Fingerprint`, everything parsed, where the key
leaves out offsets past their low bits); the draw's fetch shader then
becomes the shader's compatible one. `test/fetch_shader` parses into one
object again and again on four threads, each the same as made fresh.

A shader decompiled ahead has its module made there too, from the SPIR-V
the decompiler wrote (`LatteShader_MakeRendererShaderAhead`; the shader
compile threads build it), and its parts for every template seen so far:
the compile thread marks its slot, and the GPU thread, before each draw's
shaders are looked up, queues its parts as a shader's first pipeline
queues them (`LatteShaderAhead_Poll`, `PipelineCompiler::
PrepareAheadPartsFor` - templates are the GPU thread's to take). A draw
that takes the shader keeps its module and the parts made or being made,
so a first pipeline whose render pass and state a template has links what
is there. A shader a graphic pack replaces, or a filtered one, gets no
module ahead: its draw makes it as before. One the draw cannot use is put
back rather than destroyed on the GPU thread, for a read to free on a
compile thread. A part's robust buffer access is its own shaders' alone
(`VK_EXT_pipeline_robustness` chained to that part, out of the keys
without it), so a part made ahead from one stage keys as its pipelines'
whatever the other stage asks for.

A geometry shader's draw has its three shaders decompiled ahead too: the
export (vertex) shader, the pixel shader, and the geometry shader with
its copy shader, whose key and decompile take the ring the export shader
just decompiled writes (`LattePM4_GeometryShaderInputs`, the copy
shader's hash checked at the draw). Their keys are the draw's own stages'
(`LatteShaderKeys.h`); a shader cache's keyed the old way are still found.
The export and geometry shaders get modules, not parts: their first part
takes both. `test/decompiler` decompiles every random shader again with
noise in every register its digest leaves out, which must change nothing.

The read goes on past the buffer the GPU thread starts, into the buffers
the guest has submitted to the ring since, the registers carried from one
to the next (`TCL::TCLGPUPeekRB`, `LattePM4_WholePackets`): where the GPU
thread is behind - as it is when new shaders come - the read is a whole
queue of buffers ahead rather than a few draws. A buffer a read got
through is not read again as the GPU thread starts it, nor one a read
under way may still get to; one a read stopped short of is
(`LattePM4ReadTo`). `test/shader_ahead` checks the walk of the ring, the
registers carried through buffers and the starts.

The shaders a read finds are decompiled side by side, not one after the
other on the thread reading: each is handed to a compile thread of its
own, with a copy of only the registers its decompile and hashes read
(`LattePM4_CopyShaderInputs`, the same ranges as its digest) and of its
programs, from eight tasks made at the start; the read decompiles one
itself where none is free. A scene's worth of new shaders is then ready
in the time of the longest, not their sum. `test/decompiler` decompiles
every random shader again from a register file holding those registers
alone, which must come out the same.

A GPU thread that is never behind starts each buffer as it is submitted,
and a game that flushes once a frame writes the whole frame before the
GPU thread sees any of it. So the guest's own writes are read too: the
GPU thread, going idle with nothing in the ring, offers a copy of its
registers - what the buffer the guest is writing starts from - and
withdraws it once it has work (`LattePM4IdleSnapshot`); as the main
core's command buffer grows past 512 words, and again each time it has
doubled, the guest's thread takes the copy if it has submitted nothing
since and queues a read of what it has written (`GX2Command_ReadAhead`,
`LatteShaderAhead_OnGuestCommands`). Neither thread waits for the other;
a read of the guest's still under way is never waited for either.
`test/shader_ahead` races the hand-over, every copy taken read whole.
The copy is given back offered after each read, so the buffer is read
again as it doubles (it had been read once, at 512 words, and the rest of
the frame not at all). And a read of the ring that gets through to the end
of what the ring held offers its registers there too - what the buffer the
guest submits next starts from - so a GPU thread that is behind, and never
goes idle, has the frame being written read ahead as well, not only what
was submitted (`TCL::TCLGPUPeekRB`'s end index, `LattePM4IdleSnapshot`:
the newer offer replaces one not taken). Reads of the guest's buffer, read
again from its start, do not move the ring's reads' backoff.
A read stops at a wait only where the command processor would wait there
now: a wait on memory whose test fails on what memory holds (the compare
functions are one, `LattePM4_WaitPasses`, the command processor's and the
read's), a semaphore wait with no count left past the waits the read went
past, a flip or a sync with the asynchronous commands. A fence already
written, a semaphore already signalled, a wait on a register (which the
command processor passes) and a semaphore's signal are read past - before,
every one of them ended the read, and with it the lead past the first
sync of a frame. `test/shader_ahead` checks each, the compare functions
against their definitions.

A draw whose shaders the GPU thread has, but whose pipeline it has never
made - a render pass or state no pipeline had - has its pipeline made by
the read too, on a compile thread of its own: the read finds the shaders
in a table the GPU thread publishes them to as it registers them
(`LatteSHRC_FindRegistered`; insert-only, freed only together after the
reads are done), works the key the draw looks its pipeline up by out from
its registers and the render pass they name (the pipeline cache's way,
`VulkanPipelineStableCache_Attachments`, `VKRObjectRenderPass::
HashForPipeline`), and where neither the GPU thread (`LatteShaderAhead_
NotePipeline`, its own and the cache's) nor a read made it, makes it from
the read's registers. The pipeline - with graphics pipeline libraries its
parts made into the cache of parts and linked optimized, without compiled
whole - is handed to its draw (`PipelineCompiler::HandOverAhead`): the GPU
thread keeps the last 64 handed over (`KeptForDraws`, its alone, filled
from a lock-free queue) and a draw that would make its pipeline takes the
one made for it first (`TakeAhead`), with no link, no compile and no ask
of the driver's cache on the GPU thread, and no relink later. It comes
with the layouts it was made with, which the draw's PipelineInfo takes in
place of the ones it had just set up - defined the same, and a layout must
outlive its pipelines without `VK_KHR_maintenance4` - and its render pass
was compatible with the draw's. A read's lead is short, so the pipeline
often lands after its first draw, which was skipped (or, essential, would
wait) for a compile of its own: a later draw of it takes the one made
ahead then, before it is skipped or waits (`draw_awaitAsyncPipeline`), in
place of the pipeline its own compile is still writing (let go once that
compile lets go of it), and binds what its PipelineInfo then has. One
crowded out unused, or left as the title stops, is destroyed, never bound
(`tools/check_ahead_handover.sh`). Nor is anything made twice where the
draw came first: a slot does not make a pipeline the GPU thread has noted
by the time it starts, even one it was queued with (`LatteAhead_
MakePipeline`) - the draw's own compile runs in the lane before the
slot's, ahead of all it holds, and finishes first - and a decompile task
does not decompile a variant the GPU thread has registered by the time it
starts (the log counts these: "not made (their draw first)").
`test/dynstate` makes such pipelines on a real driver as a read's slot
does - compiled whole and linked optimized, with a render pass of its own
that is compatible but not the same and layouts of its own - and draws
them in the draw's render pass through the layouts they came with: they
draw what the draw's own pipeline draws, and the validation layer reports
nothing (it reports an incompatible render pass). Before, the slot made only the parts
and let the pipeline go, and the draw linked it fast on the GPU thread,
or - on a device without libraries and without cache control - was
skipped until a compile thread made it again. Four at a time,
from pools, in the compile threads' lane of work made ahead - behind the
reads and everything a draw needs now (below). A geometry
shader's draw has its geometry shader found by the key the draw takes
(its program's, and the ring of the vertex shader found).

A draw whose shader the read has just had made ahead - its other shader
registered, or new as well - has its pipeline made ahead too, from the
shaders made ahead: queued behind their decompiles with a copy of the
draw's registers, it holds each one (a draw that comes meanwhile, for the
few microseconds, decompiles its own), checks it was made for these
registers and this fetch shader, sets the pipeline up from it and puts it
back, then makes the parts from the modules. A shader made ahead is not
claimed back for another key while a pipeline is made from its module
(`RendererShaderVk::InPipelines`, `AheadSlotStale`), so the module
outlives the compile; one a draw takes the GPU thread keeps for good. So
a new shader in new state - an effect with a blend no pipeline had - is
ready before its first draw too, a new geometry shader as well: decompiled
with its new vertex (export) shader, it is held the same way, found by the
key its draw takes with that shader's ring and checked made for the
draw's copy shader. `test/shader_
ahead` checks the table, one thread adding while three find. What is left: shaders
whose first draw comes before any read reaches it (an idle GPU thread
starts each buffer as it is submitted), and the render passes and states
no pipeline had before where no read got to them.

The reads ahead go to the compile threads in a lane of their own, taken
before any pipeline compile queued (`CompileThreadPool_QueuePreparedUrgent`,
`MpmcQueue_PopPreferring`): when new shaders come, the compile threads are
busiest, and a read queued behind them is late for the draws it reads.
What only a later draw may need - each new shader's parts for every
template (up to 64 builds a shader), the pipelines made ahead, and the
optimized links that replace a fast link already drawing - goes in a lane
of its own after everything a draw needs now (`CompileThreadPool_
QueuePreparedAhead`): before, a draw skipped until its pipeline landed was
skipped for all the ahead work queued before it as well. A pipeline made
ahead hands its parts to that lane too when it makes them together (one
no compile thread has taken it makes itself). `test/prepared_pipeline`
checks the three lanes taken in order, and `tools/check_compile_lanes.sh`
fails where ahead work is queued with what a draw needs now.
The read ahead's own decisions - where a read got to, when its registers
are offered for the guest's buffer, how many buffers the ring's reads pass
over, which pipelines are made ahead - are in `LatteShaderAheadRules.h`,
apart from the reads and the GPU, and `test/shader_ahead` checks each
(`tools/check_ahead_rules.sh`: `LatteShaderAhead.cpp`, which no test links,
decides by every one).
As a title stops, the log has, for vertex, pixel and geometry shaders, how many were
made ahead, taken by their draw, not usable there, drawn while still being
made, drawn with none made, and let go unused (`[ShaderAhead]`): the
numbers to judge this by on real games. Then what the first draw of each
new pipeline met, in any build (`LatteDrawPathCounts.h`): linked or loaded
from what was ready, skipped until a compile thread made it, or made while
it waited - each with how many of those a read had queued ahead - the
draws skipped for a pipeline not made yet, the draws that may not be
skipped and waited for the compile thread making theirs, and the time
waited in all and the longest wait. M2 is done when nothing is made while
a draw waits and no draw is skipped; `tools/check_draw_path_counts.sh`
fails where a way the draw path goes is not counted.

Next: the draws async skips while the parts are made.

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
- Done: no lock, and no read buffer. Every change to the file is made by
  the one writer thread; a reader finds an entry in what that thread
  publishes (records that are never changed in place, a lookup whose
  names are written once) and reads it through the file's mapping
  (libretro-common's, through FileStream), inflating straight from the
  mapped bytes into the caller's buffer: one allocation per entry read,
  the caller's. What a reader may still be reading - a replaced record,
  the bytes of a replaced or deleted entry - stays until the cache goes,
  or the file is next opened. `test/compression` holds a reader between
  finding an entry and reading it while the entry is rewritten or
  deleted and new entries are written, and reads with four threads while
  the writer writes.
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
  entry into one buffer. The entries' programs are inflated into the
  thread's own buffers (`ShaderProgramScratch`), not vectors of their own
  (gated: no allocation per 1000 inflated, 2000 before).
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
