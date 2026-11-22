#pragma once
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "VKRBase.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/PreparedPipeline.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "util/helpers/PoolNew.h"

// A new pipeline's state, built on the GPU thread and compiled there or on a
// compile thread. From a pool (PoolNew.h), with its state in arrays of its
// own, so that a game reaching new state allocates nothing for it: only the
// driver does, compiling.
// A rect emulation geometry shader (RECTS without VK_NV_fill_rectangle): one
// shared by every pipeline whose source is alike (sharedKey its key), or one
// for the pipeline alone to keep (sharedKey 0). Queued for the compile
// threads, not compiled on the thread asking.
struct RectEmulationGS
{
	class RendererShaderVk* shader;
	uint64 sharedKey;
};
RectEmulationGS rectsEmulationGS_generate(LatteDecompilerShader* vertexShader, const LatteContextRegister& latteRegister);
// the shared ones, once no pipeline is left
void rectsEmulationGS_deleteShared();

class PipelineCompiler : public VKRMoveableRefCounter, public PoolNew<PipelineCompiler, 64>
{
private:
	// helper functions
	bool ConsumesBlendConstants(VkBlendFactor blendFactor);

	void CreateDescriptorSetLayout(VulkanRenderer* vkRenderer, LatteDecompilerShader* shader, VkDescriptorSetLayout& layout, PipelineInfo* vkrPipelineInfo);

private:

	/* shader stages (requires compiled shader) */

	RendererShaderVk* m_rectEmulationGS{};
	uint64 m_rectEmulationKey{}; // its source's, when shared (rectsEmulationGS_generate)

	bool InitShaderStages(VulkanRenderer* vkRenderer, RendererShaderVk* vkVertexShader, RendererShaderVk* vkPixelShader, RendererShaderVk* vkGeometryShader);

	/* vertex input state */

	void InitVertexInputState(const LatteContextRegister& latteRegister, LatteDecompilerShader* vertexShader, LatteFetchShader* fetchShader);
	void InitInputAssemblyState(const Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode);
	void InitViewportState();
	void InitRasterizerState(const LatteContextRegister& latteRegister, VulkanRenderer* vkRenderer, bool isPrimitiveRect, bool& usesDepthBias);
	void InitBlendState(const LatteContextRegister& latteRegister, PipelineInfo* pipelineInfo, bool& usesBlendConstants, VKRObjectRenderPass* renderPassObj);
	void InitDescriptorSetLayouts(VulkanRenderer* vkRenderer, PipelineInfo* vkrPipelineInfo, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader);
	void InitDepthStencilState(const LatteContextRegister& latteRegister);
	void InitDynamicState(PipelineInfo* pipelineInfo, bool usesBlendConstants, bool usesDepthBias);

public:
	PipelineCompiler();
	~PipelineCompiler();

	VKRObjectPipeline* m_vkrObjPipeline{};
	LatteFetchShader* m_fetchShader{};
	RendererShaderVk* m_vkVertexShader{};
	RendererShaderVk* m_vkPixelShader{};
	RendererShaderVk* m_vkGeometryShader{};

	bool InitFromCurrentGPUState(PipelineInfo* pipelineInfo, const LatteContextRegister& latteRegister, VKRObjectRenderPass* renderPassObj, bool requireRobustBufferAccess);
	void TrackAsCached(uint64 baseHash, uint64 pipelineStateHash); // stores pipeline to permanent cache if not yet cached. Must be called synchronously from render thread due to dependency on GPU state

	static bool CalcRobustBufferAccessRequirement(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader);

