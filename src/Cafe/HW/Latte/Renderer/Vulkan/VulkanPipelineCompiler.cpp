#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "util/helpers/SharedByKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#include "Cafe/HW/Latte/Core/PsoTrace.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "util/helpers/Serializer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "HW/Latte/Renderer/RendererCore.h"
#include "util/helpers/ThreadOwned.h"
#include "util/helpers/AheadSlots.h"

// The source of the rect emulation geometry shader for a vertex shader and
// the pixel shader's inputs, written into gsSrc (emptied first). The inputs
// are the registers' own: a pipeline the cache loads is made on a compile
// thread from the state the cache stored, while the GPU thread's table is
// whatever it draws now (check_pipeline_registers.sh).
static void rectsEmulationGS_write(std::string& gsSrc, LatteDecompilerShader* vertexShader, const LatteContextRegister& latteRegister)
{
	gsSrc.clear();

	gsSrc.append("#version 450\r\n");

	LatteShaderPSInputTable inputs;
	LatteShader_CreatePSInputTable(&inputs, latteRegister.GetRawView());
	LatteShaderPSInputTable* psInputTable = &inputs;

	// layout
	gsSrc.append("layout(triangles) in;\r\n");
	gsSrc.append("layout(triangle_strip) out;\r\n");
	gsSrc.append("layout(max_vertices = 4) out;\r\n");

	// inputs & outputs
	auto parameterMask = vertexShader->outputParameterMask;
	for (sint32 f = 0; f < 2; f++)
	{
		for (uint32 i = 0; i < 32; i++)
		{
			if ((parameterMask & (1 << i)) == 0)
				continue;
			sint32 vsSemanticId = psInputTable->getVertexShaderOutParamSemanticId(latteRegister.GetRawView(), i);
			if (vsSemanticId < 0)
				continue;
			auto psImport = psInputTable->getPSImportBySemanticId(vsSemanticId);
			if (psImport == nullptr)
				continue;

			fmt::format_to(std::back_inserter(gsSrc), "layout(location = {}) ", psInputTable->getPSImportLocationBySemanticId(vsSemanticId));
			if (psImport->isFlat)
				gsSrc.append("flat ");
			if (psImport->isNoPerspective)
				gsSrc.append("noperspective ");

			if (f == 0)
				gsSrc.append("in");
			else
				gsSrc.append("out");

			if (f == 0)
				fmt::format_to(std::back_inserter(gsSrc), " vec4 passParameterSem{}In[];\r\n", vsSemanticId);
			else
				fmt::format_to(std::back_inserter(gsSrc), " vec4 passParameterSem{}Out;\r\n", vsSemanticId);
		}
	}

	// gen function
	gsSrc.append("vec4 gen4thVertexA(vec4 a, vec4 b, vec4 c)\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("return b - (c - a);\r\n");
	gsSrc.append("}\r\n");

	gsSrc.append("vec4 gen4thVertexB(vec4 a, vec4 b, vec4 c)\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("return c - (b - a);\r\n");
	gsSrc.append("}\r\n");

	gsSrc.append("vec4 gen4thVertexC(vec4 a, vec4 b, vec4 c)\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("return c + (b - a);\r\n");
	gsSrc.append("}\r\n");

	// main
	gsSrc.append("void main()\r\n");
	gsSrc.append("{\r\n");

	// there are two possible winding orders that need different triangle generation:
	// 0 1
	// 2 3
	// and
	// 0 1
	// 3 2
	// all others are just symmetries of these cases

	// we can determine the case by comparing the distance 0<->1 and 0<->2

	gsSrc.append("float dist0_1 = length(gl_in[1].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n");
	gsSrc.append("float dist0_2 = length(gl_in[2].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n");
	gsSrc.append("float dist1_2 = length(gl_in[2].gl_Position.xy - gl_in[1].gl_Position.xy);\r\n");

	// emit vertices
	gsSrc.append("if(dist0_1 > dist0_2 && dist0_1 > dist1_2)\r\n");
	gsSrc.append("{\r\n");
	// p0 to p1 is diagonal
	rectsEmulationGS_outputVerticesCode(gsSrc, vertexShader, psInputTable, 2, 1, 0, 3, "A", latteRegister);
	gsSrc.append("} else if ( dist0_2 > dist0_1 && dist0_2 > dist1_2 ) {\r\n");
	// p0 to p2 is diagonal
	rectsEmulationGS_outputVerticesCode(gsSrc, vertexShader, psInputTable, 1, 2, 0, 3, "B", latteRegister);
	gsSrc.append("} else {\r\n");
	// p1 to p2 is diagonal
	rectsEmulationGS_outputVerticesCode(gsSrc, vertexShader, psInputTable, 0, 1, 2, 3, "C", latteRegister);
	gsSrc.append("}\r\n");

	gsSrc.append("}\r\n");

}

// The rect emulation geometry shaders, one for each source written: the
// pipelines whose vertex shader outputs and pixel shader inputs are alike
// share one, compiled once on a compile thread rather than once per pipeline
// on the thread making it. The GPU thread and the cache loader's compile
// threads make pipelines at once, so the table is lock-free (SharedByKey.h);
// a thread that finds the source's key being published, or no slot left,
// makes one of its own, which its pipeline keeps. The shared ones go with the
// renderer (rectsEmulationGS_deleteShared).
static SharedByKey<RendererShaderVk, 1024> s_rectGS;

static uint64 RectGSHash(std::string_view src, uint64 h, uint64 mul)
{
	for (unsigned char c : src)
		h = (h ^ c) * mul;
	return h ^ (h >> 31);
}

RectEmulationGS rectsEmulationGS_generate(LatteDecompilerShader* vertexShader, const LatteContextRegister& latteRegister)
{
	// a source a compiled shader gave back; moved into the shader made, or
	// given back again
	thread_local std::string* t_gsSrc;
	std::string& gsSrc = ThreadOwned(t_gsSrc);
	LatteDecompiler_SourceBuffers().Take(gsSrc);
	rectsEmulationGS_write(gsSrc, vertexShader, latteRegister);
	const uint64 key = RectGSHash(gsSrc, 0xcbf29ce484222325ull, 0x100000001b3ull) | 1;
	const uint64 check = RectGSHash(gsSrc, 0x9E3779B97F4A7C15ull, 0xff51afd7ed558ccdull);
	bool made = false;
	RendererShaderVk* shared = s_rectGS.Get(key, check, [&] {
		made = true;
		// queued for the compile threads
		return new RendererShaderVk(RendererShader::ShaderType::kGeometry, 0, 0, false, false, std::move(gsSrc));
	});
	if (shared)
	{
		if (!made)
			LatteDecompiler_SourceBuffers().Give(gsSrc);
		return { shared, key };
	}
	return { new RendererShaderVk(RendererShader::ShaderType::kGeometry, 0, 0, false, false, std::move(gsSrc)), 0 };
}

void rectsEmulationGS_deleteShared()
{
	s_rectGS.Clear([](RendererShaderVk* shader) { delete shader; });
}

/* pipeline compiler and cache helper */


PipelineCompiler::PipelineCompiler() {};
PipelineCompiler::~PipelineCompiler()
{
	if (m_preparedTicket)
		PreparedPipeline::Drop(m_preparedTicket);
	for (VkPipelineLayout layout : m_partLayout)
		if (layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(VulkanRenderer::GetInstance()->GetLogicalDevice(), layout, nullptr);
	if (m_vkrObjPipeline)
		m_vkrObjPipeline->decRef();
	if (m_renderPassObj)
		m_renderPassObj->decRef();
};

bool PipelineCompiler::ConsumesBlendConstants(VkBlendFactor blendFactor)
{
	if (blendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
		blendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR ||
		blendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
		blendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA)
		return true;
	return false;
}

void PipelineCompiler::CreateDescriptorSetLayout(VulkanRenderer* vkRenderer, LatteDecompilerShader* shader, VkDescriptorSetLayout& layout, PipelineInfo* vkrPipelineInfo)
{
	VkShaderStageFlags stageFlags = 0;
	uint32 stageIndex = 0;
	if (shader->shaderType == LatteConst::ShaderType::Vertex)
	{
		stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		stageIndex = VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX;
	}
	else if (shader->shaderType == LatteConst::ShaderType::Pixel)
	{
		stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		stageIndex = VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT;
	}
	else if (shader->shaderType == LatteConst::ShaderType::Geometry)
	{
		stageFlags = VK_SHADER_STAGE_GEOMETRY_BIT;
		stageIndex = VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY;
	}
	// the textures, uniform register block, uniform buffers and transform
	// feedback buffer (attributes are not in a descriptor set)
	VulkanDescriptorBindings b;
	VulkanPipelineKey_DescriptorBindings(shader->resourceMapping, stageFlags, b);

	auto& offsets = vkrPipelineInfo->dynamicOffsetInfo;
	std::copy_n(b.uniformBuffers, b.uniformBufferCount, offsets.list_uniformBuffers[stageIndex]);
	offsets.uniformBufferCount[stageIndex] = (uint8)b.uniformBufferCount;
	if (b.hasUniformVar)
		offsets.hasUniformVar[stageIndex] = true;
	if (b.hasUniformBuffers)
		offsets.hasUniformBuffers[stageIndex] = true;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = b.count;
	layoutInfo.pBindings = b.bindings;

	if (vkCreateDescriptorSetLayout(vkRenderer->m_logicalDevice, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		vkRenderer->UnrecoverableError(fmt::format("Failed to create descriptor set layout for shader {0:#x}", shader->baseHash).c_str());
}

bool PipelineCompiler::InitShaderStages(VulkanRenderer* vkRenderer, RendererShaderVk* vkVertexShader, RendererShaderVk* vkPixelShader, RendererShaderVk* vkGeometryShader)
{
	// prepare shader stages
	cemu_assert_debug(vkVertexShader == nullptr || vkVertexShader->IsCompiled());
	cemu_assert_debug(vkPixelShader == nullptr || vkPixelShader->IsCompiled());
	cemu_assert_debug(vkGeometryShader == nullptr || vkGeometryShader->IsCompiled());
	cemu_assert_debug(m_rectEmulationGS == nullptr || m_rectEmulationGS->IsCompiled());

	if ((vkVertexShader && vkVertexShader->GetShaderModule() == VK_NULL_HANDLE) ||
		(vkGeometryShader && vkGeometryShader->GetShaderModule() == VK_NULL_HANDLE) ||
		(!vkGeometryShader && m_rectEmulationGS && m_rectEmulationGS->GetShaderModule() == VK_NULL_HANDLE) ||
		(vkPixelShader && vkPixelShader->GetShaderModule() == VK_NULL_HANDLE))
	{
		cemuLog_log(LogType::Force, "Vulkan-Info: Pipeline creation failed due to invalid shader(s)");
		return false;
	}

	if (vkVertexShader)
		shaderStages[shaderStageCount++] = vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, vkVertexShader->GetShaderModule(), "main");

	if (vkGeometryShader)
		shaderStages[shaderStageCount++] = vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_GEOMETRY_BIT, vkGeometryShader->GetShaderModule(), "main");
	else if (m_rectEmulationGS)
		shaderStages[shaderStageCount++] = vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_GEOMETRY_BIT, m_rectEmulationGS->GetShaderModule(), "main");

	if (vkPixelShader)
		shaderStages[shaderStageCount++] = vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, vkPixelShader->GetShaderModule(), "main");

	return true;
}

void PipelineCompiler::InitVertexInputState(const LatteContextRegister& latteRegister, LatteDecompilerShader* vertexShader, LatteFetchShader* fetchShader)
{
	// with VK_EXT_vertex_input_dynamic_state the draw sets the same
	// (draw_updateVertexInput) and what is baked here is ignored
	VulkanVertexInput input;
	VulkanPipelineKey_VertexInput(*fetchShader, vertexShader->resourceMapping.attributeMapping, latteRegister, input);

	for (uint32 i = 0; i < input.attributeCount; i++)
	{
		const auto& a = input.attributes[i];
		if (a.format == VK_FORMAT_UNDEFINED)
			cemuLog_log(LogType::Force, "Unsupported vertex format in fetch shader {:016x}", fetchShader->key);
		vertexInputAttributeDescription[i] = {a.location, a.binding, a.format, a.offset};
	}
	for (uint32 i = 0; i < input.bindingCount; i++)
	{
		const auto& b = input.bindings[i];
		vertexInputBindingDescription[i] = {b.binding, b.stride, b.inputRate};
	}

	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = input.bindingCount;
	vertexInputInfo.pVertexBindingDescriptions = vertexInputBindingDescription;
	vertexInputInfo.vertexAttributeDescriptionCount = input.attributeCount;
	vertexInputInfo.pVertexAttributeDescriptions = vertexInputAttributeDescription;
}

void PipelineCompiler::InitInputAssemblyState(const Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode)
{
	// with dynamic topology the draw sets the same (draw_updateInputAssembly)
	const VulkanInputAssembly ia = VulkanPipelineKey_InputAssembly(primitiveMode);
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = ia.topology;
	inputAssembly.primitiveRestartEnable = ia.primitiveRestartEnable;
	if (!ia.supported)
	{
		cemuLog_logDebug(LogType::Force, "Vulkan-Unsupported: Graphics pipeline with primitive mode {} created", primitiveMode);
		cemu_assert_debug(false);
	}
}

void PipelineCompiler::InitViewportState()
{
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
}

void PipelineCompiler::InitRasterizerState(const LatteContextRegister& latteRegister, VulkanRenderer* vkRenderer, bool isPrimitiveRect, bool& usesDepthBias)
{
	// polygon control
	const VulkanRasterizerEnables enables = VulkanPipelineKey_Rasterizer(latteRegister);
	// with VK_EXT_extended_dynamic_state2 both enables are dynamic state
	// (InitDynamicState), set by the draw (draw_updateRasterizer), what is
	// set here then ignored; the depth bias values are dynamic regardless
	const bool dynamicEnables = vkRenderer->m_featureControl.deviceExtensions.extended_dynamic_state2;

	cemu_assert_debug(latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() == latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE()); // near or far clipping can be disabled individually
	bool zClipEnable = latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() == false;

	// z-clipping
	rasterizerExt.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	rasterizerExt.depthClipEnable = zClipEnable;
	rasterizerExt.flags = 0;

	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.rasterizerDiscardEnable = enables.rasterizerDiscardEnable;
	rasterizer.pNext = VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.depth_clip_enable ? &rasterizerExt : nullptr;

	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	if (vkRenderer->m_featureControl.deviceExtensions.nv_fill_rectangle && isPrimitiveRect)
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL_RECTANGLE_NV;

	rasterizer.depthClampEnable = VK_TRUE; // depth clamping is always enabled

	rasterizer.lineWidth = 1.0f; // TODO -> mmPA_SU_LINE_CNTL

	usesDepthBias = dynamicEnables || enables.depthBiasEnable;
	rasterizer.depthBiasEnable = enables.depthBiasEnable;
	// initialize to zero, set dynamically via vkCmdSetDepthBias
	rasterizer.depthBiasConstantFactor = 0.0f;
	rasterizer.depthBiasSlopeFactor = 0.0f;
	rasterizer.depthBiasClamp = 0.0f;

	const VulkanDepthStencilCull dsc = VulkanPipelineKey_DepthStencilCull(latteRegister);
	rasterizer.cullMode = dsc.cullMode;
	rasterizer.frontFace = dsc.frontFace;

	// multisampling
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
}

void PipelineCompiler::InitBlendState(const LatteContextRegister& latteRegister, PipelineInfo* pipelineInfo, bool& usesBlendConstants, VKRObjectRenderPass* renderPassObj)
{
	const Latte::LATTE_CB_COLOR_CONTROL& colorControlReg = latteRegister.CB_COLOR_CONTROL;

	VkFormat colorFormats[8];
	for (size_t i = 0; i < 8; i++)
		colorFormats[i] = renderPassObj->GetColorFormat(i);
	usesBlendConstants = VulkanPipelineKey_Blend(latteRegister, colorFormats, colorBlendAttachments.data());
	// with the blend equations dynamic state (draw_updateBlend), the draw may
	// pick factors that read them
	if (VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.extended_dynamic_state3_blend)
		usesBlendConstants = true;

	// setup VkPipelineColorBlendStateCreateInfo
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

	const auto logicOp = colorControlReg.get_ROP();
	if (logicOp == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY)
	{
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
	}
	else
	{
		colorBlending.logicOpEnable = VK_TRUE;
		switch (logicOp)
		{
		case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::SET:
			colorBlending.logicOp = VK_LOGIC_OP_SET;
			break;
		case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR:
			colorBlending.logicOp = VK_LOGIC_OP_CLEAR;
			break;
		case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::OR:
			colorBlending.logicOp = VK_LOGIC_OP_OR;
			break;
		default:
			colorBlending.logicOp = VK_LOGIC_OP_COPY;
			cemu_assert_unimplemented();
		}
	}

	colorBlending.attachmentCount = colorBlendAttachments.size();
	colorBlending.pAttachments = colorBlendAttachments.data();

	// we use VK_DYNAMIC_STATE_BLEND_CONSTANTS, the blend constants here don't matter
	colorBlending.blendConstants[0] = 0;
	colorBlending.blendConstants[1] = 0;
	colorBlending.blendConstants[2] = 0;
	colorBlending.blendConstants[3] = 0;
}

void PipelineCompiler::InitDescriptorSetLayouts(VulkanRenderer* vkRenderer, PipelineInfo* vkrPipelineInfo, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader)
{
	auto vkObjPipeline = vkrPipelineInfo->m_vkrObjPipeline;

	if (vertexShader)
	{
		cemu_assert_debug(descriptorSetLayoutCount == 0);
		CreateDescriptorSetLayout(vkRenderer, vertexShader, descriptorSetLayout[descriptorSetLayoutCount], vkrPipelineInfo);
		vkObjPipeline->m_vertexDSL = descriptorSetLayout[descriptorSetLayoutCount];
		descriptorSetLayoutCount++;
	}

	if (pixelShader)
	{
		cemu_assert_debug(descriptorSetLayoutCount == 1);
		CreateDescriptorSetLayout(vkRenderer, pixelShader, descriptorSetLayout[descriptorSetLayoutCount], vkrPipelineInfo);
		vkObjPipeline->m_pixelDSL = descriptorSetLayout[descriptorSetLayoutCount];
		descriptorSetLayoutCount++;
	}
	else if (geometryShader)
	{
		// if no pixel shader is present, create empty placeholder descriptor set layout (geometry shader set must be at index 2)
		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = 0;
		layoutInfo.pBindings = nullptr;
		if (vkCreateDescriptorSetLayout(vkRenderer->m_logicalDevice, &layoutInfo, nullptr, &descriptorSetLayout[descriptorSetLayoutCount]) != VK_SUCCESS)
			vkRenderer->UnrecoverableError(fmt::format("Failed to create placeholder descriptor set layout for shader {0:#x}", geometryShader->baseHash).c_str());
		descriptorSetLayoutCount++;
	}

	if (geometryShader)
	{
		cemu_assert_debug(descriptorSetLayoutCount == 2);
		CreateDescriptorSetLayout(vkRenderer, geometryShader, descriptorSetLayout[descriptorSetLayoutCount], vkrPipelineInfo);
		vkObjPipeline->m_geometryDSL = descriptorSetLayout[descriptorSetLayoutCount];
		descriptorSetLayoutCount++;
	}
}

void PipelineCompiler::InitDepthStencilState(const LatteContextRegister& latteRegister)
{
	const VulkanDepthStencilCull dsc = VulkanPipelineKey_DepthStencilCull(latteRegister);
	depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilState.depthTestEnable = dsc.depthTestEnable;
	depthStencilState.depthWriteEnable = dsc.depthWriteEnable;
	depthStencilState.depthCompareOp = dsc.depthCompareOp;
	depthStencilState.depthBoundsTestEnable = false; // todo
	depthStencilState.minDepthBounds = 0.0f;
	depthStencilState.maxDepthBounds = 1.0f;
	depthStencilState.stencilTestEnable = dsc.stencilTestEnable;
	depthStencilState.front.compareOp = dsc.frontCompareOp;
	depthStencilState.front.passOp = dsc.frontPassOp;
	depthStencilState.front.depthFailOp = dsc.frontDepthFailOp;
	depthStencilState.front.failOp = dsc.frontFailOp;
	depthStencilState.back.compareOp = dsc.backCompareOp;
	depthStencilState.back.passOp = dsc.backPassOp;
	depthStencilState.back.depthFailOp = dsc.backDepthFailOp;
	depthStencilState.back.failOp = dsc.backFailOp;
	// The stencil reference, compare mask and write mask are dynamic state
	// (InitDynamicState), set by the draw (draw_updateStencilState): a
	// pipeline per value of them was a new driver compile for each. With
	// VK_EXT_extended_dynamic_state the rest of this is dynamic state too
	// (draw_updateDepthStencilCull), what is set here then ignored.
}

void PipelineCompiler::InitDynamicState(PipelineInfo* pipelineInfo, bool usesBlendConstants, bool usesDepthBias)
{
	uint32 uses = 0;
	if (usesBlendConstants)
	{
		uses |= VULKAN_PIPELINE_USES_BLEND_CONSTANTS;
		pipelineInfo->usesBlendConstants = true;
	}
	if (usesDepthBias)
	{
		uses |= VULKAN_PIPELINE_USES_DEPTH_BIAS;
		pipelineInfo->usesDepthBias = true;
	}
	if (VulkanRenderer::GetInstance()->UseAttachmentFeedbackLoop())
		uses |= VULKAN_PIPELINE_FEEDBACK_LOOP_DYNAMIC;

	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = VulkanPipelineKey_DynamicStates(VulkanRenderer::draw_pipelineKeyDynamicFlags(), uses, dynamicStates);
	dynamicState.pDynamicStates = dynamicStates;
}

bool PipelineCompiler::InitFromCurrentGPUState(PipelineInfo* pipelineInfo, const LatteContextRegister& latteRegister, VKRObjectRenderPass* renderPassObj, bool requireRobustBufferAccess)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();

	// ##########################################################################################################################################
	bool isPrimitiveRect = false;
	const auto primitiveMode = latteRegister.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	isPrimitiveRect = (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS);

	m_fetchShader = pipelineInfo->fetchShader;
	m_vkVertexShader = pipelineInfo->vertexShaderVk;
	m_vkPixelShader = pipelineInfo->pixelShaderVk;
	m_vkGeometryShader = pipelineInfo->geometryShaderVk;
	m_vkrObjPipeline = pipelineInfo->m_vkrObjPipeline;
	m_renderPassObj = renderPassObj;
	m_requestRobustBufferAccess = requireRobustBufferAccess;
	const bool perPipeline = vkRenderer->m_featureControl.deviceExtensions.pipeline_robustness;
	m_robustVertex = perPipeline && CalcRobustBufferAccessRequirement(pipelineInfo->vertexShader, nullptr, pipelineInfo->geometryShader);
	m_robustPixel = perPipeline && CalcRobustBufferAccessRequirement(nullptr, pipelineInfo->pixelShader, nullptr);

	// if required generate RECT emulation geometry shader
	if (!vkRenderer->m_featureControl.deviceExtensions.nv_fill_rectangle && isPrimitiveRect)
	{
		cemu_assert(m_vkGeometryShader == nullptr); // todo - handle cases where the game already provides a GS
		const RectEmulationGS gs = rectsEmulationGS_generate(pipelineInfo->vertexShader, latteRegister);
		m_rectEmulationGS = gs.shader;
		m_rectEmulationKey = gs.sharedKey;
		// one of its own: the pipeline keeps it
		if (!gs.sharedKey)
			pipelineInfo->rectEmulationGS = gs.shader;
	}

	// ##########################################################################################################################################

	pipelineInfo->primitiveMode = primitiveMode;
	InitVertexInputState(latteRegister, pipelineInfo->vertexShader, pipelineInfo->fetchShader);
	InitInputAssemblyState(primitiveMode);
	InitViewportState();
	bool usesDepthBias = false;
	InitRasterizerState(latteRegister, vkRenderer, isPrimitiveRect, usesDepthBias);
	bool usesBlendConstants = false;
	InitBlendState(latteRegister, pipelineInfo, usesBlendConstants, renderPassObj);
	InitDescriptorSetLayouts(vkRenderer, pipelineInfo, pipelineInfo->vertexShader, pipelineInfo->pixelShader, pipelineInfo->geometryShader);

	// ##########################################################################################################################################

	// with graphics pipeline libraries every pipeline is linked from parts,
	// each with only its own shaders' sets: the layouts' sets are
	// independent (a layout's flags are part of what it is compatible with,
	// so no pipeline is made whole then)
	const bool libraries = vkRenderer->m_featureControl.deviceExtensions.graphics_pipeline_library;
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.flags = libraries ? VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT : 0;
	pipelineLayoutInfo.setLayoutCount = descriptorSetLayoutCount;
	pipelineLayoutInfo.pSetLayouts = descriptorSetLayout;
	pipelineLayoutInfo.pPushConstantRanges = nullptr;
	pipelineLayoutInfo.pushConstantRangeCount = 0;

	VkResult result = vkCreatePipelineLayout(vkRenderer->m_logicalDevice, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
	if (result != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create pipeline layout: {}", result);
		return false;
	}
	if (libraries)
	{
		// the vertex and geometry shaders' sets (0, 2), the pixel shader's (1)
		VkDescriptorSetLayout pre[3] = {
			descriptorSetLayoutCount > 0 ? descriptorSetLayout[0] : VK_NULL_HANDLE, VK_NULL_HANDLE,
			descriptorSetLayoutCount > 2 ? descriptorSetLayout[2] : VK_NULL_HANDLE};
		VkDescriptorSetLayout frag[2] = {VK_NULL_HANDLE, pipelineInfo->pixelShader ? descriptorSetLayout[1] : VK_NULL_HANDLE};
		VkPipelineLayoutCreateInfo partInfo = pipelineLayoutInfo;
		partInfo.setLayoutCount = (uint32)descriptorSetLayoutCount;
		partInfo.pSetLayouts = pre;
		result = vkCreatePipelineLayout(vkRenderer->m_logicalDevice, &partInfo, nullptr, &m_partLayout[VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION]);
		partInfo.setLayoutCount = pipelineInfo->pixelShader ? 2 : 0;
		partInfo.pSetLayouts = frag;
		if (result == VK_SUCCESS)
			result = vkCreatePipelineLayout(vkRenderer->m_logicalDevice, &partInfo, nullptr, &m_partLayout[VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER]);
		if (result != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "Failed to create pipeline library layout: {}", result);
			return false;
		}
	}

	// ###################################################

	InitDepthStencilState(latteRegister);

	// ##########################################################################################################################################

	InitDynamicState(pipelineInfo, usesBlendConstants, usesDepthBias);

	// ##########################################################################################################################################

	pipelineInfo->m_vkrObjPipeline->m_pipelineLayout = m_pipelineLayout;

	// increment ref counter for vkrObjPipeline and renderpass object to make sure they dont get released while we are using them
	m_vkrObjPipeline->incRef();
	m_renderPassObj->incRef();
	return true;
}

// cold, out of Compile: a failed pipeline's description is put together only
// here
void PipelineCompiler::LogCompileFailure(VkResult result, sint32 topology)
{
	// Which pipeline, so that a failure on one driver can be told apart
	// from the rest: the shaders by the hashes their dumps are named by,
	// and the render pass's formats
	auto shaderName = [](RendererShaderVk* shader) {
		return shader ? fmt::format("{:016x}_{:016x}", shader->GetBaseHash(), shader->GetAuxHash()) : std::string("-");
	};
	std::string colorFormats;
	for (size_t i = 0; m_renderPassObj && i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
		if (m_renderPassObj->m_colorAttachmentFormat[i] != VK_FORMAT_UNDEFINED)
			colorFormats += fmt::format("{}{}:{}", colorFormats.empty() ? "" : " ", i, (sint32)m_renderPassObj->m_colorAttachmentFormat[i]);
	// and the vertex attributes' formats, each marked "!" where the device
	// cannot fetch vertex data in it (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
	std::string vertexFormats;
	for (const auto& attr : std::span(vertexInputAttributeDescription, vertexInputInfo.vertexAttributeDescriptionCount))
	{
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(VulkanRenderer::GetInstance()->GetPhysicalDevice(), attr.format, &props);
		const bool supported = (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
		vertexFormats += fmt::format("{}{}:{}{}", vertexFormats.empty() ? "" : " ", attr.location, (sint32)attr.format, supported ? "" : "!");
	}
	cemuLog_log(LogType::Force, "Failed to create graphics pipeline. Error {} (VS {} GS {} PS {}, color formats [{}], depth format {}, topology {}, vertex formats [{}])",
		(sint32)result, shaderName(m_vkVertexShader), shaderName(m_vkGeometryShader), shaderName(m_vkPixelShader), colorFormats,
		m_renderPassObj ? (sint32)m_renderPassObj->m_depthAttachmentFormat : -1,
		topology, vertexFormats);
}

// A shader a waiting draw needs compiled: by a compile thread, or by the
// thread taking it (PreparedPipeline::BuildTogether); PreponeCompilation
// compiles it there unless the shader's own compile thread has it already,
// and waits for that one if so
struct ShaderCompileTogether
{
	RendererShaderVk* shader{};
	bool callerIsRenderThread{};
	// last: retired first
	PreparedPipeline prepared{[](void* self, bool onCompileThread) {
		ShaderCompileTogether* j = (ShaderCompileTogether*)self;
		if (!j->shader->IsCompiled())
			j->shader->PreponeCompilation(!onCompileThread && j->callerIsRenderThread);
		return (VkPipeline)VK_NULL_HANDLE;
	}, this};
};

bool PipelineCompiler::Compile(bool forceCompile, bool isRenderThread, bool showInOverlay)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();

	// a try where the driver's cache cannot be asked without compiling takes
	// only parts already made - or the pipeline, known to be in that cache:
	// never a compile on the draw path
	const VulkanPipelineTry mode = VulkanPipeline_Try(forceCompile, vkRenderer->m_featureControl.deviceExtensions.pipeline_creation_cache_control, m_driverCached);
	if (mode == VulkanPipelineTry::Made && !vkRenderer->m_featureControl.deviceExtensions.graphics_pipeline_library)
		return false;

	if (!forceCompile)
	{
		// fail early if some shader stages are not compiled
		if (m_vkVertexShader && m_vkVertexShader->IsCompiled() == false)
			return false;
		if (m_vkPixelShader && m_vkPixelShader->IsCompiled() == false)
			return false;
		if (m_vkGeometryShader && m_vkGeometryShader->IsCompiled() == false)
			return false;
		if (m_rectEmulationGS && m_rectEmulationGS->IsCompiled() == false)
			return false;
	}
	else
	{
		// if some shader stages are not compiled yet, compile them now: for
		// a draw that waits, together (the first here, the others offered
		// to the compile threads meanwhile), else one after the other
		RendererShaderVk* const stages[4] = { m_vkVertexShader, m_vkPixelShader, m_vkGeometryShader, m_rectEmulationGS };
		ShaderCompileTogether jobs[4];
		PreparedPipeline* here[1];
		PreparedPipeline* queued[3];
		size_t count = 0;
		for (RendererShaderVk* shader : stages)
		{
			if (!shader || shader->IsCompiled())
				continue;
			jobs[count].shader = shader;
			jobs[count].callerIsRenderThread = isRenderThread;
			count++;
		}
		if (count > 1 && (isRenderThread || m_drawWaits))
		{
			here[0] = &jobs[0].prepared;
			for (size_t i = 1; i < count; i++)
				queued[i - 1] = &jobs[i].prepared;
			PreparedPipeline::BuildTogether(here, 1, queued, count - 1, [](PreparedPipeline& item) { CompileThreadPool_QueuePrepared(item); });
		}
		else
		{
			for (size_t i = 0; i < count; i++)
				jobs[i].shader->PreponeCompilation(isRenderThread);
		}
	}

	if (shaderStageCount == 0)
	{
		if (!InitShaderStages(vkRenderer, m_vkVertexShader, m_vkPixelShader, m_vkGeometryShader))
			return true; // invalid shaders, cannot compile
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = shaderStageCount;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.layout = m_pipelineLayout;
	pipelineInfo.renderPass = m_renderPassObj->m_renderPass;
	pipelineInfo.pDepthStencilState = &depthStencilState;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = nullptr;
	pipelineInfo.flags = 0;
	if (mode == VulkanPipelineTry::FromCache)
		pipelineInfo.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;

	void* prevStruct = nullptr;

	VkPipelineRobustnessCreateInfoEXT pipelineRobustnessCreateInfo{};
	if (vkRenderer->m_featureControl.deviceExtensions.pipeline_robustness && m_requestRobustBufferAccess)
	{
		// per-pipeline handling of robust buffer access, if the extension is not available then we fall back to device feature robustBufferAccess
		pipelineRobustnessCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT;
		pipelineRobustnessCreateInfo.pNext = prevStruct;
		prevStruct = &pipelineRobustnessCreateInfo;
		pipelineRobustnessCreateInfo.storageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
		pipelineRobustnessCreateInfo.uniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
		pipelineRobustnessCreateInfo.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
		pipelineRobustnessCreateInfo.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT;
	}

	pipelineInfo.pNext = prevStruct;

	if (vkRenderer->m_featureControl.deviceExtensions.graphics_pipeline_library)
		return CompileFromLibraries(pipelineInfo, mode == VulkanPipelineTry::Compile, mode == VulkanPipelineTry::Made, isRenderThread);

	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult result;
	uint8 retryCount = 0;
	while (retryCount < 3)
	{
		// marked as under way for the cache's save, which waits for a moment
		// none is; nothing here waits for it
		vkRenderer->m_pipeline_cache_builds.Enter();
		result = vkCreateGraphicsPipelines(vkRenderer->m_logicalDevice, vkRenderer->m_pipeline_cache, 1, &pipelineInfo, nullptr, &pipeline);
		vkRenderer->m_pipeline_cache_builds.Leave();
		if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
			break;
		retryCount++;
	}

	if (result == VK_ERROR_PIPELINE_COMPILE_REQUIRED_EXT)
	{
		return false;
	}
	else if (result == VK_SUCCESS)
	{
		m_vkrObjPipeline->SetPipeline(pipeline);
	}
	else
	{
		LogCompileFailure(result, pipelineInfo.pInputAssemblyState ? (sint32)pipelineInfo.pInputAssemblyState->topology : -1);
		cemu_assert_debug(false);
		return true; // true indicates that caller should no longer attempt to compile this pipeline again
	}
	vkRenderer->m_pipeline_cache_semaphore.notify();

	return true;
}

// pipelines the compile threads linked optimized, for the GPU thread to put
// in the fast-linked ones' place
static MpmcQueue<PipelineCompiler::OptimizedPipeline> s_optimizedPipelines;

// pipelines made ahead: handed over by the compile threads, kept by the GPU
// thread for their draws. Never bound until a draw takes one, so one given
// up is destroyed at once, its layouts with it
struct AheadPipeline
{
	uint64 hash;
	VKRObjectPipeline* pipeline;
};
static MpmcQueue<AheadPipeline> s_aheadPipelines;
// as many as the ring holds before it would allocate past it, with room
static constexpr size_t kAheadHandedMax = 512;
static KeptForDraws<VKRObjectPipeline*, 64> s_keptPipelines;

void PipelineCompiler::HandOverAhead(uint64 pipelineHash, VKRObjectPipeline* pipeline)
{
	if (s_aheadPipelines.size() >= kAheadHandedMax)
	{
		delete pipeline;
		return;
	}
	s_aheadPipelines.push({pipelineHash, pipeline});
}

VKRObjectPipeline* PipelineCompiler::TakeAhead(uint64 pipelineHash)
{
	AheadPipeline handed;
	while (s_aheadPipelines.peek2(handed))
	{
		VKRObjectPipeline* gone;
		if (s_keptPipelines.Put(handed.hash, handed.pipeline, &gone))
			delete gone;
	}
	VKRObjectPipeline* pipeline = nullptr;
	s_keptPipelines.Take(pipelineHash, &pipeline);
	return pipeline;
}

void PipelineCompiler::DropAhead()
{
	AheadPipeline handed;
	while (s_aheadPipelines.peek2(handed))
		delete handed.pipeline;
	s_keptPipelines.Clear([](VKRObjectPipeline* p) { delete p; });
}

bool PipelineCompiler::TakeOptimizedPipeline(OptimizedPipeline& result)
{
	return s_optimizedPipelines.peek2(result);
}

// The pipeline from its four parts (VulkanPipelineLibrary.h): each found
// made, or made here and kept for the pipelines after it, then linked.
// Without forceCompile nothing is compiled: a part not made yet - or only
// made by the driver's cache - leaves it to a compile thread (false); with
// it a part another thread is making is waited for, not made twice. When a
// draw waits for it - on the GPU thread, or a draw skipped until it lands -
// the parts are linked as they are, a link and no compile, and a compile
// thread links them optimized, which then takes its place; otherwise they
// are linked optimized at once.
// What a library part chains for robust buffer access: robust, the
// extension's struct; else nothing
static const void* PartRobustness(bool robust)
{
	static const VkPipelineRobustnessCreateInfoEXT s_robust{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT, nullptr,
		VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT, VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT,
		VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT, VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT};
	return robust ? &s_robust : nullptr;
}

VkPipeline PipelineCompiler::MakeLibraryPart(int p, uint64 key, bool claimed, const VkGraphicsPipelineCreateInfo& full, bool forceCompile, bool isRenderThread, VkResult& result)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	VulkanPipelineLibraryCache& cache = *vkRenderer->m_pipelineLibraries;
	const VulkanPipelineLibraryPart part = (VulkanPipelineLibraryPart)p;
	// made here: under the claim, or adopted by the cache when the key is
	// another thread's (a pipeline needs its parts for as long as it lives);
	// without forceCompile only out of the driver's cache
	VkGraphicsPipelineCreateInfo ci = full;
	// robust buffer access for the part's own shaders alone (all the
	// pipeline's create info chains)
	ci.pNext = PartRobustness(p == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION ? m_robustVertex :
		p == VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER ? m_robustPixel : false);
	if (!forceCompile)
		ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
	else
		ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
	PSO_TRACE_SCOPE(isRenderThread ? (forceCompile ? "pipeline library sync" : "pipeline library from cache") : "pipeline library", key);
	VkPipeline library = VK_NULL_HANDLE;
	vkRenderer->m_pipeline_cache_builds.Enter();
	result = VulkanPipelineLibrary_Create(vkRenderer->m_logicalDevice, vkRenderer->m_pipeline_cache, part, ci, m_partLayout[p], &library);
	vkRenderer->m_pipeline_cache_builds.Leave();
	if (result != VK_SUCCESS)
		library = VK_NULL_HANDLE;
	if (claimed)
		cache.Publish(part, key, library);
	else if (library != VK_NULL_HANDLE)
		cache.Adopt(library);
	return library;
}

namespace
{
	// a part a waiting draw needs, made by a compile thread or by the GPU
	// thread taking it (PreparedPipeline::BuildTogether)
	struct LibraryPartBuild
	{
		PipelineCompiler* compiler;
		const VkGraphicsPipelineCreateInfo* full;
		int part;
		uint64 key;
		bool callerIsRenderThread;
		VkResult result{VK_SUCCESS};
		// last: retired first
		PreparedPipeline prepared{[](void* self, bool onCompileThread) {
			LibraryPartBuild* b = (LibraryPartBuild*)self;
			return b->compiler->MakeLibraryPart(b->part, b->key, true, *b->full, true, !onCompileThread && b->callerIsRenderThread, b->result);
		}, this};
	};
}

// The parts of a pipeline a draw waits for (on the GPU thread, or on a
// compile thread for a draw skipped until it lands): those made already
// taken, those another thread makes waited for, and the missing
// ones claimed and made together - the shader parts (the costly ones) but
// the first queued for the compile threads while this thread makes the
// first, then each taken - where they were made one after the other. False
// when one could not be made.
bool PipelineCompiler::GatherLibraryPartsTogether(const VkGraphicsPipelineCreateInfo& full, const uint64 keys[], VkPipeline libraries[], bool isRenderThread)
{
	VulkanPipelineLibraryCache& cache = *VulkanRenderer::GetInstance()->m_pipelineLibraries;
	bool pending[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	int toMake[VULKAN_PIPELINE_LIBRARY_PARTS];
	int makeCount = 0;
	for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
	{
		const VulkanPipelineLibraryPart part = (VulkanPipelineLibraryPart)p;
		VkPipeline library = VK_NULL_HANDLE;
		const VulkanPipelineLibraryCache::State state = cache.Find(part, keys[p], library);
		if (state == VulkanPipelineLibraryCache::State::Ready)
			libraries[p] = library;
		else if (state == VulkanPipelineLibraryCache::State::Missing && cache.Claim(part, keys[p]))
			toMake[makeCount++] = p;
		else
			pending[p] = true; // another thread's
	}
	// the costly ones (the shader parts) but the first to the compile
	// threads; the first and the cheap ones made here meanwhile
	std::stable_partition(toMake, toMake + makeCount, [](int p) {
		return p == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION || p == VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER;
	});
	int costly = 0;
	while (costly < makeCount && (toMake[costly] == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION || toMake[costly] == VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER))
		costly++;
	LibraryPartBuild builds[VULKAN_PIPELINE_LIBRARY_PARTS];
	PreparedPipeline* here[VULKAN_PIPELINE_LIBRARY_PARTS];
	PreparedPipeline* queued[VULKAN_PIPELINE_LIBRARY_PARTS];
	size_t hereCount = 0, queuedCount = 0;
	for (int i = 0; i < makeCount; i++)
	{
		builds[i].compiler = this;
		builds[i].full = &full;
		builds[i].part = toMake[i];
		builds[i].key = keys[toMake[i]];
		builds[i].callerIsRenderThread = isRenderThread;
		if (i > 0 && i < costly)
			queued[queuedCount++] = &builds[i].prepared;
		else
			here[hereCount++] = &builds[i].prepared;
	}
	// helpers in the lane of the pipeline they are for: a pipeline made
	// ahead's never before a draw's (one no thread took is made here)
	PreparedPipeline::BuildTogether(here, hereCount, queued, queuedCount, [ahead = m_madeAhead](PreparedPipeline& item) {
		if (ahead)
			CompileThreadPool_QueuePreparedAhead(item);
		else
			CompileThreadPool_QueuePrepared(item);
	});
	bool ok = true;
	for (int i = 0; i < makeCount; i++)
	{
		VkPipeline library = builds[i].prepared.Take();
		if (library == VK_NULL_HANDLE)
		{
			cemuLog_log(LogType::Force, "Failed to create graphics pipeline library part {}: {}", builds[i].part, (sint32)builds[i].result);
			ok = false;
		}
		libraries[builds[i].part] = library;
	}
	for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
	{
		if (!pending[p])
			continue;
		PSO_TRACE_SCOPE(isRenderThread ? "pipeline library wait" : "pipeline library wait (worker)", keys[p]);
		VkPipeline library = VK_NULL_HANDLE;
		if (cache.WaitFor((VulkanPipelineLibraryPart)p, keys[p], library) != VulkanPipelineLibraryCache::State::Ready)
		{
			// failed on the thread that claimed it: made here, as before
			VkResult result;
			library = MakeLibraryPart(p, keys[p], cache.Claim((VulkanPipelineLibraryPart)p, keys[p]), full, true, isRenderThread, result);
			if (library == VK_NULL_HANDLE)
			{
				cemuLog_log(LogType::Force, "Failed to create graphics pipeline library part {}: {}", p, (sint32)result);
				ok = false;
			}
		}
		libraries[p] = library;
	}
	return ok;
}

bool PipelineCompiler::CompileFromLibraries(const VkGraphicsPipelineCreateInfo& full, bool forceCompile, bool madeOnly, bool isRenderThread)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	VulkanPipelineLibraryCache& cache = *vkRenderer->m_pipelineLibraries;
	const VkDevice device = vkRenderer->m_logicalDevice;
	auto shaderHash = [](RendererShaderVk* shader) {
		return shader ? shader->GetBaseHash() * 0x9E3779B97F4A7C15ull + shader->GetAuxHash() + 1 : 0;
	};
	VulkanPipelineLibraryIdentity identity{};
	identity.vertexShader = shaderHash(m_vkVertexShader);
	// a shared rect emulation geometry shader goes by its source's key; one
	// made for this pipeline alone, under a number no other part has (one per
	// compiler, so that a compile thread's try finds the GPU thread's)
	static std::atomic<uint64> s_rectEmulationSerial{0};
	if (m_rectEmulationGS && m_rectEmulationSerial == 0)
		m_rectEmulationSerial = m_rectEmulationKey ? m_rectEmulationKey : (1ull << 63) | s_rectEmulationSerial.fetch_add(1, std::memory_order_relaxed);
	identity.geometryShader = m_vkGeometryShader ? shaderHash(m_vkGeometryShader) : m_rectEmulationSerial;
	identity.pixelShader = shaderHash(m_vkPixelShader);
	identity.renderPass = m_renderPassObj->m_hashForPipeline;
	identity.robustVertex = m_robustVertex;
	identity.robustPixel = m_robustPixel;

	VkPipeline libraries[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	bool ok = true;
	if (forceCompile && (isRenderThread || m_drawWaits))
	{
		// a draw waits - on the GPU thread, or skipped until a compile thread
		// lands it: what is missing made at once, not one after the other
		uint64 keys[VULKAN_PIPELINE_LIBRARY_PARTS];
		for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS; p++)
			keys[p] = VulkanPipelineLibrary_Key((VulkanPipelineLibraryPart)p, full, identity);
		if (!GatherLibraryPartsTogether(full, keys, libraries, isRenderThread))
			return true; // a part failed: no use trying again
	}
	else for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS && ok; p++)
	{
		const VulkanPipelineLibraryPart part = (VulkanPipelineLibraryPart)p;
		const uint64 key = VulkanPipelineLibrary_Key(part, full, identity);
		VkPipeline library = VK_NULL_HANDLE;
		VulkanPipelineLibraryCache::State state = cache.Find(part, key, library);
		if (state == VulkanPipelineLibraryCache::State::Pending && forceCompile)
		{
			// another thread is making it: waited for, not made twice
			PSO_TRACE_SCOPE(isRenderThread ? "pipeline library wait" : "pipeline library wait (worker)", key);
			state = cache.WaitFor(part, key, library);
		}
		if (state == VulkanPipelineLibraryCache::State::Ready)
		{
			libraries[p] = library;
			continue;
		}
		if (madeOnly)
		{
			ok = false; // not made, and the driver's cache not to be asked
			break;
		}
		const bool claimed = state == VulkanPipelineLibraryCache::State::Missing && cache.Claim(part, key);
		if (!forceCompile && !claimed)
		{
			ok = false; // being made elsewhere
			break;
		}
		VkResult result;
		library = MakeLibraryPart(p, key, claimed, full, forceCompile, isRenderThread, result);
		if (library == VK_NULL_HANDLE)
		{
			if (result != VK_PIPELINE_COMPILE_REQUIRED_EXT)
			{
				cemuLog_log(LogType::Force, "Failed to create graphics pipeline library part {}: {}", p, (sint32)result);
				return true; // no use trying again
			}
			ok = false;
			break;
		}
		libraries[p] = library;
	}
	VkPipeline pipeline = VK_NULL_HANDLE;
	bool optimize = false, queueOptimized = false;
	if (ok)
	{
		// linked as they are when a draw waits for it, optimized after
		optimize = VulkanPipelineLibrary_LinkOptimized(isRenderThread || m_drawWaits, &queueOptimized);
		PSO_TRACE_SCOPE(optimize ? "pipeline link optimized" : "pipeline fast link", (uint64)m_vkrObjPipeline);
		vkRenderer->m_pipeline_cache_builds.Enter();
		const VkResult result = VulkanPipelineLibrary_Link(device, vkRenderer->m_pipeline_cache, libraries, m_pipelineLayout, optimize, &pipeline);
		vkRenderer->m_pipeline_cache_builds.Leave();
		if (result != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "Failed to link graphics pipeline libraries: {}", (sint32)result);
			pipeline = VK_NULL_HANDLE;
		}
	}
	if (!ok)
		return false;
	if (pipeline == VK_NULL_HANDLE)
		return true;
	m_vkrObjPipeline->SetPipeline(pipeline);
	vkRenderer->m_pipeline_cache_semaphore.notify();
	if (queueOptimized)
	{
		// the optimized one, from the same parts, on a compile thread
		PipelineCompiler* job = new PipelineCompiler();
		job->m_linkOptimizedOnly = true;
		job->m_vkrObjPipeline = m_vkrObjPipeline;
		m_vkrObjPipeline->incRef();
		std::copy(std::begin(libraries), std::end(libraries), job->m_linkLibraries);
		job->m_pipelineLayout = m_pipelineLayout;
		CompileThreadPool_QueueCompilation(job);
	}
	return true;
}

