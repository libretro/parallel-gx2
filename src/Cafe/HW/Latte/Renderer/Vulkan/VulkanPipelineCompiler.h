#pragma once
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "VKRBase.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/PreparedPipeline.h"

class PipelineCompiler : public VKRMoveableRefCounter
{
private:
	// helper functions
	bool ConsumesBlendConstants(VkBlendFactor blendFactor);

	void CreateDescriptorSetLayout(VulkanRenderer* vkRenderer, LatteDecompilerShader* shader, VkDescriptorSetLayout& layout, PipelineInfo* vkrPipelineInfo);

private:

	/* shader stages (requires compiled shader) */

	RendererShaderVk* m_rectEmulationGS{};

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
	static void CompileThreadPool_QueueCompilation(PipelineCompiler* v);
	// a pipeline made ahead, built by the compile threads (PreparedPipeline.h)
	static void CompileThreadPool_QueuePrepared(PreparedPipeline& prepared);

	VkPipelineLayout m_pipelineLayout;
	VKRObjectRenderPass* m_renderPassObj{};
	bool m_requestRobustBufferAccess{false};

	/* shader stages */
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

	/* vertex attribute description */
	std::vector<VkVertexInputAttributeDescription> vertexInputAttributeDescription;
	std::vector<VkVertexInputBindingDescription> vertexInputBindingDescription;
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
	std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};

	// returns true if the shader was compiled (even if errors occurred)
	bool Compile(bool forceCompile, bool isRenderThread, bool showInOverlay);

	/* graphics pipeline libraries (VulkanPipelineLibrary.h) */

	// the parts' own layouts: the pre-rasterization shaders' sets and the
	// pixel shader's (the whole pipeline's is m_pipelineLayout)
	VkPipelineLayout m_partLayout[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	// a job for a compile thread that only links the parts, optimized, and
	// hands the pipeline to the GPU thread to take the fast-linked one's
	// place (VulkanRenderer::draw_takeOptimizedPipelines)
	bool m_linkOptimizedOnly{false};
	uint64 m_rectEmulationSerial{0};
	VkPipeline m_linkLibraries[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	// a pipeline the compile threads linked optimized, for the GPU thread
	struct OptimizedPipeline
	{
		VKRObjectPipeline* object; // a reference held until taken
		VkPipeline pipeline;
	};
	static bool TakeOptimizedPipeline(OptimizedPipeline& result);

	// the compile threads' part of m_linkOptimizedOnly
	bool LinkOptimized();

	// a job for a compile thread that only builds a prepared pipeline
	PreparedPipeline::Ticket* m_preparedTicket{};

private:
	bool CompileFromLibraries(const VkGraphicsPipelineCreateInfo& full, bool forceCompile, bool isRenderThread);
public:

};

// A pipeline compiled on a compile thread for a draw that could be skipped
// meanwhile (Async Shader Compile), owned by its PipelineInfo. A later draw
// that may not be skipped compiles it itself when no compile thread has
// started on it, as it would have for a new pipeline, or waits for the one
// that has, rather than being skipped as well
// (VulkanRenderer::draw_awaitAsyncPipeline). Destroyed with the
// PipelineInfo: a compile not started is dropped, one under way finished.
struct AsyncPipelineCompile
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
		c->m_job->Compile(true, !onCompileThread, true);
		const VkPipeline pipeline = c->m_job->m_vkrObjPipeline->GetPipeline();
		delete c->m_job;
		c->m_job = nullptr;
		return pipeline;
	}, this};
};
