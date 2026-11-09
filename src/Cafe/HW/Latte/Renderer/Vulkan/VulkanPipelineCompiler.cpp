#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#include "Cafe/HW/Latte/Core/PsoTrace.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "util/helpers/Serializer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "HW/Latte/Renderer/RendererCore.h"

RendererShaderVk* rectsEmulationGS_generate(LatteDecompilerShader* vertexShader, const LatteContextRegister& latteRegister)
{
	std::string gsSrc;

	gsSrc.append("#version 450\r\n");

	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();

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

			gsSrc.append(fmt::format("layout(location = {}) ", psInputTable->getPSImportLocationBySemanticId(vsSemanticId)));
			if (psImport->isFlat)
				gsSrc.append("flat ");
			if (psImport->isNoPerspective)
				gsSrc.append("noperspective ");

			if (f == 0)
				gsSrc.append("in");
			else
				gsSrc.append("out");

			if (f == 0)
				gsSrc.append(fmt::format(" vec4 passParameterSem{}In[];\r\n", vsSemanticId));
			else
				gsSrc.append(fmt::format(" vec4 passParameterSem{}Out;\r\n", vsSemanticId));
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

	auto vkShader = new RendererShaderVk(RendererShader::ShaderType::kGeometry, 0, 0, false, false, gsSrc);
	vkShader->PreponeCompilation(true);
	return vkShader;
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
	// create vertex shader descriptor set
	std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings;

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
	// attributes
	// -> not part of descriptor

	// textures
	sint32 textureBindingBase = shader->resourceMapping.getTextureBaseBindingPoint();
	if (textureBindingBase >= 0)
	{
		sint32 textureCount = shader->resourceMapping.getTextureCount();
		for (sint32 i = 0; i < textureCount; i++)
		{
			VkDescriptorSetLayoutBinding entry{};
			entry.binding = (uint32)textureBindingBase + i;
			entry.descriptorCount = 1;
			entry.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			entry.pImmutableSamplers = nullptr;
			entry.stageFlags = stageFlags;
			descriptorSetLayoutBindings.emplace_back(entry);
		}
	}

	// uniform buffers
	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
	{
		VkDescriptorSetLayoutBinding entry{};
		entry.binding = shader->resourceMapping.uniformVarsBufferBindingPoint;
		entry.descriptorCount = 1;
		entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		entry.pImmutableSamplers = nullptr;
		entry.stageFlags = stageFlags;
		descriptorSetLayoutBindings.emplace_back(entry);
	}

	for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (shader->resourceMapping.uniformBuffersBindingPoint[i] >= 0)
		{
			VkDescriptorSetLayoutBinding entry{};
			entry.binding = shader->resourceMapping.uniformBuffersBindingPoint[i];
			entry.descriptorCount = 1;
			entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			entry.pImmutableSamplers = nullptr;
			entry.stageFlags = stageFlags;
			descriptorSetLayoutBindings.emplace_back(entry);

			vkrPipelineInfo->dynamicOffsetInfo.list_uniformBuffers[stageIndex].emplace_back((uint8)i);
		}
	}

	// storage buffer for TF
	if (shader->resourceMapping.tfStorageBindingPoint >= 0)
	{
		VkDescriptorSetLayoutBinding entry{};
		entry.binding = shader->resourceMapping.tfStorageBindingPoint;
		entry.descriptorCount = 1;
		entry.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		entry.pImmutableSamplers = nullptr;
		entry.stageFlags = stageFlags;
		descriptorSetLayoutBindings.emplace_back(entry);
	}

	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
		vkrPipelineInfo->dynamicOffsetInfo.hasUniformVar[stageIndex] = true;
	if (shader->resourceMapping.hasUniformBuffers())
		vkrPipelineInfo->dynamicOffsetInfo.hasUniformBuffers[stageIndex] = true;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = descriptorSetLayoutBindings.size();
	layoutInfo.pBindings = descriptorSetLayoutBindings.data();

	if (vkCreateDescriptorSetLayout(vkRenderer->m_logicalDevice, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		vkRenderer->UnrecoverableError(fmt::format("Failed to create descriptor set layout for shader {0:#x}", shader->baseHash).c_str());
}

bool PipelineCompiler::InitShaderStages(VulkanRenderer* vkRenderer, RendererShaderVk* vkVertexShader, RendererShaderVk* vkPixelShader, RendererShaderVk* vkGeometryShader)
{
	// prepare shader stages
	cemu_assert_debug(vkVertexShader == nullptr || vkVertexShader->IsCompiled());
	cemu_assert_debug(vkPixelShader == nullptr || vkPixelShader->IsCompiled());
	cemu_assert_debug(vkGeometryShader == nullptr || vkGeometryShader->IsCompiled());

	if ((vkVertexShader && vkVertexShader->GetShaderModule() == VK_NULL_HANDLE) ||
		(vkGeometryShader && vkGeometryShader->GetShaderModule() == VK_NULL_HANDLE) ||
		(vkPixelShader && vkPixelShader->GetShaderModule() == VK_NULL_HANDLE))
	{
		cemuLog_log(LogType::Force, "Vulkan-Info: Pipeline creation failed due to invalid shader(s)");
		return false;
	}

	if (vkVertexShader)
		shaderStages.emplace_back(vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, vkVertexShader->GetShaderModule(), "main"));

	if (vkGeometryShader)
		shaderStages.emplace_back(vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_GEOMETRY_BIT, vkGeometryShader->GetShaderModule(), "main"));
	else if (m_rectEmulationGS)
		shaderStages.emplace_back(vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_GEOMETRY_BIT, m_rectEmulationGS->GetShaderModule(), "main"));

	if (vkPixelShader)
		shaderStages.emplace_back(vkRenderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, vkPixelShader->GetShaderModule(), "main"));

	return true;
}