// A shader's parts made ahead (docs/SHADER-STUTTER.md M2): its
// pre-rasterization part (a vertex shader, no geometry shader) or fragment
// shader part for each template the GPU thread had seen when it first set up
// a pipeline with it - the render passes and state other than that
// pipeline's - so that the shader's first pipeline with one of them links
// what is made rather than compiling. Built once, by a compile thread, from
// what the GPU thread took for it; owned by the shader.
struct AheadParts : PoolNew<AheadParts, 64>
{
	RendererShaderVk* shader;
	VulkanPipelineLibraryPart part;
	uint64 shaderHash;
	bool robust;
	uint64 slots; // the templates', held until built or retired
	VulkanDescriptorBindings bindings;
	// last, so that it is retired first
	PreparedPipeline prepared{&AheadParts::Build, this};

	static VkPipeline Build(void* self, bool onCompileThread)
	{
		PipelineCompiler::BuildAheadParts((AheadParts*)self, onCompileThread);
		return VK_NULL_HANDLE;
	}
};

void PipelineCompiler::BuildAheadParts(AheadParts* a, bool onCompileThread)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	VulkanPipelinePartTemplates& templates = *vkRenderer->m_partTemplates;
	const VkDevice device = vkRenderer->m_logicalDevice;
	if (!a->shader->IsCompiled())
		a->shader->PreponeCompilation(!onCompileThread);
	VkShaderModule module = a->shader->GetShaderModule();
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	if (module != VK_NULL_HANDLE)
	{
		VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		setInfo.bindingCount = a->bindings.count;
		setInfo.pBindings = a->bindings.bindings;
		// the sets as the pipeline compiler lays them out: the vertex
		// shader's 0, the pixel shader's 1
		const bool pre = a->part == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION;
		VkDescriptorSetLayout sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layoutInfo.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
		layoutInfo.setLayoutCount = 2;
		layoutInfo.pSetLayouts = sets;
		if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout) == VK_SUCCESS)
		{
			sets[pre ? 0 : 1] = setLayout;
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
				layout = VK_NULL_HANDLE;
		}
	}
	if (layout != VK_NULL_HANDLE)
	{
		const void* next = PartRobustness(a->robust);
		const VkPipelineShaderStageCreateInfo stage = vkRenderer->CreatePipelineShaderStageCreateInfo(
			a->part == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, module, "main");
		VulkanPipelineLibraryCache& cache = *vkRenderer->m_pipelineLibraries;
		for (uint64 slots = a->slots; slots; slots &= slots - 1)
		{
			const VulkanPipelinePartTemplate& t = templates.Get((uint32)std::countr_zero(slots));
			VulkanPipelinePartAhead ahead;
			VulkanPipelineLibrary_FromTemplate(t, &stage, next, ahead);
			VulkanPipelineLibraryIdentity identity{};
			if (a->part == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION)
			{
				identity.vertexShader = a->shaderHash;
				identity.robustVertex = a->robust;
			}
			else
			{
				identity.pixelShader = a->shaderHash;
				identity.robustPixel = a->robust;
			}
			identity.renderPass = t.renderPassHash;
			const uint64 key = VulkanPipelineLibrary_Key(a->part, ahead.full, identity);
			VkPipeline library = VK_NULL_HANDLE;
			if (cache.Find(a->part, key, library) != VulkanPipelineLibraryCache::State::Missing || !cache.Claim(a->part, key))
				continue;
			PSO_TRACE_SCOPE("pipeline library ahead", key);
			vkRenderer->m_pipeline_cache_builds.Enter();
			const VkResult result = VulkanPipelineLibrary_Create(device, vkRenderer->m_pipeline_cache, a->part, ahead.full, layout, &library);
			vkRenderer->m_pipeline_cache_builds.Leave();
			cache.Publish(a->part, key, result == VK_SUCCESS ? library : VK_NULL_HANDLE);
		}
		vkDestroyPipelineLayout(device, layout, nullptr);
	}
	if (setLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
	templates.Drop(a->slots);
	a->slots = 0;
}

