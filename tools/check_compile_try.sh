#!/bin/sh
# Fails if a pipeline compile that was asked only to try - the GPU thread's,
# for a draw that may be skipped - can compile after all. Upstream forces
# every compile where the device has no VK_EXT_pipeline_creation_cache_control
# ("forceCompile = true"), which compiled each new pipeline on the draw path
# on those devices with Async Shader Compile on; a try there takes only what
# is made, or a pipeline known to be in the driver's cache (VulkanPipeline_
# Try), and a merge from upstream must not bring the forcing back.
cd "$(dirname "$0")/.." || exit 2
f=src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.cpp
wrong=$(grep -n 'forceCompile[[:space:]]*=[[:space:]]*true' "$f" | grep -v ':[0-9]*:[[:space:]]*//')
if ! awk '/^bool PipelineCompiler::Compile\(/ { inside = 1 }
	inside && /VulkanPipeline_Try\(/ { found = 1 }
	inside && /^}/ { inside = 0 }
	END { exit !found }' "$f"; then
	wrong="$wrong
$f: PipelineCompiler::Compile does not decide by VulkanPipeline_Try"
fi
# a pipeline taken for one in the driver's cache only where it was made
# through it: noted where either compile succeeds (whole, and linked from
# parts), the pipeline cache's boot load giving its compiler the key, and
# asked by the draw - not the pipeline cache's recipes, recorded before a
# compile that may never finish
[ "$(grep -c 'NoteMade(m_pipelineHash);' "$f")" = 2 ] || wrong="$wrong
$f: a pipeline made not noted where both compiles succeed (NoteMade)"
grep -q 'pipelineCompiler.m_pipelineHash = pipelineStateHash;' src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.cpp || wrong="$wrong
VulkanPipelineStableCache.cpp: the boot load's pipelines not noted as made"
grep -q 'm_driverCached = PipelineCompiler::MadeBefore(' src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRendererCore.cpp || wrong="$wrong
VulkanRendererCore.cpp: the try not told whether its pipeline was made through the driver's cache"
if [ -n "$wrong" ]; then
	echo "a try at a pipeline that can compile on the draw path:"
	printf '%s\n' "$wrong"
	exit 1
fi
echo "a try at a pipeline never compiles"
