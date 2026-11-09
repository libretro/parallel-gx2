#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineRecord.h"

#include <cstring>

// "GX2PSOR" and the version: a header at the start of each session's
// entries, a tag before each entry
static const uint8 kMagic[7] = {'G', 'X', '2', 'P', 'S', 'O', 'R'};
static const uint8 kVersion = 1;
static const uint8 kTagHeader = 'H';
static const uint8 kTagEntry = 'E';

VulkanPipelineRecorder::VulkanPipelineRecorder() : m_entry(std::make_unique<VulkanPipelineRecordEntry>())
{
	m_writer.writeBE<uint8>(kTagHeader);
	m_writer.writeData(kMagic, sizeof(kMagic));
	m_writer.writeBE<uint8>(kVersion);
}

bool VulkanPipelineRecorder::Draw(const LatteFetchShader& fetchShader, uint64 vertexShaderHash, uint64 geometryShaderHash,
	uint64 pixelShaderHash, uint64 renderPassHash, const LatteContextRegister& lcr)
{
	const uint64 key = VulkanPipelineKey_Graphics(&fetchShader, vertexShaderHash, geometryShaderHash, pixelShaderHash, renderPassHash, lcr, 0);
	if (!m_seen.insert(key).second)
		return false;
	VulkanPipelineRecordEntry& e = *m_entry;
	e.vertexShaderHash = vertexShaderHash;
	e.geometryShaderHash = geometryShaderHash;
	e.pixelShaderHash = pixelShaderHash;
	e.renderPassHash = renderPassHash;
	e.fetchShaderFragment = fetchShader.getVkPipelineHashFragment();
	e.bufferCount = 0;
	for (auto& group : fetchShader.bufferGroups)
		if (e.bufferCount < std::size(e.buffers))
			e.buffers[e.bufferCount++] = group.attributeBufferIndex;
	Latte::StoreGPURegisterState(lcr, e.registers);

	// straight into what the caller writes out
	MemStreamWriter& w = m_writer;
	w.writeBE<uint8>(kTagEntry);
	w.writeBE<uint64>(e.vertexShaderHash);
	w.writeBE<uint64>(e.geometryShaderHash);
	w.writeBE<uint64>(e.pixelShaderHash);
	w.writeBE<uint64>(e.renderPassHash);
	w.writeBE<uint64>(e.fetchShaderFragment);
	w.writeBE<uint8>(e.bufferCount);
	w.writeData(e.buffers, e.bufferCount);
	Latte::SerializeRegisterState(e.registers, w);
	return true;
}

bool VulkanPipelineRecord_Read(std::span<const uint8> data, const std::function<void(const VulkanPipelineRecordEntry&)>& onEntry)
{
	MemStreamReader r(data.data(), (sint32)data.size());
	auto e = std::make_unique<VulkanPipelineRecordEntry>();
	bool header = false;
	while (!r.isEndOfStream())
	{
		const uint8 tag = r.readBE<uint8>();
		if (tag == kTagHeader)
		{
			uint8 magic[sizeof(kMagic)];
			if (!r.readData(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(magic)) != 0 || r.readBE<uint8>() != kVersion)
				return false;
			header = true;
			continue;
		}
		if (tag != kTagEntry || !header)
			return false;
		e->vertexShaderHash = r.readBE<uint64>();
		e->geometryShaderHash = r.readBE<uint64>();
		e->pixelShaderHash = r.readBE<uint64>();
		e->renderPassHash = r.readBE<uint64>();
		e->fetchShaderFragment = r.readBE<uint64>();
		e->bufferCount = r.readBE<uint8>();
		if (r.hasError() || e->bufferCount > std::size(e->buffers) || !r.readData(e->buffers, e->bufferCount))
			return false;
		if (!Latte::DeserializeRegisterState(e->registers, r) || r.hasError())
			return false;
		onEntry(*e);
	}
	// a short read stops at the entry it cut
	return true;
}

uint64 VulkanPipelineRecord_Key(const VulkanPipelineRecordEntry& entry, uint32 dynamic)
{
	// the fetch shader as the key reads it: its buffers and attribute
	// offsets, in the thread's one (no allocation per entry once grown)
	thread_local LatteFetchShader fetchShader;
	fetchShader.vkPipelineHashFragment = entry.fetchShaderFragment;
	fetchShader.bufferGroups.resize(entry.bufferCount);
	for (uint32 i = 0; i < entry.bufferCount; i++)
		fetchShader.bufferGroups[i].attributeBufferIndex = entry.buffers[i];
	const LatteContextRegister& lcr = Latte::LoadGPURegisterStateScratch(entry.registers);
	return VulkanPipelineKey_Graphics(&fetchShader, entry.vertexShaderHash, entry.geometryShaderHash, entry.pixelShaderHash,
		entry.renderPassHash, lcr, dynamic);
}