void PipelineCompiler::RetireAheadParts(AheadParts* ahead)
{
	ahead->prepared.Retire();
	if (!ahead->prepared.IsBuilt())
		VulkanRenderer::GetInstance()->m_partTemplates->Drop(ahead->slots);
	delete ahead;
}

void PipelineCompiler::PrepareAheadParts(PipelineInfo* pipelineInfo)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	VulkanPipelinePartTemplates& templates = *vkRenderer->m_partTemplates;
	// this pipeline's state, kept: what its parts are made from, without
	// the shaders
	VkGraphicsPipelineCreateInfo full{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	full.pViewportState = &viewportState;
	full.pRasterizationState = &rasterizer;
	full.pMultisampleState = &multisampling;
	full.pDepthStencilState = &depthStencilState;
	full.pDynamicState = &dynamicState;
	full.renderPass = m_renderPassObj->m_renderPass;
	const VulkanPipelineLibraryPart parts[2] = {VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER};
	uint64 keys[2] = {0, 0};
	for (int i = 0; i < 2; i++)
	{
		VulkanPipelinePartTemplate t;
		if (!VulkanPipelineLibrary_Template(parts[i], full, m_renderPassObj->m_hashForPipeline, t))
			continue;
		keys[i] = t.key;
		templates.Record(t, m_renderPassObj);
	}
	// a shader's first pipeline: its parts for the other templates
	auto queue = [&](RendererShaderVk* shader, LatteDecompilerShader* decompiled, VulkanPipelineLibraryPart part, uint64 skipKey,
		VkShaderStageFlags stage) {
		if (!shader || !decompiled || shader->m_aheadParts)
			return;
		const uint64 slots = templates.Take(part, skipKey);
		if (!slots)
			return;
		AheadParts* ahead = new AheadParts();
		ahead->shader = shader;
		ahead->part = part;
		ahead->shaderHash = shader->GetBaseHash() * 0x9E3779B97F4A7C15ull + shader->GetAuxHash() + 1;
		ahead->robust = part == VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION ? m_robustVertex : m_robustPixel;
		ahead->slots = slots;
		VulkanPipelineKey_DescriptorBindings(decompiled->resourceMapping, stage, ahead->bindings);
		shader->m_aheadParts = ahead;
		CompileThreadPool_QueuePreparedAhead(ahead->prepared);
	};
	if (!m_vkGeometryShader && !m_rectEmulationGS)
		queue(m_vkVertexShader, pipelineInfo->vertexShader, VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION, keys[0], VK_SHADER_STAGE_VERTEX_BIT);
	queue(m_vkPixelShader, pipelineInfo->pixelShader, VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER, keys[1], VK_SHADER_STAGE_FRAGMENT_BIT);
}

