#include "Cafe/HW/Latte/Core/LatteShaderAhead.h"
#include "Cafe/HW/Latte/Core/LatteDrawPathCounts.h"
#include "Cafe/HW/Latte/Core/LatteShaderAheadRules.h"
#include "Cafe/HW/Latte/Core/LattePM4Scan.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteShaderKeys.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "util/helpers/AheadSlots.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/PreparedPipeline.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#endif

void _calcShaderHashGeneric(uint32* programCode, uint32 programSize, uint64& outputHash1, uint64& outputHash2);

namespace
{
	struct AheadShader
	{
		uint64 digest; // the registers its decompile read (LattePM4Scan.h)
		uint64 auxHash;
		// a vertex shader's: its fetch shader's Fingerprint; a geometry
		// shader's: its copy shader's hash, which its key leaves out
		uint64 fetchShader;
		uint32 scan; // the read it was made in, to let one never taken go
		// its module made (Vulkan), its parts not yet queued by the GPU thread
		bool partsPending;
		bool robust; // what the shader itself asks of its parts
		LatteDecompilerOutput_t output;
	};
	// one key a slot (AheadSlots.h); a shader whose slot is taken is left to
	// its draw
	using Slots = AheadSlots<AheadShader, 64>;
	// the shaders the GPU thread has: none decompiled ahead again
	using Seen = SeenKeys<1u << 15>;
	Slots s_pixel, s_vertex, s_geometry;
	Seen s_seenPixel, s_seenVertex, s_seenGeometry;
	// what happened to them, logged as the title stops: made ahead, taken by
	// their draw, not usable there, the draw first while one was being made,
	// none there for the draw, let go unused
	struct Counts
	{
		const char* kind;
		std::atomic<uint64> made{0}, taken{0}, unusable{0}, late{0}, none{0}, letGo{0};
		// claimed, and not made: its draw decompiled it first
		std::atomic<uint64> drawnFirst{0};
	};
	Counts s_countsVertex{"vertex"}, s_countsPixel{"pixel"}, s_countsGeometry{"geometry"};
	LatteDrawPathCounts s_drawPath;

	// the slots whose shader waits for the GPU thread to queue its parts
	// (LatteShaderAhead_Poll), a bit each
	std::atomic<uint64> s_partsPixel{0}, s_partsVertex{0};

	constexpr uint32 kJobs = 4;
	constexpr uint32 kMaxProgram = 128 * 1024;
	constexpr uint32 kMaxFetchProgram = 16 * 1024;
	constexpr uint32 kMaxPerScan = 8;
	// the ring's words past the buffer read first, copied with it: the whole
	// ring (TCL.cpp)
	constexpr uint32 kRingWords = 4096;
	// a shader made ahead and not taken by this many reads later goes
	constexpr uint32 kStaleScans = 64;

	// the registers copied from the GPU's for a read: what a shader's
	// decompile and its hashes read is in these (LattePM4_PixelShaderInputs,
	// LattePM4_VertexShaderInputs)
	constexpr struct { uint32 start, count; } kCopied[] = {
		{LATTE_REG_BASE_CONFIG, 0x1000},
		{LATTE_REG_BASE_CONTEXT, 0x1000},
		{LATTE_REG_BASE_RESOURCE, 0x1000},
	};

	struct Job
	{
		uint32 regs[LATTE_MAX_REGISTER];
		// copies of the programs, each the one hashed and decompiled: guest
		// memory may change under the read
		uint8 psProgram[kMaxProgram];
		uint8 vsProgram[kMaxProgram];
		uint8 fsProgram[kMaxFetchProgram];
		uint8 gsProgram[kMaxProgram];
		uint8 copyProgram[kMaxProgram]; // the geometry shader's copy shader
		// the vertex shader's fetch shader, parsed here, never registered
		LatteFetchShader fetchShader;
		LatteShaderPSInputTable inputs;
		uint32 physAddr;
		uint32 words;
		// the ring's words after it, whole packets (calls: the buffers they
		// call), and the buffer's number among those the ring calls
		uint32be ringWords[kRingWords];
		uint32 ringCount;
		uint32 ringCalls;
		// the ring's write index those end at, where they are all it held
		// (TCL::kRingNoEnd otherwise, and for a read of the guest's buffer)
		uint32 ringEnd;
		uint32 seq;
		uint32 scan;
		uint32 found;
#ifdef ENABLE_VULKAN
		alignas(PreparedPipeline) unsigned char prepared[sizeof(PreparedPipeline)];
		bool queued;
		PreparedPipeline* Prepared() { return reinterpret_cast<PreparedPipeline*>(prepared); }
#endif
	};
	Job* s_jobs;

	struct Idle
	{
		LattePM4IdleSnapshot snapshot;
		uint32 regs[LATTE_MAX_REGISTER]; // the snapshot's
		Job job; // the guest's thread's alone
	};
	// reads started (GPU thread), and how many command buffers to pass over
	// before the next: doubled, up to 63, for every read that found nothing
	std::atomic<uint32> s_scans{0};
	std::atomic<uint32> s_backoff{0};
	uint32 s_skip;
	// the ring's buffers a read starts at, and the job of the last started
	LattePM4ReadTo s_readTo;
	struct Job* s_plannedJob;
	// the idle GPU thread's registers, offered to the guest's thread, and
	// that thread's own read of the buffer it is writing (Job, below)
	struct Idle;
	std::atomic<Idle*> s_idle{nullptr};
	std::atomic<uint64> s_guestReads{0};
	std::atomic<uint64> s_ringEndOffers{0};

	const uint32be* MapGuest(void*, uint32 physAddr, uint32 count)
	{
		if (count == 0 || count > (1u << 22) || !memory_isAddressRangeAccessible(physAddr, count * 4))
			return nullptr;
		return reinterpret_cast<const uint32be*>(memory_getPointerFromPhysicalOffset(physAddr));
	}

	// size bytes of guest memory at a physical address, copied; false where
	// they cannot be read or do not fit
	bool CopyGuest(uint8* to, uint32 addr, uint32 size, uint32 room)
	{
		if (size > room || (size && (addr == 0 || !memory_isAddressRangeAccessible(addr, size))))
			return false;
		if (size)
			memcpy(to, memory_getPointerFromPhysicalOffset(addr), size);
		return true;
	}

	uint64 CopyShaderHash(const uint8* program, uint32 size)
	{
		uint64 h1, h2;
		_calcShaderHashGeneric(reinterpret_cast<uint32*>(const_cast<uint8*>(program)), size, h1, h2);
		return h1 ^ (h2 * 0x9E3779B97F4A7C15ull);
	}

