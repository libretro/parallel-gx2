#pragma once

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"

#include <functional>
#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

// A title's draw states, recorded for the shader/PSO gate (test/pso,
// docs/SHADER-STUTTER.md): each state a draw used that makes a pipeline key
// of its own with nothing left to the draw - the finest key, so the
// pipelines any set of dynamic state makes can be counted from it - with
// what that key reads: the shaders, the render pass, the fetch shader's
// buffers and attribute offsets, and the register state as the pipeline
// cache keeps it. Recorded by PSO_TRACE builds with PSO_RECORD naming the
// file (VulkanRenderer::draw_record), replayed with pso_gate --replay.
// The core's own file, its own version; appending sessions to one file is
// fine, the replay counts each state once.
struct VulkanPipelineRecordEntry
{
	uint64 vertexShaderHash;
	uint64 geometryShaderHash;
	uint64 pixelShaderHash;
	uint64 renderPassHash;
	uint64 fetchShaderFragment; // LatteFetchShader::vkPipelineHashFragment
	uint8 bufferCount;
	uint8 buffers[Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS]; // the fetch shader's, in its order
	Latte::GPUCompactedRegisterState registers;
};

// Keeps the draw states not seen before and serializes them
class VulkanPipelineRecorder
{
public:
	VulkanPipelineRecorder();
	// The draw's state serialized into Output() if no draw before it made
	// the same finest key; true if it was new
	bool Draw(const LatteFetchShader& fetchShader, uint64 vertexShaderHash, uint64 geometryShaderHash, uint64 pixelShaderHash,
		uint64 renderPassHash, const LatteContextRegister& lcr);
	// What is not yet written out, the header first: borrowed, valid until
	// the next Draw or Taken
	std::span<const uint8> Output() { return m_writer.getResult(); }
	// The caller has written Output() out: the buffer is emptied, keeping
	// its capacity
	void Taken() { m_writer.reset(); }
	size_t States() const { return m_seen.size(); }

private:
	std::unordered_set<uint64> m_seen;
	MemStreamWriter m_writer{4096};
	std::unique_ptr<VulkanPipelineRecordEntry> m_entry;
};

// Each entry of a recording, in order; false if it is not one or is cut
// short (the entries before are still given)
bool VulkanPipelineRecord_Read(std::span<const uint8> data, const std::function<void(const VulkanPipelineRecordEntry&)>& onEntry);
// The pipeline key of a recorded state with dynamic state flags
// (VULKAN_PIPELINE_KEY_DYNAMIC_*), as the renderer computes it
uint64 VulkanPipelineRecord_Key(const VulkanPipelineRecordEntry& entry, uint32 dynamic);