bool PipelineCompiler::PrepareAheadPartsFor(RendererShaderVk* shader, LatteDecompilerShader* decompiled, bool robust)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	if (!vkRenderer->m_partTemplates || !shader || !decompiled || shader->m_aheadParts)
		return false;
	const bool vertex = decompiled->shaderType == LatteConst::ShaderType::Vertex;
	const VulkanPipelineLibraryPart part = vertex ? VULKAN_PIPELINE_LIBRARY_PRE_RASTERIZATION : VULKAN_PIPELINE_LIBRARY_FRAGMENT_SHADER;
	const uint64 slots = vkRenderer->m_partTemplates->Take(part, 0);
	if (!slots)
		return false;
	AheadParts* ahead = new AheadParts();
	ahead->shader = shader;
	ahead->part = part;
	ahead->shaderHash = shader->GetBaseHash() * 0x9E3779B97F4A7C15ull + shader->GetAuxHash() + 1;
	// its own stage's, as its pipelines' parts take it
	ahead->robust = robust && vkRenderer->m_featureControl.deviceExtensions.pipeline_robustness;
	ahead->slots = slots;
	VulkanPipelineKey_DescriptorBindings(decompiled->resourceMapping, vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, ahead->bindings);
	shader->m_aheadParts = ahead;
	CompileThreadPool_QueuePreparedAhead(ahead->prepared);
	return true;
}

