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
if [ -n "$wrong" ]; then
	echo "a try at a pipeline that can compile on the draw path:"
	printf '%s\n' "$wrong"
	exit 1
fi
echo "a try at a pipeline never compiles"