	// The slot for baseHash, the caller's to fill (FillAhead): none where the
	// GPU thread has the shader, or its slot is in hand or published
	Slots::Held ClaimAhead(Slots& slots, Seen& seen, Counts& counts, uint64 baseHash, uint32 scan)
	{
		if (seen.Contains(baseHash))
			return {};
		bool hadPayload;
		auto held = slots.Claim(baseHash, [&seen, scan](uint64 key, const AheadShader& p) {
			// not while a pipeline is made from its module (PipelineAhead)
			bool inUse = false;
#ifdef ENABLE_VULKAN
			inUse = p.output.shader && p.output.shader->shader && static_cast<RendererShaderVk*>(p.output.shader->shader)->InPipelines();
#endif
			return AheadSlotStale(inUse, seen.Contains(key), scan - p.scan, kStaleScans);
		}, &hadPayload);
		if (!held)
			return {};
		if (hadPayload)
		{
			LatteShader_FreeDecompilerOutput(held.payload->output);
			counts.letGo.fetch_add(1, std::memory_order_relaxed);
		}
		held.payload->output = {};
		return held;
	}

	// decompile(output) into the slot claimed for baseHash, published for its
	// draw with finish(it) filling in what the draw checks it by, and its
	// module made, for the GPU thread to queue its parts (parts: the slots'
	// pending bits; none for a shader whose parts take a geometry shader's
	// with it)
	template<typename Decompile, typename Finish>
	bool FillAhead(Slots& slots, Slots::Held held, std::atomic<uint64>* parts, Counts& counts, uint64 baseHash, uint32 scan, Decompile decompile, Finish finish)
	{
		decompile(held.payload->output);
		if (!held.payload->output.shader)
		{
			slots.Abandon(held);
			return false;
		}
		finish(*held.payload);
		held.payload->scan = scan;
		held.payload->robust = false;
		held.payload->partsPending = LatteShader_MakeRendererShaderAhead(held.payload->output, baseHash, held.payload->auxHash) && parts;
#ifdef ENABLE_VULKAN
		if (held.payload->partsPending)
			held.payload->robust = held.payload->output.shader->shaderType == LatteConst::ShaderType::Vertex
				? PipelineCompiler::CalcRobustBufferAccessRequirement(held.payload->output.shader, nullptr, nullptr)
				: PipelineCompiler::CalcRobustBufferAccessRequirement(nullptr, held.payload->output.shader, nullptr);
#endif
		const size_t index = held.index;
		const bool pending = held.payload->partsPending;
		slots.Publish(held);
		counts.made.fetch_add(1, std::memory_order_relaxed);
		if (pending)
			parts->fetch_or(1ull << index, std::memory_order_release);
		return true;
	}

	// What a draw's shaders are decompiled from: the read's own registers and
	// copies, or a task's copy of them (Task)
	struct Sources
	{
		uint32* regs;
		const LatteShaderPSInputTable* inputs;
		uint8* program; // the pixel shader's, or the vertex (export) shader's
		uint32 size;
		LatteFetchShader* fetchShader; // a vertex shader's, parsed
		bool gs; // a geometry shader's draw: its program and copy shader's
		uint8* gsProgram;
		uint32 gsSize;
		uint64 gsHash1, gsHash2;
		uint8* copyProgram;
		uint32 copySize;
		uint32 scan;
	};

	bool MakePixel(const Sources& src, Slots::Held held, uint64 baseHash)
	{
		LatteShaderPSInputTable inputs = *src.inputs;
		return FillAhead(s_pixel, held, &s_partsPixel, s_countsPixel, baseHash, src.scan,
			[&](LatteDecompilerOutput_t& out) {
				LatteShader_DecompilePixelShaderAhead(src.regs, src.program, src.size, src.gs, &inputs, baseHash, &out);
			},
			[&](AheadShader& p) {
				p.auxHash = LatteSHRC_CalcPSAuxHash(p.output.shader, src.regs);
				p.digest = LattePM4_PixelShaderInputs(src.regs);
				p.fetchShader = 0;
			});
	}

	// after the vertex shader made for this draw, whose ring it reads
	void MakeGeometry(const Sources& src, uint32 ring)
	{
		const uint64 baseHash = LatteSHRC_GeometryBaseHash(src.gsHash1, src.gsHash2, ring, src.regs[mmVGT_STRMOUT_EN] != 0);
		Slots::Held held = ClaimAhead(s_geometry, s_seenGeometry, s_countsGeometry, baseHash, src.scan);
		if (!held)
			return;
		LatteShaderPSInputTable inputs = *src.inputs;
		FillAhead(s_geometry, held, nullptr, s_countsGeometry, baseHash, src.scan,
			[&](LatteDecompilerOutput_t& out) {
				LatteShader_DecompileGeometryShaderAhead(src.regs, src.gsProgram, src.gsSize, src.copyProgram, src.copySize, ring,
					&inputs, baseHash, &out);
			},
			[&](AheadShader& p) {
				p.auxHash = 0; // LatteSHRC_CalcGSAuxHash
				p.digest = LattePM4_GeometryShaderInputs(src.regs);
				p.fetchShader = CopyShaderHash(src.copyProgram, src.copySize);
			});
	}

	// the export shader where a geometry shader runs, and then that
	void MakeVertex(const Sources& src, Slots::Held held, uint64 baseHash)
	{
		LatteShaderPSInputTable inputs = *src.inputs;
		uint32 ring = 0;
		const bool made = FillAhead(s_vertex, held, src.gs ? nullptr : &s_partsVertex, s_countsVertex, baseHash, src.scan,
			[&](LatteDecompilerOutput_t& out) {
				LatteShader_DecompileVertexShaderAhead(src.regs, src.program, src.size, src.gs, src.fetchShader, &inputs, baseHash, &out);
			},
			[&](AheadShader& p) {
				p.auxHash = LatteSHRC_CalcVSAuxHash(p.output.shader, src.regs);
				p.digest = LattePM4_VertexShaderInputs(src.regs);
				p.fetchShader = src.fetchShader->Fingerprint();
				ring = p.output.shader->ringParameterCount;
				p.output.shader->compatibleFetchShader = nullptr; // the draw's, once taken
			});
		if (made && src.gs)
			MakeGeometry(src, ring);
	}

	// A shader's decompile handed to a compile thread of its own, so that the
	// new shaders a read finds are decompiled side by side rather than one
	// after the other on the thread reading. It takes a copy of only the
	// registers its decompile and hashes read (LattePM4_CopyShaderInputs;
	// test/decompiler decompiles with nothing else and gets the same); the
	// read's registers move on.
	constexpr uint32 kTasks = 8;
	struct Task
	{
		uint32 regs[LATTE_MAX_REGISTER];
		uint8 program[kMaxProgram];
		uint8 fsProgram[kMaxFetchProgram];
		uint8 gsProgram[kMaxProgram];
		uint8 copyProgram[kMaxProgram];
		LatteFetchShader fetchShader;
		LatteShaderPSInputTable inputs;
		Sources src;
		uint32 fsSize;
		bool vertex;
		uint64 baseHash;
		Slots::Held held;
		// a read's, from taking it to the end of its run (none waits for it)
		std::atomic<bool> busy;
		bool queued;
		bool ran; // false after a retire: never run, its slot given up
#ifdef ENABLE_VULKAN
		alignas(PreparedPipeline) unsigned char prepared[sizeof(PreparedPipeline)];
		PreparedPipeline* Prepared() { return reinterpret_cast<PreparedPipeline*>(prepared); }
#endif
	};
	Task* s_tasks;