// a compile thread's: the parts linked optimized, handed to the GPU thread
bool PipelineCompiler::LinkOptimized()
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	VkPipeline pipeline = VK_NULL_HANDLE;
	vkRenderer->m_pipeline_cache_builds.Enter();
	const VkResult result = VulkanPipelineLibrary_Link(vkRenderer->m_logicalDevice, vkRenderer->m_pipeline_cache, m_linkLibraries,
		m_pipelineLayout, true, &pipeline);
	vkRenderer->m_pipeline_cache_builds.Leave();
	if (result != VK_SUCCESS)
		return true; // the fast-linked one stays
	m_vkrObjPipeline->incRef();
	s_optimizedPipelines.push({m_vkrObjPipeline, pipeline});
	vkRenderer->m_pipeline_cache_semaphore.notify();
	return true;
}

void PipelineCompiler::TrackAsCached(uint64 baseHash, uint64 pipelineStateHash)
{
	auto& pipelineCache = VulkanPipelineStableCache::GetInstance();
	if (pipelineCache.HasPipelineCached(baseHash, pipelineStateHash))
		return;
	pipelineCache.AddCurrentStateToCache(baseHash, pipelineStateHash);
}

// calculate whether the pipeline requires robust buffer access
// if there is a potential risk for a shader to do out-of-bounds reads or writes we need to enable robust buffer access
// this can happen when:
// - Streamout is used with too small of a buffer (probably? Could also be some issue with how the streamout array index is calculated -> We can maybe fix this in the future)
// - The shader uses dynamic indices for uniform access. This will trigger the uniform mode to be FULL_CBANK
bool PipelineCompiler::CalcRobustBufferAccessRequirement(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader)
{
	bool requiresRobustBufferAcces = false;
	if (vertexShader)
	{
		cemu_assert_debug(vertexShader->shaderType == LatteConst::ShaderType::Vertex);
		requiresRobustBufferAcces |= vertexShader->hasStreamoutBufferWrite;
		requiresRobustBufferAcces |= vertexShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
	}
	if (geometryShader)
	{
		cemu_assert_debug(geometryShader->shaderType == LatteConst::ShaderType::Geometry);
		requiresRobustBufferAcces |= geometryShader->hasStreamoutBufferWrite;
		requiresRobustBufferAcces |= geometryShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
	}
	if (pixelShader)
	{
		cemu_assert_debug(pixelShader->shaderType == LatteConst::ShaderType::Pixel);
		requiresRobustBufferAcces |= pixelShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
	}
	return requiresRobustBufferAcces;
}

