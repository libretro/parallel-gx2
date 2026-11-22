#!/bin/sh
# Fails if a pipeline a read made ahead of its draw no longer reaches it
# whole: the read's slot hands it over (PipelineCompiler::HandOverAhead),
# the draw takes it before it would link or compile one itself (TakeAhead,
# first in draw_createGraphicsPipeline), a draw skipped or waiting for its
# own compile takes one landed since before it skips or waits (TakeAhead,
# first in draw_awaitAsyncPipeline) and the draw binds the pipeline its
# PipelineInfo then has, not the one it read before, and those no draw took are given
# up as the title stops and as the renderer goes (DropAhead). Without the
# hand-over the draw linked or compiled again what the read had made, on
# the GPU thread; without the drop the pipelines would outlive the device.
cd "$(dirname "$0")/.." || exit 2
core=src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRendererCore.cpp
wrong=$(awk '
	/^PipelineInfo\* VulkanRenderer::draw_createGraphicsPipeline\(/ { inside = 1 }
	inside && /TakeAhead\(/ && !compiled { taken = 1 }
	inside && /->Compile\(/ { compiled = 1 }
	inside && /^}/ { inside = 0 }
	/^bool VulkanRenderer::draw_awaitAsyncPipeline\(/ { await = 1 }
	await && /TakeAhead\(/ && !waited { lateTaken = 1 }
	await && (/NoteSkippedDraw\(/ || /prepared\.Take\(/) { waited = 1 }
	await && /^}/ { await = 0 }
	# each caller of the wait reads the pipeline again after it
	/!draw_awaitAsyncPipeline\(/ { calls++; pending = FNR }
	pending && FNR > pending && FNR <= pending + 10 && /vkObjPipeline = pipeline_info->m_vkrObjPipeline;/ { reread++; pending = 0 }
	END {
		if (!taken) print "draw_createGraphicsPipeline: no pipeline made ahead taken before its compile"
		if (!lateTaken) print "draw_awaitAsyncPipeline: no pipeline made ahead taken before the draw is skipped or waits"
		if (!calls || reread != calls) print "a caller of draw_awaitAsyncPipeline binds the pipeline it read before the wait"
	}' "$core")
grep -q 'PipelineCompiler::HandOverAhead(' src/Cafe/HW/Latte/Core/LatteShaderAhead.cpp ||
	wrong="$wrong
LatteShaderAhead.cpp: the pipelines made ahead not handed over"
for f in src/Cafe/HW/Latte/Core/LatteShaderAhead.cpp src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp; do
	grep -q 'PipelineCompiler::DropAhead()' "$f" || wrong="$wrong
$f: the pipelines made ahead no draw took not given up"
done
if [ -n "$wrong" ]; then
	echo "a pipeline made ahead does not reach its draw whole:"
	printf '%s\n' "$wrong"
	exit 1
fi
echo "a pipeline made ahead reaches its draw whole"
