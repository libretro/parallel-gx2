#pragma once
#include "util/helpers/Serializer.h"

namespace Latte
{
	struct GPUCompactedRegisterState
	{
		static constexpr int const NUM_REGS = 1854;

		// tied to g_gpuRegSerializerMapping_v1
		uint32 rawArray[NUM_REGS];
	};

	// convert GPU register state into compacted representation. Stores almost all registers, excluding ALU consts
	void StoreGPURegisterState(const LatteContextRegister& contextRegister, GPUCompactedRegisterState& registerStateOut);
	void LoadGPURegisterState(LatteContextRegister& contextRegisterOut, const GPUCompactedRegisterState& registerState);
	// The register file a cached shader or pipeline is rebuilt with, loaded
	// with registerState: the calling thread's own, allocated and zeroed the
	// first time and reused after (a new one per entry was 256 KiB to
	// allocate and zero for each, thousands of times over at boot).
	// LoadGPURegisterState writes the same registers every time, all of them,
	// so as long as nothing else writes to it - its users only read it - it
	// is what a freshly zeroed one would be. Valid until the thread's next
	// call.
	LatteContextRegister& LoadGPURegisterStateScratch(const GPUCompactedRegisterState& registerState);

	void SerializeRegisterState(GPUCompactedRegisterState& regState, MemStreamWriter& memWriter);
	bool DeserializeRegisterState(GPUCompactedRegisterState& regState, MemStreamReader& memReader);
}