static std::vector<std::thread> s_compileThreads;
static std::atomic_bool s_compileThreadsShutdownSignal{};
// Three lanes, pushed by any thread and taken by every compile thread, no
// lock: what is worth nothing late (the reads ahead of the GPU thread,
// LatteShaderAhead.cpp); then what a draw needs now (a skipped draw's
// pipeline, a waiting draw's parts and shaders, a surface copy's pipeline);
// then what only a later draw may need, or none (parts and pipelines made
// ahead, optimized links), which the threads wait on: each item for the
// lanes before it is pushed with a null into this one, to wake a thread
static MpmcQueue<PipelineCompiler*> s_urgentCompileRequests;
static MpmcQueue<PipelineCompiler*> s_pipelineCompileRequests;
static MpmcQueue<PipelineCompiler*> s_aheadCompileRequests;

static void compilePipeline_thread(sint32 threadIndex)
{
	SetThreadName("compilePl");
	// the decompiles the GPU thread hands over, sized now rather than in play
	LatteDecompiler_PrepareThread();
#ifdef _WIN32
	// to avoid starving the main cpu and render threads the pipeline compile threads run at lower priority
	// except for one thread which we always run at normal priority to prevent the opposite scenario where all compile threads are starved
	if(threadIndex != 0)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
	while (!s_compileThreadsShutdownSignal)
	{
		PipelineCompiler* request = MpmcQueue_PopPreferring(s_urgentCompileRequests, s_pipelineCompileRequests, s_aheadCompileRequests);
		if (!request)
			continue; // an urgent one's wake (taken above, now or by another thread), or the end
		if (request->m_preparedTicket)
		{
			PreparedPipeline::Run(request->m_preparedTicket);
			request->m_preparedTicket = nullptr;
		}
		else if (request->m_linkOptimizedOnly)
			request->LinkOptimized();
		else
			request->Compile(true, false, true);
		delete request;
	}
}