	// API for thread pool
	static void CompileThreadPool_Start();
	static void CompileThreadPool_Stop();
	// an optimized link, after anything a draw needs now
	static void CompileThreadPool_QueueCompilation(PipelineCompiler* v);
	// a pipeline made ahead, built by the compile threads (PreparedPipeline.h),
	// for a draw that needs it now: before anything queued ahead
	static void CompileThreadPool_QueuePrepared(PreparedPipeline& prepared);
	// the same, for one only a later draw may need (parts and pipelines made
	// ahead): after everything a draw needs now
	static void CompileThreadPool_QueuePreparedAhead(PreparedPipeline& prepared);
	// the same, taken before anything queued without it, by the next compile
	// thread free: for work worth nothing late and short (the reads ahead of
	// the GPU thread, LatteShaderAhead.cpp), never a pipeline's compile
	static void CompileThreadPool_QueuePreparedUrgent(PreparedPipeline& prepared);

	VkPipelineLayout m_pipelineLayout;
	VKRObjectRenderPass* m_renderPassObj{};
	bool m_requestRobustBufferAccess{false};
	// the pre-rasterization and fragment shader parts' own, chained to each
	// alone; false without VK_EXT_pipeline_robustness (the device's then)
	bool m_robustVertex{false};
	bool m_robustPixel{false};
	// made ahead of its draw by a read (LatteShaderAhead.cpp): the parts it
	// makes together in the compile threads' lane of work made ahead, and the
	// pipeline, linked optimized, handed to its draw (HandOverAhead)
	bool m_madeAhead{false};
	// a try's pipeline known to be in the driver's cache (VulkanPipeline_Try)
	bool m_driverCached{false};

	/* shader stages */
	VkPipelineShaderStageCreateInfo shaderStages[3]{};
	uint32 shaderStageCount{0};