void PipelineCompiler::InitVertexInputState(const LatteContextRegister& latteRegister, LatteDecompilerShader* vertexShader, LatteFetchShader* fetchShader)
{
	// with VK_EXT_vertex_input_dynamic_state the draw sets the same
	// (draw_updateVertexInput) and what is baked here is ignored
	auto input = std::make_unique<VulkanVertexInput>();
	VulkanPipelineKey_VertexInput(*fetchShader, vertexShader->resourceMapping.attributeMapping, latteRegister, *input);

	vertexInputAttributeDescription.resize(input->attributeCount);
	for (uint32 i = 0; i < input->attributeCount; i++)
	{
		const auto& a = input->attributes[i];
		if (a.format == VK_FORMAT_UNDEFINED)
			cemuLog_log(LogType::Force, "Unsupported vertex format in fetch shader {:016x}", fetchShader->key);
		vertexInputAttributeDescription[i] = {a.location, a.binding, a.format, a.offset};
	}
	vertexInputBindingDescription.resize(input->bindingCount);
	for (uint32 i = 0; i < input->bindingCount; i++)
	{
		const auto& b = input->bindings[i];
		vertexInputBindingDescription[i] = {b.binding, b.stride, b.inputRate};
	}

	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = vertexInputBindingDescription.size();
	vertexInputInfo.pVertexBindingDescriptions = vertexInputBindingDescription.data();
	vertexInputInfo.vertexAttributeDescriptionCount = vertexInputAttributeDescription.size();
	vertexInputInfo.pVertexAttributeDescriptions = vertexInputAttributeDescription.data();
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

	if (usesBlendConstants)
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
		pipelineInfo->usesBlendConstants = true;
	}
	if (usesDepthBias)
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
		pipelineInfo->usesDepthBias = true;
	}
	if (VulkanRenderer::GetInstance()->UseAttachmentFeedbackLoop())
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT);
	}
	// core Vulkan, every device
	dynamicStates.emplace_back(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
	dynamicStates.emplace_back(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
	dynamicStates.emplace_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
	if (VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.extended_dynamic_state)
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_CULL_MODE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_FRONT_FACE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_STENCIL_OP_EXT);
	}
	if (VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.extended_dynamic_state3_blend)
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
	}
	if (VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.vertex_input_dynamic_state)
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
	if (VulkanRenderer::draw_pipelineKeyDynamicFlags() & VULKAN_PIPELINE_KEY_DYNAMIC_TOPOLOGY)
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT);
	}
	if (VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.extended_dynamic_state2)
	{
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT);
		dynamicStates.emplace_back(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT);
	}

	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = dynamicStates.size();
	dynamicState.pDynamicStates = dynamicStates.data();
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

	// if required generate RECT emulation geometry shader
	if (!vkRenderer->m_featureControl.deviceExtensions.nv_fill_rectangle && isPrimitiveRect)
	{
		cemu_assert(m_vkGeometryShader == nullptr); // todo - handle cases where the game already provides a GS
		m_rectEmulationGS = rectsEmulationGS_generate(pipelineInfo->vertexShader, latteRegister);
		pipelineInfo->rectEmulationGS = m_rectEmulationGS;
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

bool PipelineCompiler::Compile(bool forceCompile, bool isRenderThread, bool showInOverlay)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();

	if (!vkRenderer->m_featureControl.deviceExtensions.pipeline_creation_cache_control)
		forceCompile = true; // if VK_EXT_pipeline_creation_cache_control is not supported we always force synchronous compilation

	if (!forceCompile)
	{
		// fail early if some shader stages are not compiled
		if (m_vkVertexShader && m_vkVertexShader->IsCompiled() == false)
			return false;
		if (m_vkPixelShader && m_vkPixelShader->IsCompiled() == false)
			return false;
		if (m_vkGeometryShader && m_vkGeometryShader->IsCompiled() == false)
			return false;
	}
	else
	{
		// if some shader stages are not compiled yet, compile them now
		if (m_vkVertexShader && m_vkVertexShader->IsCompiled() == false)
			m_vkVertexShader->PreponeCompilation(isRenderThread);
		if (m_vkPixelShader && m_vkPixelShader->IsCompiled() == false)
			m_vkPixelShader->PreponeCompilation(isRenderThread);
		if (m_vkGeometryShader && m_vkGeometryShader->IsCompiled() == false)
			m_vkGeometryShader->PreponeCompilation(isRenderThread);
	}

	if (shaderStages.empty())
	{
		if (!InitShaderStages(vkRenderer, m_vkVertexShader, m_vkPixelShader, m_vkGeometryShader))
			return true; // invalid shaders, cannot compile
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = shaderStages.size();
	pipelineInfo.pStages = shaderStages.data();
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
	if (!forceCompile)
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
		return CompileFromLibraries(pipelineInfo, forceCompile, isRenderThread);

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
		for (const auto& attr : vertexInputAttributeDescription)
		{
			VkFormatProperties props{};
			vkGetPhysicalDeviceFormatProperties(VulkanRenderer::GetInstance()->GetPhysicalDevice(), attr.format, &props);
			const bool supported = (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
			vertexFormats += fmt::format("{}{}:{}{}", vertexFormats.empty() ? "" : " ", attr.location, (sint32)attr.format, supported ? "" : "!");
		}
		cemuLog_log(LogType::Force, "Failed to create graphics pipeline. Error {} (VS {} GS {} PS {}, color formats [{}], depth format {}, topology {}, vertex formats [{}])",
			(sint32)result, shaderName(m_vkVertexShader), shaderName(m_vkGeometryShader), shaderName(m_vkPixelShader), colorFormats,
			m_renderPassObj ? (sint32)m_renderPassObj->m_depthAttachmentFormat : -1,
			pipelineInfo.pInputAssemblyState ? (sint32)pipelineInfo.pInputAssemblyState->topology : -1, vertexFormats);
		cemu_assert_debug(false);
		return true; // true indicates that caller should no longer attempt to compile this pipeline again
	}
	vkRenderer->m_pipeline_cache_semaphore.notify();

	return true;
}

// pipelines the compile threads linked optimized, for the GPU thread to put
// in the fast-linked ones' place
static MpmcQueue<PipelineCompiler::OptimizedPipeline> s_optimizedPipelines;

bool PipelineCompiler::TakeOptimizedPipeline(OptimizedPipeline& result)
{
	return s_optimizedPipelines.peek2(result);
}

// The pipeline from its four parts (VulkanPipelineLibrary.h): each found
// made, or made here and kept for the pipelines after it, then linked.
// Without forceCompile nothing is compiled: a part not made yet - or only
// made by the driver's cache - leaves it to a compile thread (false). On
// the GPU thread the parts are linked as they are, a link and no compile,
// and a compile thread links them optimized, which then takes its place;
// on a compile thread they are linked optimized at once.
bool PipelineCompiler::CompileFromLibraries(const VkGraphicsPipelineCreateInfo& full, bool forceCompile, bool isRenderThread)
{
	VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance();
	VulkanPipelineLibraryCache& cache = *vkRenderer->m_pipelineLibraries;
	const VkDevice device = vkRenderer->m_logicalDevice;
	auto shaderHash = [](RendererShaderVk* shader) {
		return shader ? shader->GetBaseHash() * 0x9E3779B97F4A7C15ull + shader->GetAuxHash() + 1 : 0;
	};
	VulkanPipelineLibraryIdentity identity{};
	identity.vertexShader = shaderHash(m_vkVertexShader);
	// the rect emulation geometry shader is made for this pipeline: its part
	// is too, under a number no other part has (one per compiler, so that a
	// compile thread's try finds the GPU thread's)
	static std::atomic<uint64> s_rectEmulationSerial{0};
	if (m_rectEmulationGS && m_rectEmulationSerial == 0)
		m_rectEmulationSerial = (1ull << 63) | s_rectEmulationSerial.fetch_add(1, std::memory_order_relaxed);
	identity.geometryShader = m_vkGeometryShader ? shaderHash(m_vkGeometryShader) : m_rectEmulationSerial;
	identity.pixelShader = shaderHash(m_vkPixelShader);
	identity.renderPass = m_renderPassObj->m_hashForPipeline;
	identity.robustBufferAccess = m_requestRobustBufferAccess;

	VkPipeline libraries[VULKAN_PIPELINE_LIBRARY_PARTS]{};
	bool ok = true;
	for (int p = 0; p < VULKAN_PIPELINE_LIBRARY_PARTS && ok; p++)
	{
		const VulkanPipelineLibraryPart part = (VulkanPipelineLibraryPart)p;
		const uint64 key = VulkanPipelineLibrary_Key(part, full, identity);
		VkPipeline library = VK_NULL_HANDLE;
		const VulkanPipelineLibraryCache::State state = cache.Find(part, key, library);
		if (state == VulkanPipelineLibraryCache::State::Ready)
		{
			libraries[p] = library;
			continue;
		}
		const bool claimed = state == VulkanPipelineLibraryCache::State::Missing && cache.Claim(part, key);
		if (!forceCompile && !claimed)
		{
			ok = false; // being made elsewhere
			break;
		}
		// made here: under the claim, or adopted by the cache when the key is
		// another thread's (a pipeline needs its parts for as long as it
		// lives); without forceCompile only out of the driver's cache
		VkGraphicsPipelineCreateInfo ci = full;
		if (!forceCompile)
			ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
		else
			ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
		PSO_TRACE_SCOPE(isRenderThread ? (forceCompile ? "pipeline library sync" : "pipeline library from cache") : "pipeline library", key);
		vkRenderer->m_pipeline_cache_builds.Enter();
		const VkResult result = VulkanPipelineLibrary_Create(device, vkRenderer->m_pipeline_cache, part, ci, m_partLayout[p], &library);
		vkRenderer->m_pipeline_cache_builds.Leave();
		if (result != VK_SUCCESS)
			library = VK_NULL_HANDLE;
		if (claimed)
			cache.Publish(part, key, library);
		else if (library != VK_NULL_HANDLE)
			cache.Adopt(library);
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
	if (ok)
	{
		// linked as they are on the GPU thread, optimized elsewhere
		const bool optimize = !isRenderThread;
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
	if (isRenderThread)
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
// pushed by the GPU thread, taken by every compile thread: no lock
static MpmcQueue<PipelineCompiler*> s_pipelineCompileRequests;

static void compilePipeline_thread(sint32 threadIndex)
{
	SetThreadName("compilePl");
#ifdef _WIN32
	// to avoid starving the main cpu and render threads the pipeline compile threads run at lower priority
	// except for one thread which we always run at normal priority to prevent the opposite scenario where all compile threads are starved
	if(threadIndex != 0)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
	while (!s_compileThreadsShutdownSignal)
	{
		PipelineCompiler* request = s_pipelineCompileRequests.pop();
		if (!request)
			continue;
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
			s_pipelineCompileRequests.push(nullptr);
	}
	for (auto& thread : s_compileThreads)
		thread.join();
	while (!s_pipelineCompileRequests.empty())
	{
		PipelineCompiler* pipelineCompiler = s_pipelineCompileRequests.pop();
		if (pipelineCompiler)
			delete pipelineCompiler;
	}
	s_compileThreads.clear();
}

void PipelineCompiler::CompileThreadPool_QueueCompilation(PipelineCompiler* v)
{
	s_pipelineCompileRequests.push(v);
}

void PipelineCompiler::CompileThreadPool_QueuePrepared(PreparedPipeline& prepared)
{
	PipelineCompiler* job = new PipelineCompiler();
	job->m_preparedTicket = prepared.Queue();
	s_pipelineCompileRequests.push(job);
}