void PipelineCompiler::CompileThreadPool_Start()
{
	cemu_assert_debug(s_compileThreads.empty());
	s_compileThreadsShutdownSignal = false;
	uint32 numCompileThreads;

	uint32 cpuCoreCount = GetPhysicalCoreCount();
	if (cpuCoreCount <= 2)
		numCompileThreads = 1;
	else
		numCompileThreads = 2 + (cpuCoreCount - 3); // 2 plus one additionally for every extra core above 3

	numCompileThreads = std::min(numCompileThreads, 8u); // cap at 8

	for (uint32_t i = 0; i < numCompileThreads; i++)
	{
		s_compileThreads.emplace_back(compilePipeline_thread, i);
	}
}

void PipelineCompiler::CompileThreadPool_Stop()
{
	s_compileThreadsShutdownSignal = true;
	{
		// push one empty workload for each thread
		// this way we can make sure that each waiting thread is woken up to see the shutdown signal
		for (auto& thread : s_compileThreads)
			s_aheadCompileRequests.push(nullptr);
	}
	for (auto& thread : s_compileThreads)
		thread.join();
	for (MpmcQueue<PipelineCompiler*>* queue : {&s_urgentCompileRequests, &s_pipelineCompileRequests, &s_aheadCompileRequests})
	{
		PipelineCompiler* pipelineCompiler;
		while (queue->peek2(pipelineCompiler))
			delete pipelineCompiler; // a ticket unrun dropped with it
	}
	s_compileThreads.clear();
}

void PipelineCompiler::CompileThreadPool_QueueCompilation(PipelineCompiler* v)
{
	s_aheadCompileRequests.push(v);
}

void PipelineCompiler::CompileThreadPool_QueuePrepared(PreparedPipeline& prepared)
{
	PipelineCompiler* job = new PipelineCompiler();
	job->m_preparedTicket = prepared.Queue();
	MpmcQueue_PushUrgent(s_pipelineCompileRequests, s_aheadCompileRequests, job);
}

void PipelineCompiler::CompileThreadPool_QueuePreparedAhead(PreparedPipeline& prepared)
{
	PipelineCompiler* job = new PipelineCompiler();
	job->m_preparedTicket = prepared.Queue();
	s_aheadCompileRequests.push(job);
}

void PipelineCompiler::CompileThreadPool_QueuePreparedUrgent(PreparedPipeline& prepared)
{
	PipelineCompiler* job = new PipelineCompiler();
	job->m_preparedTicket = prepared.Queue();
	MpmcQueue_PushUrgent(s_urgentCompileRequests, s_aheadCompileRequests, job);
}