	// a task no read holds and none runs; null with all in use
	Task* TakeTask()
	{
#ifdef ENABLE_VULKAN
		for (uint32 i = 0; s_tasks && i < kTasks; i++)
		{
			Task& t = s_tasks[i];
			bool expected = false;
			if (t.busy.load(std::memory_order_relaxed) ||
				!t.busy.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
				continue;
			if (t.queued)
			{
				// run, but its build not marked done yet: left, not waited for
				if (!t.Prepared()->IsBuilt())
				{
					t.busy.store(false, std::memory_order_release);
					continue;
				}
				t.Prepared()->~PreparedPipeline();
				t.queued = false;
			}
			return &t;
		}
#endif
		return nullptr;
	}

	LatteDecompilerShader* Registered(LatteConst::ShaderType type, uint64 base, uint32* regs);

	void RunTask(Task* t)
	{
		t->ran = true;
		// its draw came first and the GPU thread decompiled this variant
		// itself since the read claimed it: nothing to make (a geometry
		// shader's draw goes on, for the geometry shader)
		if (!t->src.gs && Registered(t->vertex ? LatteConst::ShaderType::Vertex : LatteConst::ShaderType::Pixel, t->baseHash, t->regs))
		{
			(t->vertex ? s_vertex : s_pixel).Abandon(t->held);
			(t->vertex ? s_countsVertex : s_countsPixel).drawnFirst.fetch_add(1, std::memory_order_relaxed);
			t->busy.store(false, std::memory_order_release);
			return;
		}
		if (t->vertex)
		{
			t->fetchShader.Reset();
			LatteFetchShader_Parse(&t->fetchShader, LatteFetchShader::CalculateCacheHash(t->fsProgram, t->fsSize), t->regs,
				reinterpret_cast<uint32*>(t->fsProgram), t->fsSize);
			MakeVertex(t->src, t->held, t->baseHash);
		}
		else
			MakePixel(t->src, t->held, t->baseHash);
		t->busy.store(false, std::memory_order_release);
	}

	void QueueTask(Task* t)
	{
#ifdef ENABLE_VULKAN
		t->ran = false;
		PreparedPipeline* prepared = new (t->prepared) PreparedPipeline([](void* ctx, bool) {
			RunTask(static_cast<Task*>(ctx));
			return (VkPipeline)VK_NULL_HANDLE;
		}, t);
		t->queued = true;
		PipelineCompiler::CompileThreadPool_QueuePreparedUrgent(*prepared);
#endif
	}

	// The read's own sources for this draw
	Sources ReadSources(Job* job, uint32* regs, bool vertex, uint32 size, bool gs, uint32 gsSize, uint64 gsHash1, uint64 gsHash2, uint32 copySize)
	{
		Sources src{};
		src.regs = regs;
		src.inputs = &job->inputs;
		src.program = vertex ? job->vsProgram : job->psProgram;
		src.size = size;
		src.fetchShader = &job->fetchShader;
		src.gs = gs;
		src.gsProgram = job->gsProgram;
		src.gsSize = gsSize;
		src.gsHash1 = gsHash1;
		src.gsHash2 = gsHash2;
		src.copyProgram = job->copyProgram;
		src.copySize = copySize;
		src.scan = job->scan;
		return src;
	}

	// The claimed shader made: by a task where one is free, here otherwise
	void Make(Job* job, const Sources& read, bool vertex, Slots::Held held, uint64 baseHash, uint32 fsSize)
	{
		Task* t = TakeTask();
		if (!t)
		{
			if (vertex)
				MakeVertex(read, held, baseHash);
			else
				MakePixel(read, held, baseHash);
			return;
		}
		LattePM4_CopyShaderInputs(vertex ? LattePM4Inputs::Vertex : LattePM4Inputs::Pixel, t->regs, read.regs);
		if (vertex && read.gs)
			LattePM4_CopyShaderInputs(LattePM4Inputs::Geometry, t->regs, read.regs);
		memcpy(t->program, read.program, read.size);
		t->inputs = *read.inputs;
		t->src = read;
		t->src.regs = t->regs;
		t->src.inputs = &t->inputs;
		t->src.program = t->program;
		t->src.fetchShader = &t->fetchShader;
		t->vertex = vertex;
		if (vertex)
		{
			memcpy(t->fsProgram, job->fsProgram, fsSize);
			t->fsSize = fsSize;
			if (read.gs)
			{
				memcpy(t->gsProgram, read.gsProgram, read.gsSize);
				memcpy(t->copyProgram, read.copyProgram, read.copySize);
				t->src.gsProgram = t->gsProgram;
				t->src.copyProgram = t->copyProgram;
			}
		}
		t->baseHash = baseHash;
		t->held = held;
		QueueTask(t);
	}

	// (gs: a geometry shader's draw, gsHash1 its program's first hash)
	// the shader's base hash, 0 where not read
	uint64 PixelAhead(Job* job, uint32* regs, bool gs, uint64 gsHash1, bool& made)
	{
		const uint32 size = regs[mmSQ_PGM_START_PS + 1] << 3;
		if (size == 0 || !CopyGuest(job->psProgram, regs[mmSQ_PGM_START_PS] << 8, size, kMaxProgram))
			return 0;
		uint64 h1, h2;
		_calcShaderHashGeneric(reinterpret_cast<uint32*>(job->psProgram), size, h1, h2);
		const uint64 baseHash = LatteSHRC_PixelBaseHash(h1, h2, job->inputs.key, gs, gsHash1);
		Slots::Held held = ClaimAhead(s_pixel, s_seenPixel, s_countsPixel, baseHash, job->scan);
		if (!held)
			return baseHash;
		Make(job, ReadSources(job, regs, false, size, gs, 0, gsHash1, 0, 0), false, held, baseHash, 0);
		job->found++;
		made = true;
		return baseHash;
	}

	// the export shader where a geometry shader runs (gs), with that after
	// it; its base hash, 0 where not read (the fetch shader then parsed)
	uint64 VertexAhead(Job* job, uint32* regs, bool gs, uint32 gsSize, uint64 gsHash1, uint64 gsHash2, uint32 copySize, bool& made)
	{
		const uint32 program = gs ? mmSQ_PGM_START_ES : mmSQ_PGM_START_VS;
		const uint32 size = regs[program + 1] << 3;
		const uint32 fsSize = regs[mmSQ_PGM_START_FS + 1] << 3;
		if (size == 0 || !CopyGuest(job->vsProgram, regs[program] << 8, size, kMaxProgram) ||
			!CopyGuest(job->fsProgram, regs[mmSQ_PGM_START_FS] << 8, fsSize, kMaxFetchProgram))
			return 0;
		// the fetch shader parsed as LatteFetchShader::FindByGPUState would parse it
		// for these registers; the draw's own, which may have been parsed for
		// others, has to match it (its Fingerprint)
		job->fetchShader.Reset();
		LatteFetchShader_Parse(&job->fetchShader, LatteFetchShader::CalculateCacheHash(job->fsProgram, fsSize), regs,
			reinterpret_cast<uint32*>(job->fsProgram), fsSize);
		uint64 h1, h2;
		_calcShaderHashGeneric(reinterpret_cast<uint32*>(job->vsProgram), size, h1, h2);
		const uint64 baseHash = LatteSHRC_VertexBaseHash(h1, h2, &job->fetchShader, job->inputs.key, gs, regs);
		Slots::Held held = ClaimAhead(s_vertex, s_seenVertex, s_countsVertex, baseHash, job->scan);
		if (!held)
			return baseHash;
		Make(job, ReadSources(job, regs, true, size, gs, gsSize, gsHash1, gsHash2, copySize), true, held, baseHash, fsSize);
		job->found++;
		made = true;
		return baseHash;
	}

#ifdef ENABLE_VULKAN
	// A pipeline made ahead of its draw: from a copy of the read's registers
	// at it, on a compile thread of its own, its render pass the one the
	// registers name (as the pipeline cache's are made,
	// VulkanPipelineStableCache_Attachments)
	constexpr uint32 kPipelineSlots = 4;
	struct PipelineSlot
	{
		uint32 regs[LATTE_MAX_REGISTER]; // what a pipeline's state reads is in kCopied
		uint8 fsProgram[kMaxFetchProgram];
		uint32 fsSize;
		LatteFetchShader fetchShader; // the draw's, parsed here
		// the draw's shaders: a base hash each, and whether it is one made
		// ahead rather than registered
		uint64 vsBase, psBase, gsHash1, gsHash2;
		bool vsAhead, psAhead, gs;
		uint64 hash; // the pipeline's, where known when queued
		uint8 copyProgram[kMaxProgram]; // a geometry shader's draw's copy shader
		uint32 copySize;
		VKRObjectRenderPass* renderPass;
		PipelineInfo* info;
		PipelineCompiler* compiler;
		std::atomic<bool> busy;
		bool queued;
		bool ran;
		alignas(PreparedPipeline) unsigned char prepared[sizeof(PreparedPipeline)];
		PreparedPipeline* Prepared() { return reinterpret_cast<PreparedPipeline*>(prepared); }
	};
	PipelineSlot* s_pipelineSlots;
	// the pipelines the GPU thread has (made, or the cache's loaded), and
	// those made ahead: none made ahead again
	SeenKeys<1u << 16> s_pipelinesKnown, s_pipelinesAhead;
	std::atomic<uint64> s_pipelinesMade{0};

	PipelineSlot* TakePipelineSlot()
	{
		for (uint32 i = 0; s_pipelineSlots && i < kPipelineSlots; i++)
		{
			PipelineSlot& slot = s_pipelineSlots[i];
			bool expected = false;
			if (slot.busy.load(std::memory_order_relaxed) ||
				!slot.busy.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
				continue;
			if (slot.queued)
			{
				if (!slot.Prepared()->IsBuilt())
				{
					slot.busy.store(false, std::memory_order_release);
					continue;
				}
				slot.Prepared()->~PreparedPipeline();
				slot.queued = false;
			}
			return &slot;
		}
		return nullptr;
	}

	// what the slot made goes, the pipeline handed over with its layouts:
	// the compiler, the pipeline's info, the render pass (its draw's is
	// compatible)
	void LetPipelineGo(PipelineSlot& slot)
	{
		delete slot.compiler;
		slot.compiler = nullptr;
		delete slot.info;
		slot.info = nullptr;
		if (slot.renderPass)
			VulkanRenderer::GetInstance()->ReleaseDestructibleObject(slot.renderPass);
		slot.renderPass = nullptr;
	}

	// a registered shader for the draw: its aux hash worked out from the
	// registers with any of its variants
	LatteDecompilerShader* Registered(LatteConst::ShaderType type, uint64 base, uint32* regs)
	{
		LatteDecompilerShader* any = LatteSHRC_FindRegisteredBase(type, base);
		if (!any)
			return nullptr;
		const uint64 aux = type == LatteConst::ShaderType::Vertex ? LatteSHRC_CalcVSAuxHash(any, regs) : LatteSHRC_CalcPSAuxHash(any, regs);
		return LatteSHRC_FindRegistered(type, base, aux);
	}

	// a shader made ahead for the draw, held (no draw takes it meanwhile)
	// while its pipeline is set up from it: one made for these registers,
	// and a vertex shader for this fetch shader; null where none is, or it
	// is being made still or was taken
	LatteDecompilerShader* HoldAhead(Slots& slots, Slots::Held& held, uint64 base, uint64 digest, uint64 fetchShader, uint64& aux)
	{
		held = slots.Take(base);
		if (!held)
			return nullptr;
		const AheadShader& p = *held.payload;
		if (!p.output.shader || p.digest != digest || p.fetchShader != fetchShader || !p.output.shader->shader)
		{
			slots.Publish(held);
			held = {};
			return nullptr;
		}
		aux = p.auxHash;
		return p.output.shader;
	}

	void RunPipelineSlot(PipelineSlot* slot)
	{
		slot->ran = true;
		uint32* regs = slot->regs;
		slot->fetchShader.Reset();
		LatteFetchShader_Parse(&slot->fetchShader, LatteFetchShader::CalculateCacheHash(slot->fsProgram, slot->fsSize), regs,
			reinterpret_cast<uint32*>(slot->fsProgram), slot->fsSize);
		// the shaders: those made ahead held until the pipeline is set up
		// from them, a draw that comes meanwhile decompiling its own; their
		// modules stay while it is made (RendererShaderVk::InPipelines)
		Slots::Held vsHeld{}, psHeld{};
		uint64 vsAux = 0, psAux = 0;
		LatteDecompilerShader* vs = nullptr;
		LatteDecompilerShader* ps = nullptr;
		if (slot->vsAhead)
			vs = HoldAhead(s_vertex, vsHeld, slot->vsBase, LattePM4_VertexShaderInputs(regs), slot->fetchShader.Fingerprint(), vsAux);
		if (!vs)
			vs = Registered(LatteConst::ShaderType::Vertex, slot->vsBase, regs); // the draw's own, taken by then
		if (slot->psAhead)
			ps = HoldAhead(s_pixel, psHeld, slot->psBase, LattePM4_PixelShaderInputs(regs), 0, psAux);
		if (!ps)
			ps = Registered(LatteConst::ShaderType::Pixel, slot->psBase, regs);
		if (vs && !vsHeld)
			vsAux = vs->auxHash;
		if (ps && !psHeld)
			psAux = ps->auxHash;
		LatteDecompilerShader* geometry = nullptr;
		Slots::Held gsHeld{};
		bool ok = vs && ps && !vs->hasError && !ps->hasError && vs->shader && ps->shader;
		if (ok && slot->gs)
		{
			// by the key its draw takes, with the ring of the vertex shader
			// found: registered, or made ahead with it for this copy shader
			const uint64 gsBase = LatteSHRC_GeometryBaseHash(slot->gsHash1, slot->gsHash2, vs->ringParameterCount, regs[mmVGT_STRMOUT_EN] != 0);
			geometry = LatteSHRC_FindRegistered(LatteConst::ShaderType::Geometry, gsBase, 0);
			uint64 gsAux = 0;
			if (!geometry && slot->vsAhead)
				geometry = HoldAhead(s_geometry, gsHeld, gsBase, LattePM4_GeometryShaderInputs(regs),
					CopyShaderHash(slot->copyProgram, slot->copySize), gsAux);
			ok = geometry && !geometry->hasError && geometry->shader;
		}
		const LatteContextRegister& lcr = *reinterpret_cast<const LatteContextRegister*>(regs);
		VKRObjectRenderPass::AttachmentInfo_t attachments;
		if (ok)
		{
			VulkanPipelineStableCache_Attachments(ps, lcr, attachments);
			// the key the draw looks its pipeline up by
			// (VulkanRenderer::draw_calculateGraphicsPipelineHash)
			const uint64 hash = VulkanPipelineKey_Graphics(&slot->fetchShader, slot->vsBase, geometry ? geometry->baseHash : 0, slot->psBase + psAux,
				VKRObjectRenderPass::HashForPipeline(attachments), lcr, VulkanRenderer::draw_pipelineKeyDynamicFlags());
			if (!LatteAhead_MakePipeline(slot->hash, hash, s_pipelinesKnown.Contains(hash), s_pipelinesAhead.Contains(hash)))
				ok = false;
			else
			{
				s_pipelinesAhead.Insert(hash);
				slot->hash = hash;
			}
		}
		if (ok)
		{
			slot->renderPass = new VKRObjectRenderPass(attachments);
			slot->info = new PipelineInfo(0, slot->hash, &slot->fetchShader, vs, ps, geometry);
			slot->compiler = new PipelineCompiler();
			ok = slot->compiler->InitFromCurrentGPUState(slot->info, lcr, slot->renderPass, PipelineCompiler::CalcRobustBufferAccessRequirement(vs, ps, geometry));
		}
		// set up: what was held, for its draw again
		if (gsHeld)
			s_geometry.Publish(gsHeld);
		if (vsHeld)
			s_vertex.Publish(vsHeld);
		if (psHeld)
			s_pixel.Publish(psHeld);
		if (ok)
		{
			slot->compiler->m_madeAhead = true;
			slot->compiler->Compile(true, false, false);
			s_pipelinesMade.fetch_add(1, std::memory_order_relaxed);
			// the pipeline, with its layouts, to its draw, which takes it
			// whole; the rest goes. The compiler's hold on it let go first
			delete slot->compiler;
			slot->compiler = nullptr;
			VKRObjectPipeline* made = slot->info->m_vkrObjPipeline;
			if (made->GetPipeline() != VK_NULL_HANDLE)
			{
				slot->info->m_vkrObjPipeline = nullptr;
				PipelineCompiler::HandOverAhead(slot->hash, made);
			}
		}
		LetPipelineGo(*slot);
		slot->busy.store(false, std::memory_order_release);
	}

	// A draw whose pipeline the GPU thread has not made has it made ahead:
	// one whose shaders it has, with a render pass or state no pipeline had,
	// and one whose shader this read just had made ahead (vsNew, psNew), its
	// other shader registered or new too - from the shaders made ahead, on
	// a compile thread queued behind their decompiles. With graphics
	// pipeline libraries its parts, which the draw's own pipeline then
	// links; otherwise the pipeline, for the driver's cache to give the
	// draw's. A geometry shader's draw has its geometry shader found by the
	// key its draw takes: its program's and the ring of the vertex shader
	// found - registered, or made ahead with a new vertex shader.
	void PipelineAhead(Job* job, uint32* regs, uint64 vsBase, bool vsNew, uint64 psBase, bool psNew, bool gs, uint64 gsHash1, uint64 gsHash2)
	{
		if (!vsBase || !psBase)
			return;
		uint64 hash = 0;
		if (!vsNew && !psNew)
		{
			// worked out here, to queue none the GPU thread has
			LatteDecompilerShader* vs = Registered(LatteConst::ShaderType::Vertex, vsBase, regs);
			LatteDecompilerShader* ps = Registered(LatteConst::ShaderType::Pixel, psBase, regs);
			if (!vs || !ps || vs->hasError || ps->hasError || !vs->shader || !ps->shader)
				return;
			LatteDecompilerShader* geometry = nullptr;
			if (gs)
			{
				geometry = LatteSHRC_FindRegistered(LatteConst::ShaderType::Geometry,
					LatteSHRC_GeometryBaseHash(gsHash1, gsHash2, vs->ringParameterCount, regs[mmVGT_STRMOUT_EN] != 0), 0);
				if (!geometry)
					return;
			}
			const LatteContextRegister& lcr = *reinterpret_cast<const LatteContextRegister*>(regs);
			VKRObjectRenderPass::AttachmentInfo_t attachments;
			VulkanPipelineStableCache_Attachments(ps, lcr, attachments);
			hash = VulkanPipelineKey_Graphics(&job->fetchShader, vs->baseHash, geometry ? geometry->baseHash : 0, ps->baseHash + ps->auxHash,
				VKRObjectRenderPass::HashForPipeline(attachments), lcr, VulkanRenderer::draw_pipelineKeyDynamicFlags());
			if (!LatteAhead_QueuePipeline(false, false, true, true, s_pipelinesKnown.Contains(hash), s_pipelinesAhead.Contains(hash)))
				return;
		}
		else
		{
			// the other shader there by the time: registered, or new too
			const bool vsThere = vsNew || LatteSHRC_FindRegisteredBase(LatteConst::ShaderType::Vertex, vsBase);
			const bool psThere = psNew || LatteSHRC_FindRegisteredBase(LatteConst::ShaderType::Pixel, psBase);
			if (!LatteAhead_QueuePipeline(vsNew, psNew, vsThere, psThere, false, false))
				return;
		}
		PipelineSlot* slot = TakePipelineSlot();
		if (!slot)
			return;
		if (hash)
			s_pipelinesAhead.Insert(hash);
		for (const auto& r : kCopied)
			memcpy(slot->regs + r.start, regs + r.start, r.count * sizeof(uint32));
		slot->fsSize = regs[mmSQ_PGM_START_FS + 1] << 3;
		memcpy(slot->fsProgram, job->fsProgram, slot->fsSize);
		slot->vsBase = vsBase;
		slot->psBase = psBase;
		slot->vsAhead = vsNew;
		slot->psAhead = psNew;
		slot->gs = gs;
		slot->copySize = gs ? regs[mmSQ_PGM_START_VS + 1] << 3 : 0;
		if (gs)
			memcpy(slot->copyProgram, job->copyProgram, slot->copySize);
		slot->gsHash1 = gsHash1;
		slot->gsHash2 = gsHash2;
		slot->hash = hash;
		slot->ran = false;
		PreparedPipeline* prepared = new (slot->prepared) PreparedPipeline([](void* ctx, bool) {
			RunPipelineSlot(static_cast<PipelineSlot*>(ctx));
			return (VkPipeline)VK_NULL_HANDLE;
		}, slot);
		slot->queued = true;
		// behind the reads and their decompiles, and anything a draw needs
		// now (the compiles of the pipelines whose draws are skipped)
		PipelineCompiler::CompileThreadPool_QueuePreparedAhead(*prepared);
		job->found++;
	}
#endif

	bool OnDraw(void* ctx, uint32* regs)
	{
		Job* job = static_cast<Job*>(ctx);
		LatteShader_CreatePSInputTable(&job->inputs, regs);
		// a geometry shader's draw (LatteSHRC_UpdateActiveShaders): its
		// program hashed first, as its pixel shader's key takes it; it and
		// its copy shader read, or the draw left to the GPU thread
		const bool gs = (regs[Latte::REGADDR::VGT_GS_MODE] & 3) != 0;
		const uint32 gsSize = gs ? regs[mmSQ_PGM_START_GS + 1] << 3 : 0;
		const uint32 copySize = gs ? regs[mmSQ_PGM_START_VS + 1] << 3 : 0;
		uint64 gsHash1 = 0, gsHash2 = 0;
		if (gs)
		{
			if (gsSize == 0 || copySize == 0 || !CopyGuest(job->gsProgram, regs[mmSQ_PGM_START_GS] << 8, gsSize, kMaxProgram) ||
				!CopyGuest(job->copyProgram, regs[mmSQ_PGM_START_VS] << 8, copySize, kMaxProgram))
				return true;
			_calcShaderHashGeneric(reinterpret_cast<uint32*>(job->gsProgram), gsSize, gsHash1, gsHash2);
		}
		bool psNew = false, vsNew = false;
		const uint64 psBase = PixelAhead(job, regs, gs, gsHash1, psNew);
		const uint64 vsBase = VertexAhead(job, regs, gs, gsSize, gsHash1, gsHash2, copySize, vsNew);
#ifdef ENABLE_VULKAN
		PipelineAhead(job, regs, vsBase, vsNew, psBase, psNew, gs, gsHash1, gsHash2);
#endif
		return job->found < kMaxPerScan;
	}

	// A read through to the end of what the ring held: its registers there
	// are what the buffer the guest submits next starts from, offered to the
	// guest's thread to read that buffer as it writes it (where it has
	// submitted nothing since), as the idle GPU thread offers its own - for
	// a GPU thread that is behind, which never goes idle
	void OfferRingEnd(const uint32* regs, uint32 ringEnd)
	{
		Idle* state = s_idle.load(std::memory_order_acquire);
		if (!state || !LatteAhead_OfferRingEnd(true, ringEnd, TCL::TCLRingWriteIndex(), TCL::kRingNoEnd) || !state->snapshot.BeginFill())
			return;
		for (const auto& r : kCopied)
			memcpy(state->regs + r.start, regs + r.start, r.count * sizeof(uint32));
		state->snapshot.Offer(ringEnd);
		s_ringEndOffers.fetch_add(1, std::memory_order_relaxed);
	}

	void RunJob(Job* job)
	{
		job->found = 0;
		if (const uint32be* buf = MapGuest(nullptr, job->physAddr, job->words))
		{
			LattePM4Scan scan{};
			scan.regs = job->regs;
			scan.map = MapGuest;
			scan.onDraw = OnDraw;
			scan.ctx = job;
			LattePM4_Scan(scan, buf, job->words);
			// on through the buffers submitted since, the registers as this
			// one left them
			const bool bufferDone = scan.end == LattePM4Scan::End::Done;
			if (bufferDone && job->ringCount)
				LattePM4_Scan(scan, job->ringWords, job->ringCount);
			const LatteAheadReadEnd end = LatteAhead_ReadEnd(job->seq, bufferDone, job->ringCount,
				scan.end == LattePM4Scan::End::Done, job->ringCalls, scan.called);
			s_readTo.ReadTo(end.readTo);
			if (end.reachedEnd && job->ringEnd != TCL::kRingNoEnd)
				OfferRingEnd(job->regs, job->ringEnd);
		}
		s_backoff.store(LatteAhead_Backoff(s_backoff.load(std::memory_order_relaxed), job->seq, job->found), std::memory_order_relaxed);
	}
}

void LatteShaderAhead_Init()
{
#ifdef ENABLE_VULKAN
	if (s_jobs || g_renderer->GetType() != RendererAPI::Vulkan)
		return;
	s_jobs = new Job[kJobs];
	for (uint32 i = 0; i < kJobs; i++)
	{
		memset(s_jobs[i].regs, 0, sizeof(s_jobs[i].regs));
		s_jobs[i].queued = false;
	}
	s_pipelineSlots = new PipelineSlot[kPipelineSlots];
	for (uint32 i = 0; i < kPipelineSlots; i++)
	{
		PipelineSlot& slot = s_pipelineSlots[i];
		slot.renderPass = nullptr;
		slot.info = nullptr;
		slot.compiler = nullptr;
		slot.busy.store(false, std::memory_order_relaxed);
		slot.queued = false;
		slot.ran = false;
	}
	s_tasks = new Task[kTasks];
	for (uint32 i = 0; i < kTasks; i++)
	{
		// the registers no decompile reads stay zero
		memset(s_tasks[i].regs, 0, sizeof(s_tasks[i].regs));
		s_tasks[i].busy.store(false, std::memory_order_relaxed);
		s_tasks[i].queued = false;
		s_tasks[i].ran = false;
	}
	Idle* idle = new Idle();
	memset(idle->regs, 0, sizeof(idle->regs));
	memset(idle->job.regs, 0, sizeof(idle->job.regs));
	idle->job.queued = false;
	s_idle.store(idle, std::memory_order_release);
	s_skip = 0;
	s_backoff.store(0, std::memory_order_relaxed);
	s_readTo.Reset();
	s_plannedJob = nullptr;
#endif
}

void LatteShaderAhead_Shutdown()
{
#ifdef ENABLE_VULKAN
	if (s_jobs)
	{
		// a read under way finished, one not started taken back
		for (uint32 i = 0; i < kJobs; i++)
		{
			if (s_jobs[i].queued)
				s_jobs[i].Prepared()->~PreparedPipeline();
		}
		delete[] s_jobs;
		s_jobs = nullptr;
	}
	// the guest's thread is stopped before the GPU thread: its read, if
	// under way, finished, if not started, taken back
	if (Idle* idle = s_idle.exchange(nullptr, std::memory_order_acq_rel))
	{
		if (idle->job.queued)
			idle->job.Prepared()->~PreparedPipeline();
		delete idle;
	}
	if (s_pipelineSlots)
	{
		// the reads are done: a pipeline under way finished, one not started
		// let go here, before the renderer goes
		for (uint32 i = 0; i < kPipelineSlots; i++)
		{
			PipelineSlot& slot = s_pipelineSlots[i];
			if (!slot.queued)
				continue;
			slot.Prepared()->~PreparedPipeline();
			if (!slot.ran)
				LetPipelineGo(slot);
		}
		delete[] s_pipelineSlots;
		s_pipelineSlots = nullptr;
		// the pipelines handed over that no draw took
		PipelineCompiler::DropAhead();
	}
	s_pipelinesKnown.Clear();
	s_pipelinesAhead.Clear();
	if (s_tasks)
	{
		// the reads are done: no task taken again. One under way finished,
		// one not started taken back, its slot given up
		for (uint32 i = 0; i < kTasks; i++)
		{
			Task& t = s_tasks[i];
			if (!t.queued)
				continue;
			t.Prepared()->~PreparedPipeline();
			if (!t.ran)
				(t.vertex ? s_vertex : s_pixel).Abandon(t.held);
		}
		delete[] s_tasks;
		s_tasks = nullptr;
	}
#endif
	for (Counts* c : {&s_countsVertex, &s_countsPixel, &s_countsGeometry})
	{
		if (c->made.load(std::memory_order_relaxed) || c->none.load(std::memory_order_relaxed))
			cemuLog_log(LogType::Force, "[ShaderAhead] {} shaders: {} decompiled ahead, {} taken by their draw, {} not usable there, {} drawn while being made, {} drawn with none made, {} let go unused, {} not made (their draw first)",
				c->kind, c->made.load(), c->taken.load(), c->unusable.load(), c->late.load(), c->none.load(), c->letGo.load(), c->drawnFirst.load());
		c->made = 0;
		c->taken = 0;
		c->unusable = 0;
		c->late = 0;
		c->none = 0;
		c->letGo = 0;
		c->drawnFirst = 0;
	}
	if (const uint64 reads = s_guestReads.exchange(0, std::memory_order_relaxed))
		cemuLog_log(LogType::Force, "[ShaderAhead] {} reads of command buffers before their submission", reads);
	if (const uint64 offers = s_ringEndOffers.exchange(0, std::memory_order_relaxed))
		cemuLog_log(LogType::Force, "[ShaderAhead] {} reads through to the ring's end offered their registers for the guest's buffer", offers);
#ifdef ENABLE_VULKAN
	if (const uint64 made = s_pipelinesMade.exchange(0, std::memory_order_relaxed))
		cemuLog_log(LogType::Force, "[ShaderAhead] {} pipelines made ahead of their draw", made);
#endif
	char line[512];
	if (s_drawPath.Describe(line, sizeof(line)))
		cemuLog_log(LogType::Force, "[ShaderAhead] {}", line);
	s_drawPath.Reset();
	for (Slots* slots : {&s_pixel, &s_vertex, &s_geometry})
	{
		slots->DrainPublished([](AheadShader& p) {
			LatteShader_FreeDecompilerOutput(p.output);
			p.output = {};
		});
	}
	s_seenPixel.Clear();
	s_seenVertex.Clear();
	s_seenGeometry.Clear();
}

void LatteShaderAhead_OnCommandBuffer(uint32 physAddr, uint32 words)
{
#ifdef ENABLE_VULKAN
	if (!s_jobs || words == 0)
		return;
	bool start;
	const uint32 seq = s_readTo.Next(s_plannedJob && s_plannedJob->queued && !s_plannedJob->Prepared()->IsBuilt(), start);
	if (!start)
		return;
	if (s_skip)
	{
		s_skip--;
		return;
	}
	Job* job = nullptr;
	for (uint32 i = 0; i < kJobs && !job; i++)
	{
		Job& j = s_jobs[i];
		if (j.queued)
		{
			if (!j.Prepared()->IsBuilt())
				continue; // still reading, or not yet started: never waited for
			j.Prepared()->~PreparedPipeline();
			j.queued = false;
		}
		job = &j;
	}
	if (!job)
		return;
	for (const auto& r : kCopied)
		memcpy(job->regs + r.start, LatteGPUState.contextRegister + r.start, r.count * sizeof(uint32));
	job->physAddr = physAddr;
	job->words = words;
	job->seq = seq;
	uint32 ringEnd;
	const uint32 peeked = TCL::TCLGPUPeekRB(job->ringWords, kRingWords, &ringEnd);
	job->ringCount = LattePM4_WholePackets(job->ringWords, peeked, &job->ringCalls);
	job->ringEnd = LatteAhead_RingEnd(peeked, job->ringCount, ringEnd, TCL::kRingNoEnd);
	s_readTo.Planned(seq, job->ringCalls);
	s_plannedJob = job;
	job->scan = s_scans.fetch_add(1, std::memory_order_relaxed) + 1;
	PreparedPipeline* prepared = new (job->prepared) PreparedPipeline([](void* ctx, bool) {
		RunJob(static_cast<Job*>(ctx));
		return (VkPipeline)VK_NULL_HANDLE;
	}, job);
	job->queued = true;
	PipelineCompiler::CompileThreadPool_QueuePreparedUrgent(*prepared);
	s_skip = LatteAhead_Skip(s_backoff.load(std::memory_order_relaxed));
#endif
}

void LatteShaderAhead_OnIdle(bool idle, uint32 ringAt)
{
#ifdef ENABLE_VULKAN
	Idle* state = s_idle.load(std::memory_order_relaxed);
	if (!state)
		return;
	if (!idle)
	{
		state->snapshot.Withdraw();
		return;
	}
	if (!state->snapshot.BeginFill())
		return; // the guest's thread reading one, or a read offering one
	for (const auto& r : kCopied)
		memcpy(state->regs + r.start, LatteGPUState.contextRegister + r.start, r.count * sizeof(uint32));
	state->snapshot.Offer(ringAt);
#endif
}

void LatteShaderAhead_OnGuestCommands(uint32 physAddr, uint32 words, uint32 ringAt)
{
#ifdef ENABLE_VULKAN
	Idle* state = s_idle.load(std::memory_order_acquire);
	if (!state || words == 0)
		return;
	Job& job = state->job;
	if (job.queued)
	{
		if (!job.Prepared()->IsBuilt())
			return; // its last read still under way: never waited for
		job.Prepared()->~PreparedPipeline();
		job.queued = false;
	}
	if (!state->snapshot.Take(ringAt))
		return;
	for (const auto& r : kCopied)
		memcpy(job.regs + r.start, state->regs + r.start, r.count * sizeof(uint32));
	state->snapshot.Release();
	job.physAddr = physAddr;
	job.words = words;
	job.seq = 0; // not one of the ring's: it counts nothing read
	job.ringCount = 0;
	job.ringCalls = 0;
	job.ringEnd = TCL::kRingNoEnd; // the guest's buffer, not submitted
	job.scan = s_scans.fetch_add(1, std::memory_order_relaxed) + 1;
	PreparedPipeline* prepared = new (job.prepared) PreparedPipeline([](void* ctx, bool) {
		RunJob(static_cast<Job*>(ctx));
		return (VkPipeline)VK_NULL_HANDLE;
	}, &job);
	job.queued = true;
	s_guestReads.fetch_add(1, std::memory_order_relaxed);
	PipelineCompiler::CompileThreadPool_QueuePreparedUrgent(*prepared);
#endif
}

bool LatteShaderAhead_TakePixelShader(uint64 baseHash, LatteDecompilerOutput_t& output)
{
	auto held = s_pixel.Take(baseHash);
	if (!held)
	{
		(s_pixel.InHand(baseHash) ? s_countsPixel.late : s_countsPixel.none).fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	AheadShader& p = *held.payload;
	const bool same = p.digest == LattePM4_PixelShaderInputs(LatteGPUState.contextRegister) &&
		p.auxHash == LatteSHRC_CalcPSAuxHash(p.output.shader, LatteGPUState.contextRegister);
	if (!same)
	{
		// put back, not freed here: its module and parts may be being made
		// (destroying them would wait); a read frees it once its base hash is
		// known, which the draw's own decompile makes it
		s_pixel.Publish(held);
		s_countsPixel.unusable.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	s_countsPixel.taken.fetch_add(1, std::memory_order_relaxed);
	output = p.output;
	p.output = {};
	s_pixel.Release(held);
	return true;
}

bool LatteShaderAhead_TakeVertexShader(uint64 baseHash, LatteFetchShader* fetchShader, LatteDecompilerOutput_t& output)
{
	auto held = s_vertex.Take(baseHash);
	if (!held)
	{
		(s_vertex.InHand(baseHash) ? s_countsVertex.late : s_countsVertex.none).fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	AheadShader& p = *held.payload;
	const bool same = p.digest == LattePM4_VertexShaderInputs(LatteGPUState.contextRegister) &&
		p.fetchShader == fetchShader->Fingerprint() &&
		p.auxHash == LatteSHRC_CalcVSAuxHash(p.output.shader, LatteGPUState.contextRegister);
	if (!same)
	{
		s_vertex.Publish(held); // as a pixel shader's: put back
		s_countsVertex.unusable.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	s_countsVertex.taken.fetch_add(1, std::memory_order_relaxed);
	output = p.output;
	output.shader->compatibleFetchShader = fetchShader;
	p.output = {};
	s_vertex.Release(held);
	return true;
}

bool LatteShaderAhead_TakeGeometryShader(uint64 baseHash, uint32 vsRingParameterCount, const uint8* copyProgram, uint32 copyProgramSize, LatteDecompilerOutput_t& output)
{
	auto held = s_geometry.Take(baseHash);
	if (!held)
	{
		(s_geometry.InHand(baseHash) ? s_countsGeometry.late : s_countsGeometry.none).fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	AheadShader& p = *held.payload;
	const bool same = p.digest == LattePM4_GeometryShaderInputs(LatteGPUState.contextRegister) &&
		p.output.shader->ringParameterCountFromPrevStage == vsRingParameterCount &&
		copyProgram && p.fetchShader == CopyShaderHash(copyProgram, copyProgramSize);
	if (!same)
	{
		s_geometry.Publish(held); // as a pixel shader's: put back
		s_countsGeometry.unusable.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	s_countsGeometry.taken.fetch_add(1, std::memory_order_relaxed);
	output = p.output;
	p.output = {};
	s_geometry.Release(held);
	return true;
}

void LatteShaderAhead_NotePixelShader(uint64 baseHash)
{
	s_seenPixel.Insert(baseHash);
}

void LatteShaderAhead_NoteVertexShader(uint64 baseHash)
{
	s_seenVertex.Insert(baseHash);
}

void LatteShaderAhead_NotePipeline(uint64 pipelineHash)
{
#ifdef ENABLE_VULKAN
	s_pipelinesKnown.Insert(pipelineHash);
#endif
}

bool LatteShaderAhead_PipelineQueuedAhead(uint64 pipelineHash)
{
#ifdef ENABLE_VULKAN
	return s_pipelinesAhead.Contains(pipelineHash);
#else
	return false;
#endif
}

LatteDrawPathCounts& LatteShaderAhead_DrawPathCounts()
{
	return s_drawPath;
}

void LatteShaderAhead_NoteGeometryShader(uint64 baseHash)
{
	s_seenGeometry.Insert(baseHash);
}

void LatteShaderAhead_Poll()
{
#ifdef ENABLE_VULKAN
	struct Kind
	{
		Slots& slots;
		std::atomic<uint64>& parts;
	};
	for (Kind k : {Kind{s_pixel, s_partsPixel}, Kind{s_vertex, s_partsVertex}})
	{
		if (k.parts.load(std::memory_order_relaxed) == 0)
			continue;
		for (uint64 bits = k.parts.exchange(0, std::memory_order_acquire); bits; bits &= bits - 1)
		{
			// held while its parts are queued: no draw or read takes it meanwhile
			auto held = k.slots.TakeAt((size_t)std::countr_zero(bits));
			if (!held)
				continue;
			AheadShader& p = *held.payload;
			if (p.partsPending && p.output.shader && p.output.shader->shader)
				PipelineCompiler::PrepareAheadPartsFor(static_cast<RendererShaderVk*>(p.output.shader->shader), p.output.shader, p.robust);
			p.partsPending = false;
			k.slots.Publish(held);
		}
	}
#endif
}
