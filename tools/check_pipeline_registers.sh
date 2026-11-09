#!/bin/sh
# Fails if what builds or keys a pipeline reads the GPU thread's live
# registers (LatteGPUState) instead of the register state it is handed. A
# pipeline loaded from the cache is built on a compile thread from the
# state the cache stored; reading the live registers there baked in the
# depth, stencil and clip state of whatever the GPU thread was drawing -
# and raced it.
cd "$(dirname "$0")/.." || exit 2
found=$(grep -n 'LatteGPUState' \
	src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.cpp \
	src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp \
	| grep -v ':[0-9]*:[[:space:]]*//')
# the pipeline keys: from the registers they are given
keys=$(awk '/^uint64 (VulkanRenderer::draw_calculate(Minimal)?GraphicsPipelineHash|MetalPipelineCache::CalculatePipelineHash)\(/ { inside = 1 }
	inside && /LatteGPUState/ { print FILENAME ":" FNR ": " $0 }
	inside && /^}/ { inside = 0 }' \
	src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRendererCore.cpp \
	src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp)
if [ -n "$found$keys" ]; then
	echo "a pipeline built or keyed from the live registers, not the state it is handed:"
	[ -n "$found" ] && printf '%s\n' "$found"
	[ -n "$keys" ] && printf '%s\n' "$keys"
	exit 1
fi
echo "pipelines are built and keyed from the register state they are handed"