	/* vertex attribute description */
	VkVertexInputAttributeDescription vertexInputAttributeDescription[LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS]{};
	VkVertexInputBindingDescription vertexInputBindingDescription[Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS]{};
	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};

	/* input assembly state */
	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};

	/* viewport state */
	VkPipelineViewportStateCreateInfo viewportState{};

	/* rasterizer state */
	VkPipelineRasterizationStateCreateInfo rasterizer{};
	VkPipelineRasterizationDepthClipStateCreateInfoEXT rasterizerExt{};
	VkPipelineMultisampleStateCreateInfo multisampling{};

	/* blend state */
	std::array<VkPipelineColorBlendAttachmentState, 8> colorBlendAttachments{};
	VkPipelineColorBlendStateCreateInfo colorBlending{};

	/* descriptor set layouts */
	VkDescriptorSetLayout descriptorSetLayout[3];
	sint32 descriptorSetLayoutCount = 0;

	/* depth stencil state */
	VkPipelineDepthStencilStateCreateInfo depthStencilState{};

	/* dynamic state */
	VkDynamicState dynamicStates[VULKAN_PIPELINE_MAX_DYNAMIC_STATES]{};
	VkPipelineDynamicStateCreateInfo dynamicState = {};

	// returns true if the shader was compiled (even if errors occurred)
	bool Compile(bool forceCompile, bool isRenderThread, bool showInOverlay);
	// a part made by this thread, under the key's claim when claimed (else
	// adopted by the cache); VK_NULL_HANDLE with result when it was not
	VkPipeline MakeLibraryPart(int part, uint64 key, bool claimed, const VkGraphicsPipelineCreateInfo& full, bool forceCompile, bool isRenderThread, VkResult& result);

	/* graphics pipeline libraries (VulkanPipelineLibrary.h) */

	// the parts' own layouts: the pre-rasterization shaders' sets and the
	// pixel shader's (the whole pipeline's is m_pipelineLayout)
	VkPipelineLayout m_partLayout[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	// a job for a compile thread that only links the parts, optimized, and
	// hands the pipeline to the GPU thread to take the fast-linked one's
	// place (VulkanRenderer::draw_takeOptimizedPipelines)
	bool m_linkOptimizedOnly{false};
	// a compile for a draw skipped until it lands (AsyncPipelineCompile):
	// linked fast as soon as its parts are made, the optimized link after
	bool m_drawWaits{false};
	uint64 m_rectEmulationSerial{0};
	VkPipeline m_linkLibraries[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	// a pipeline the compile threads linked optimized, for the GPU thread
	struct OptimizedPipeline
	{
		VKRObjectPipeline* object; // a reference held until taken
		VkPipeline pipeline;
	};
	static bool TakeOptimizedPipeline(OptimizedPipeline& result);

	// A pipeline a read made ahead of its draw, with the layouts it was made
	// with (a layout outlives its pipelines without VK_KHR_maintenance4), by
	// the compile thread that made it: kept for the draw, which takes it
	// whole (TakeAhead, the GPU thread) in place of its own, its layouts
	// defined the same, rather than linking or compiling it there; given up
	// unused once newer ones crowd it out, or as the title stops (DropAhead,
	// the GPU thread, no read running)
	static void HandOverAhead(uint64 pipelineHash, VKRObjectPipeline* pipeline);
	static VKRObjectPipeline* TakeAhead(uint64 pipelineHash);
	static void DropAhead();

	// the compile threads' part of m_linkOptimizedOnly
	bool LinkOptimized();

	// a job for a compile thread that only builds a prepared pipeline
	PreparedPipeline::Ticket* m_preparedTicket{};

	// The GPU thread's, for a pipeline it set up (InitFromCurrentGPUState):
	// its pre-rasterization and fragment shader state kept as templates, and
	// for a shader of it seen for the first time, its parts for the other
	// templates queued for the compile threads (docs/SHADER-STUTTER.md M2)
	void PrepareAheadParts(PipelineInfo* pipelineInfo);
	// a shader's, as it goes: not made if not started, waited for if under way
	static void RetireAheadParts(struct AheadParts* ahead);
	// The GPU thread's, for a shader decompiled and made ahead of its first
	// draw (LatteShaderAhead.cpp): its part for every template seen so far,
	// queued for the compile threads as a first pipeline queues its
	// shaders' (robust: what the shader itself asks for, all a part takes);
	// false with no template or graphics pipeline libraries
	static bool PrepareAheadPartsFor(RendererShaderVk* shader, LatteDecompilerShader* decompiled, bool robust);
	// a compile thread's: the parts made
	static void BuildAheadParts(struct AheadParts* ahead, bool onCompileThread);

private:
	bool CompileFromLibraries(const VkGraphicsPipelineCreateInfo& full, bool forceCompile, bool madeOnly, bool isRenderThread);
	// the parts a draw waits for, the missing ones made together (BuildTogether)
	bool GatherLibraryPartsTogether(const VkGraphicsPipelineCreateInfo& full, const uint64 keys[], VkPipeline libraries[], bool isRenderThread);
	void LogCompileFailure(VkResult result, sint32 topology);
public:

};

// A pipeline compiled on a compile thread for a draw that could be skipped
// meanwhile (Async Shader Compile), owned by its PipelineInfo. A later draw
// that may not be skipped compiles it itself when no compile thread has
// started on it, as it would have for a new pipeline, or waits for the one
// that has, rather than being skipped as well
// (VulkanRenderer::draw_awaitAsyncPipeline). Destroyed with the
// PipelineInfo: a compile not started is dropped, one under way finished.
struct AsyncPipelineCompile : PoolNew<AsyncPipelineCompile, 64>
{
	explicit AsyncPipelineCompile(PipelineCompiler* job) : m_job(job) {}
	~AsyncPipelineCompile()
	{
		prepared.Retire();
		delete m_job; // nullptr once compiled
	}
	AsyncPipelineCompile(const AsyncPipelineCompile&) = delete;
	AsyncPipelineCompile& operator=(const AsyncPipelineCompile&) = delete;

	PipelineCompiler* m_job;
	// last, so that it is destroyed first
	PreparedPipeline prepared{[](void* self, bool onCompileThread) {
		AsyncPipelineCompile* c = (AsyncPipelineCompile*)self;
		c->m_job->m_drawWaits = true;
		c->m_job->Compile(true, !onCompileThread, true);
		const VkPipeline pipeline = c->m_job->m_vkrObjPipeline->GetPipeline();
		delete c->m_job;
		c->m_job = nullptr;
		return pipeline;
	}, this};
};
