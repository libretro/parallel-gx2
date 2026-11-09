// The state the Vulkan pipelines leave to the draw, drawn on a real driver
// (lavapipe in CI): for register states from a seed, a pipeline with the
// cull mode, front face, depth and stencil tests, blending, polygon offset
// enable, rasterizer discard and vertex input baked in (as without the
// extensions) and one with them set by the draw (as with
// VK_EXT_extended_dynamic_state, _3, _2 and VK_EXT_vertex_input_dynamic_state)
// draw the same four overlapping triangles, read from vertex buffers in a
// layout from the seed, over the same depth and stencil, and the pictures
// must match to the byte. Both take their values from the functions the pipeline
// compiler and the draw use (VulkanPipelineKey.h). Skipped, said so,
// without a device that has the extensions - a failure with
// VN_REQUIRE_DEVICE set, as the CIs set it.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"

#include <gfx/shaders/rslc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)
#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_); std::exit(1); } } while (0)

// the position per vertex and a scale per instance, as integers the way
// the core's vertex shaders read their attributes (VulkanPipelineKey_VertexFormat)
static const char* kVS = R"(#version 450
layout(push_constant) uniform PC { vec4 color; float z; float flip; } pc;
layout(location = 0) in uvec2 pos;
layout(location = 1) in uint scale;
void main()
{
	gl_Position = vec4(uintBitsToFloat(pos) * uintBitsToFloat(scale), pc.z, 1.0);
}
)";
static const char* kFS = R"(#version 450
layout(push_constant) uniform PC { vec4 color; float z; float flip; } pc;
layout(location = 0) out vec4 o;
void main()
{
	o = pc.color;
}
)";

struct PushConstants
{
	float color[4];
	float z;
	float flip;
};

static const uint32_t W = 16, H = 16;
// a vertex buffer for both bindings: the vertices at the start, the
// instance's scale from kInstanceData
static const uint32_t kVertexBufferSize = 4096, kInstanceData = 2048;

// a vertex layout from the seed: the fetch shader's buffers, semantics,
// formats and offsets, the strides in the registers, the locations the
// vertex shader maps the semantics to
struct VertexLayout
{
	LatteParsedFetchShaderAttribute attrs[3]{};
	LatteFetchShader fetchShader;
	sint8 mapping[LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS];
	uint32 vertexBuffer, instanceBuffer;
	uint32 vertexStride, instanceStride;
	uint32 posOffset, scaleOffset;
};

static std::vector<uint32_t> Compile(const char* glsl, rslc_stage stage)
{
	char err[512] = {};
	rslc_opts opts{};
	opts.dst = RSLC_DST_SPIRV;
	opts.err = err;
	opts.err_cap = sizeof(err);
	rslc_result result{};
	if (!rslc_compile(glsl, strlen(glsl), stage, &opts, &result))
	{
		std::printf("FAIL: rslc: %s\n", err);
		std::exit(1);
	}
	std::vector<uint32_t> words(result.spirv, result.spirv + result.spirv_len);
	rslc_result_free(&result, &opts);
	return words;
}

static void Skip(const char* why)
{
	std::printf("skip: %s, nothing was drawn\n", why);
	if (getenv("VN_REQUIRE_DEVICE"))
	{
		std::printf("FAIL: VN_REQUIRE_DEVICE is set\n");
		s_failures++;
	}
}

struct Context
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
	VkDeviceMemory colorMem = VK_NULL_HANDLE, depthMem = VK_NULL_HANDLE, readMem = VK_NULL_HANDLE;
	VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkBuffer readback = VK_NULL_HANDLE;
	VkBuffer vertices = VK_NULL_HANDLE;
	VkDeviceMemory vertexMem = VK_NULL_HANDLE;
	uint8_t* vertexData = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
};

static uint32_t MemoryType(const Context& c, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(c.gpu, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	std::printf("FAIL: no memory type\n");
	std::exit(1);
}

static VkDeviceMemory Bind(const Context& c, VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(c, req.memoryTypeBits, want);
	VkDeviceMemory m;
	VKCHECK(vkAllocateMemory(c.device, &ai, nullptr, &m));
	return m;
}

static bool CreateContext(Context& c)
{
	if (!InitializeGlobalVulkan())
		return false;
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (vkCreateInstance(&ici, nullptr, &c.instance) != VK_SUCCESS || !InitializeInstanceVulkan(c.instance))
		return false;
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
	std::vector<VkPhysicalDevice> gpus(count);
	vkEnumeratePhysicalDevices(c.instance, &count, gpus.data());
	for (VkPhysicalDevice g : gpus)
	{
		VkPhysicalDeviceExtendedDynamicStateFeaturesEXT e1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
		VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
		VkPhysicalDeviceExtendedDynamicState2FeaturesEXT e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
		VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
		e1.pNext = &e3;
		e3.pNext = &e2;
		e2.pNext = &vi;
		VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		f2.pNext = &e1;
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(g, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> exts(n);
		vkEnumerateDeviceExtensionProperties(g, nullptr, &n, exts.data());
		bool has1 = false, has2 = false, has3 = false, hasVi = false;
		for (auto& e : exts)
		{
			has1 |= !strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
			has2 |= !strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
			has3 |= !strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
			hasVi |= !strcmp(e.extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
		}
		if (!has1 || !has2 || !has3 || !hasVi)
			continue;
		vkGetPhysicalDeviceFeatures2(g, &f2);
		if (e1.extendedDynamicState && e2.extendedDynamicState2 && vi.vertexInputDynamicState && e3.extendedDynamicState3ColorBlendEnable &&
			e3.extendedDynamicState3ColorBlendEquation && e3.extendedDynamicState3ColorWriteMask)
		{
			c.gpu = g;
			break;
		}
	}
	if (!c.gpu)
		return false;
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &qn, qf.data());
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			c.family = i;
			break;
		}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = c.family;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT e1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
	e1.extendedDynamicState = VK_TRUE;
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
	e3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
	e3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
	e3.extendedDynamicState3ColorWriteMask = VK_TRUE;
	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
	e2.extendedDynamicState2 = VK_TRUE;
	VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
	vi.vertexInputDynamicState = VK_TRUE;
	e1.pNext = &e3;
	e3.pNext = &e2;
	e2.pNext = &vi;
	const char* exts[] = {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
		VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &e1;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = 4;
	dci.ppEnabledExtensionNames = exts;
	if (vkCreateDevice(c.gpu, &dci, nullptr, &c.device) != VK_SUCCESS)
		return false;
	InitializeDeviceVulkan(c.device);
	vkGetDeviceQueue(c.device, c.family, 0, &c.queue);

	for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT})
	{
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(c.gpu, f, &fp);
		if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			c.depthFormat = f;
			break;
		}
	}
	if (c.depthFormat == VK_FORMAT_UNDEFINED)
		return false;

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = c.family;
	VKCHECK(vkCreateCommandPool(c.device, &pci, nullptr, &c.pool));

	VkAttachmentDescription att[2]{};
	att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	att[1] = att[0];
	att[1].format = c.depthFormat;
	att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &cref;
	sub.pDepthStencilAttachment = &dref;
	VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpi.attachmentCount = 2;
	rpi.pAttachments = att;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &sub;
	VKCHECK(vkCreateRenderPass(c.device, &rpi, nullptr, &c.renderPass));

	auto makeImage = [&](VkFormat f, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = f;
		ii.extent = {W, H, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = usage;
		VKCHECK(vkCreateImage(c.device, &ii, nullptr, &img));
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(c.device, img, &req);
		mem = Bind(c, req, 0);
		VKCHECK(vkBindImageMemory(c.device, img, mem, 0));
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = f;
		vi.subresourceRange = {aspect, 0, 1, 0, 1};
		VKCHECK(vkCreateImageView(c.device, &vi, nullptr, &view));
	};
	makeImage(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT, c.color, c.colorMem, c.colorView);
	makeImage(c.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, c.depth, c.depthMem, c.depthView);
	VkImageView views[2] = {c.colorView, c.depthView};
	VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbi.renderPass = c.renderPass;
	fbi.attachmentCount = 2;
	fbi.pAttachments = views;
	fbi.width = W;
	fbi.height = H;
	fbi.layers = 1;
	VKCHECK(vkCreateFramebuffer(c.device, &fbi, nullptr, &c.framebuffer));

	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = W * H * 4;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VKCHECK(vkCreateBuffer(c.device, &bi, nullptr, &c.readback));
	VkMemoryRequirements breq;
	vkGetBufferMemoryRequirements(c.device, c.readback, &breq);
	c.readMem = Bind(c, breq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKCHECK(vkBindBufferMemory(c.device, c.readback, c.readMem, 0));

	bi.size = kVertexBufferSize;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VKCHECK(vkCreateBuffer(c.device, &bi, nullptr, &c.vertices));
	vkGetBufferMemoryRequirements(c.device, c.vertices, &breq);
	c.vertexMem = Bind(c, breq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKCHECK(vkBindBufferMemory(c.device, c.vertices, c.vertexMem, 0));
	void* vmap;
	VKCHECK(vkMapMemory(c.device, c.vertexMem, 0, kVertexBufferSize, 0, &vmap));
	c.vertexData = (uint8_t*)vmap;

	VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	VKCHECK(vkCreatePipelineLayout(c.device, &pli, nullptr, &c.layout));

	auto module = [&](const std::vector<uint32_t>& words) {
		VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		si.codeSize = words.size() * 4;
		si.pCode = words.data();
		VkShaderModule m;
		VKCHECK(vkCreateShaderModule(c.device, &si, nullptr, &m));
		return m;
	};
	c.vs = module(Compile(kVS, RSLC_STAGE_VERTEX));
	c.fs = module(Compile(kFS, RSLC_STAGE_FRAGMENT));
	return true;
}

static void DestroyContext(Context& c)
{
	if (c.device)
	{
		vkDeviceWaitIdle(c.device);
		vkDestroyShaderModule(c.device, c.vs, nullptr);
		vkDestroyShaderModule(c.device, c.fs, nullptr);
		vkDestroyPipelineLayout(c.device, c.layout, nullptr);
		vkDestroyBuffer(c.device, c.readback, nullptr);
		vkDestroyBuffer(c.device, c.vertices, nullptr);
		vkDestroyFramebuffer(c.device, c.framebuffer, nullptr);
		vkDestroyImageView(c.device, c.colorView, nullptr);
		vkDestroyImageView(c.device, c.depthView, nullptr);
		vkDestroyImage(c.device, c.color, nullptr);
		vkDestroyImage(c.device, c.depth, nullptr);
		vkFreeMemory(c.device, c.colorMem, nullptr);
		vkFreeMemory(c.device, c.depthMem, nullptr);
		vkFreeMemory(c.device, c.readMem, nullptr);
		vkFreeMemory(c.device, c.vertexMem, nullptr);
		vkDestroyRenderPass(c.device, c.renderPass, nullptr);
		vkDestroyCommandPool(c.device, c.pool, nullptr);
		vkDestroyDevice(c.device, nullptr);
	}
	if (c.instance)
		vkDestroyInstance(c.instance, nullptr);
}

// a pipeline with the state baked in (dynamicState false) or left to the draw
static VkPipeline MakePipeline(const Context& c, const LatteContextRegister& lcr, const VertexLayout& layout, bool dynamicState)
{
	const VulkanDepthStencilCull dsc = VulkanPipelineKey_DepthStencilCull(lcr);
	const VulkanRasterizerEnables re = VulkanPipelineKey_Rasterizer(lcr);
	VkFormat formats[8] = {VK_FORMAT_R8G8B8A8_UNORM};
	VkPipelineColorBlendAttachmentState blend[8];
	VulkanPipelineKey_Blend(lcr, formats, blend);

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = c.vs;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = c.fs;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	// baked in as PipelineCompiler::InitVertexInputState bakes it
	auto input = std::make_unique<VulkanVertexInput>();
	VulkanPipelineKey_VertexInput(layout.fetchShader, layout.mapping, lcr, *input);
	std::vector<VkVertexInputBindingDescription> vb;
	std::vector<VkVertexInputAttributeDescription> va;
	for (uint32 i = 0; i < input->bindingCount; i++)
		vb.push_back({input->bindings[i].binding, input->bindings[i].stride, input->bindings[i].inputRate});
	for (uint32 i = 0; i < input->attributeCount; i++)
		va.push_back({input->attributes[i].location, input->attributes[i].binding, input->attributes[i].format, input->attributes[i].offset});
	if (!dynamicState)
	{
		vin.vertexBindingDescriptionCount = (uint32_t)vb.size();
		vin.pVertexBindingDescriptions = vb.data();
		vin.vertexAttributeDescriptionCount = (uint32_t)va.size();
		vin.pVertexAttributeDescriptions = va.data();
	}
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport vp{0, 0, (float)W, (float)H, 0, 1};
	VkRect2D sc{{0, 0}, {W, H}};
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.pViewports = &vp;
	vps.scissorCount = 1;
	vps.pScissors = &sc;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.lineWidth = 1.0f;
	rs.cullMode = dynamicState ? VK_CULL_MODE_NONE : dsc.cullMode;
	rs.frontFace = dynamicState ? VK_FRONT_FACE_COUNTER_CLOCKWISE : dsc.frontFace;
	rs.depthBiasEnable = dynamicState ? VK_FALSE : re.depthBiasEnable;
	rs.rasterizerDiscardEnable = dynamicState ? VK_FALSE : re.rasterizerDiscardEnable;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	if (!dynamicState)
	{
		ds.depthTestEnable = dsc.depthTestEnable;
		ds.depthWriteEnable = dsc.depthWriteEnable;
		ds.depthCompareOp = dsc.depthCompareOp;
		ds.stencilTestEnable = dsc.stencilTestEnable;
		ds.front = {dsc.frontFailOp, dsc.frontPassOp, dsc.frontDepthFailOp, dsc.frontCompareOp, 0, 0, 0};
		ds.back = {dsc.backFailOp, dsc.backPassOp, dsc.backDepthFailOp, dsc.backCompareOp, 0, 0, 0};
	}
	ds.maxDepthBounds = 1.0f;
	VkPipelineColorBlendAttachmentState att = blend[0];
	if (dynamicState)
		att = {};
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &att;
	std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
		VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
	if (dynamicState)
		dyn.insert(dyn.end(), {VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
			VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
			VK_DYNAMIC_STATE_STENCIL_OP_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
			VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT,
			VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT});
	VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dsi.dynamicStateCount = (uint32_t)dyn.size();
	dsi.pDynamicStates = dyn.data();
	VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vin;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vps;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pDepthStencilState = &ds;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &dsi;
	gp.layout = c.layout;
	gp.renderPass = c.renderPass;
	VkPipeline p;
	VKCHECK(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gp, nullptr, &p));
	return p;
}

// four overlapping triangles over a cleared depth and stencil, drawn with
// the pipeline; the picture
static std::vector<uint8_t> Draw(const Context& c, VkPipeline p, const LatteContextRegister& lcr, const VertexLayout& layout,
	bool dynamicState)
{
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = c.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VKCHECK(vkAllocateCommandBuffers(c.device, &cai, &cb));
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHECK(vkBeginCommandBuffer(cb, &bi));
	VkClearValue clears[2];
	clears[0].color = {{0.2f, 0.4f, 0.6f, 0.8f}};
	clears[1].depthStencil = {0.5f, 0x5A};
	VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rbi.renderPass = c.renderPass;
	rbi.framebuffer = c.framebuffer;
	rbi.renderArea = {{0, 0}, {W, H}};
	rbi.clearValueCount = 2;
	rbi.pClearValues = clears;
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	const float constants[4] = {0.25f, 0.5f, 0.75f, 1.0f};
	vkCmdSetBlendConstants(cb, constants);
	// the stencil reference and masks: dynamic in every pipeline
	const auto& front = lcr.DB_STENCILREFMASK;
	const auto& back = lcr.DB_STENCILREFMASK_BF;
	const bool twoSided = lcr.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();
	vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, front.get_STENCILMASK_F());
	vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, front.get_STENCILWRITEMASK_F());
	vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, front.get_STENCILREF_F());
	vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, twoSided ? back.get_STENCILMASK_B() : front.get_STENCILMASK_F());
	vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, twoSided ? back.get_STENCILWRITEMASK_B() : front.get_STENCILWRITEMASK_F());
	vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, twoSided ? back.get_STENCILREF_B() : front.get_STENCILREF_F());
	// the depth bias values: dynamic in every pipeline that may use them
	// (VulkanRenderer::draw_updateDepthBias)
	vkCmdSetDepthBias(cb, lcr.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET(), lcr.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP(),
		lcr.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f);
	if (dynamicState)
	{
		// what VulkanRenderer::draw_updateDepthStencilCull, draw_updateBlend
		// and draw_updateRasterizer do
		VulkanPipelineKey_SetDepthStencilCull(cb, VulkanPipelineKey_DepthStencilCull(lcr));
		VulkanPipelineKey_SetRasterizer(cb, VulkanPipelineKey_Rasterizer(lcr));
		auto input = std::make_unique<VulkanVertexInput>();
		VulkanPipelineKey_VertexInput(layout.fetchShader, layout.mapping, lcr, *input);
		VulkanPipelineKey_SetVertexInput(cb, *input);
		VkFormat formats[8] = {VK_FORMAT_R8G8B8A8_UNORM};
		VkPipelineColorBlendAttachmentState att[8];
		VulkanPipelineKey_Blend(lcr, formats, att);
		VulkanPipelineKey_SetBlend(cb, att, 1);
	}
	// depths around the cleared 0.5 so that a test that passes and writes
	// changes whether the next draw passes, whichever way the test goes
	const PushConstants draws[4] = {
		{{0.9f, 0.1f, 0.3f, 0.6f}, 0.3f, 0},
		{{0.2f, 0.8f, 0.5f, 0.3f}, 0.4f, 0},
		{{0.1f, 0.3f, 0.9f, 0.7f}, 0.7f, 0},
		{{0.6f, 0.6f, 0.1f, 0.2f}, 0.6f, 0},
	};
	VkBuffer buffers[2] = {c.vertices, c.vertices};
	VkDeviceSize offsets[2] = {0, kInstanceData};
	vkCmdBindVertexBuffers(cb, layout.vertexBuffer, 1, &buffers[0], &offsets[0]);
	vkCmdBindVertexBuffers(cb, layout.instanceBuffer, 1, &buffers[1], &offsets[1]);
	for (uint32 d = 0; d < 4; d++)
	{
		vkCmdPushConstants(cb, c.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draws[d]), &draws[d]);
		// each draw its three vertices and the instance with its scale
		vkCmdDraw(cb, 3, 1, d * 3, d);
	}
	vkCmdEndRenderPass(cb);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {W, H, 1};
	vkCmdCopyImageToBuffer(cb, c.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.readback, 1, &region);
	VKCHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	VKCHECK(vkCreateFence(c.device, &fi, nullptr, &fence));
	VKCHECK(vkQueueSubmit(c.queue, 1, &si, fence));
	VKCHECK(vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX));
	vkDestroyFence(c.device, fence, nullptr);
	std::vector<uint8_t> pixels(W * H * 4);
	void* map;
	VKCHECK(vkMapMemory(c.device, c.readMem, 0, pixels.size(), 0, &map));
	memcpy(pixels.data(), map, pixels.size());
	vkUnmapMemory(c.device, c.readMem);
	vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
	return pixels;
}

// a register state from the seed, valid for what the pipelines take from it
static void RandomState(std::mt19937& rng, LatteContextRegister& lcr)
{
	uint32* r = lcr.GetRawView();
	auto bits = [&](uint32 n) { return rng() & ((1u << n) - 1); };
	r[Latte::REGADDR::VGT_PRIMITIVE_TYPE] = 4; // triangles
	// cull and front face, polygon offset on or off
	r[Latte::REGADDR::PA_SU_SC_MODE_CNTL] = bits(3) | (bits(1) << 11);
	// a constant offset of an eighth to a half of the depth range, up or
	// down, at the triangles' depths (2^-25 to 2^-24 a unit there): enough
	// to flip the depth tests against the clear and each other. No slope
	// (the triangles are flat) and no clamp (no device feature wanted)
	static const float offsets[] = {0.0f, 8.0e6f, -8.0e6f, 4.0e6f};
	const float offset = offsets[bits(2)];
	std::memcpy(&r[Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET], &offset, 4);
	// rasterizer discard a quarter of the time, kept by the viewport's X
	// offset half of that (GX2SetSpecialState(0, true))
	r[Latte::REGADDR::PA_CL_CLIP_CNTL] = (bits(2) == 0 ? 1u : 0u) << 22;
	r[Latte::REGADDR::PA_CL_VTE_CNTL] = bits(1) << 1;
	// depth: test, write, compare; stencil: test, two-sided, compare and ops
	r[Latte::REGADDR::DB_DEPTH_CONTROL] = bits(1) | (bits(1) << 1) | (bits(1) << 2) | (bits(3) << 4) | (bits(1) << 7) |
		(bits(3) << 8) | (bits(3) << 11) | (bits(3) << 14) | (bits(3) << 17) | (bits(3) << 20) | (bits(3) << 23) |
		(bits(3) << 26) | (bits(3) << 29);
	r[Latte::REGADDR::DB_STENCILREFMASK] = rng() & 0xFFFFFF;
	r[Latte::REGADDR::DB_STENCILREFMASK_BF] = rng() & 0xFFFFFF;
	// blending on render target 0 or not, copy as the logic op
	r[Latte::REGADDR::CB_COLOR_CONTROL] = 0xCC0000 | (bits(1) << 8);
	r[Latte::REGADDR::CB_TARGET_MASK] = bits(4);
	// factors without the dual-source ones (0x0F-0x12) and the two not mapped
	static const uint32 factors[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xD, 0xE, 0x13, 0x14};
	auto factor = [&] { return factors[rng() % std::size(factors)]; };
	r[Latte::REGADDR::CB_BLEND0_CONTROL] = factor() | ((rng() % 5) << 5) | (factor() << 8) | (factor() << 16) |
		((rng() % 5) << 21) | (factor() << 24) | (bits(1) << 29);
}

// a vertex layout from the seed and the vertices written in it: the four
// triangles, front or back facing, with a scale per draw (an instance)
static void RandomLayout(std::mt19937& rng, LatteContextRegister& lcr, VertexLayout& l, uint8_t* data)
{
	auto pick = [&](uint32 n) { return rng() % n; };
	l.vertexBuffer = pick(8);
	l.instanceBuffer = 8 + pick(8);
	// the position: two 32-bit components at an offset in a stride with
	// room for it; the scale: one, per instance
	l.vertexStride = 8 + 4 * pick(8);
	l.posOffset = 4 * pick((l.vertexStride - 8) / 4 + 1);
	l.instanceStride = 4 + 4 * pick(4);
	l.scaleOffset = 4 * pick(l.instanceStride / 4);
	const uint8 posSemantic = (uint8)(1 + pick(100)), scaleSemantic = (uint8)(101 + pick(100));
	l.attrs[0] = {(uint8)l.vertexBuffer, posSemantic, Latte::E_HWFMT::HWFMT_32_32_FLOAT, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, l.posOffset};
	// one the vertex shader does not read
	l.attrs[1] = {(uint8)l.vertexBuffer, (uint8)(posSemantic + 1), Latte::E_HWFMT::HWFMT_8_8_8_8, LatteConst::VERTEX_DATA, 0, 0, {}, {}, 1, 0};
	l.attrs[2] = {(uint8)l.instanceBuffer, scaleSemantic, Latte::E_HWFMT::HWFMT_32_FLOAT, LatteConst::INSTANCE_DATA, 0, 0, {}, {}, 1, l.scaleOffset};
	LatteParsedFetchShaderBufferGroup g0{}, g1{};
	g0.attributeBufferIndex = (uint8)l.vertexBuffer;
	g0.attribCount = 2;
	g0.attrib = l.attrs;
	g1.attributeBufferIndex = (uint8)l.instanceBuffer;
	g1.attribCount = 1;
	g1.attrib = l.attrs + 2;
	l.fetchShader.bufferGroups = {g0, g1};
	std::fill(std::begin(l.mapping), std::end(l.mapping), (sint8)-1);
	l.mapping[posSemantic] = 0;
	l.mapping[scaleSemantic] = 1;
	uint32* r = lcr.GetRawView();
	r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + l.vertexBuffer * 7 + 2] = l.vertexStride << 11;
	r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + l.instanceBuffer * 7 + 2] = l.instanceStride << 11;

	std::memset(data, 0xA5, kVertexBufferSize);
	static const float tri[3][2] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
	static const float scales[] = {0.75f, 0.5f, 1.0f, 0.875f};
	for (uint32 d = 0; d < 4; d++)
	{
		const bool flip = rng() & 1;
		for (uint32 v = 0; v < 3; v++)
		{
			const uint32 i = flip ? 2 - v : v;
			std::memcpy(data + (d * 3 + v) * l.vertexStride + l.posOffset, tri[i], 8);
		}
		const float scale = scales[pick(4)];
		std::memcpy(data + kInstanceData + d * l.instanceStride + l.scaleOffset, &scale, 4);
	}
}

int main()
{
	Context c;
	if (!CreateContext(c))
	{
		Skip("no Vulkan device with extended dynamic state, _2, _3's blend states and vertex input dynamic state");
	}
	else
	{
		std::mt19937 rng(0x5eed);
		const int states = 400;
		int differing = 0;
		auto lcr = std::make_unique<LatteContextRegister>();
		for (int i = 0; i < states; i++)
		{
			std::memset(lcr.get(), 0, sizeof(LatteContextRegister));
			RandomState(rng, *lcr);
			auto layout = std::make_unique<VertexLayout>();
			RandomLayout(rng, *lcr, *layout, c.vertexData);
			VkPipeline baked = MakePipeline(c, *lcr, *layout, false);
			VkPipeline dynamic = MakePipeline(c, *lcr, *layout, true);
			const std::vector<uint8_t> a = Draw(c, baked, *lcr, *layout, false);
			const std::vector<uint8_t> b = Draw(c, dynamic, *lcr, *layout, true);
			if (a != b)
			{
				if (differing++ < 5)
					std::printf("FAIL: state %d draws differently with the state left to the draw (DB_DEPTH_CONTROL %08x, PA_SU_SC_MODE_CNTL %x, CB_BLEND0_CONTROL %08x, PA_CL_CLIP_CNTL %08x, PA_CL_VTE_CNTL %x)\n",
						i, lcr->GetRawView()[Latte::REGADDR::DB_DEPTH_CONTROL], lcr->GetRawView()[Latte::REGADDR::PA_SU_SC_MODE_CNTL],
						lcr->GetRawView()[Latte::REGADDR::CB_BLEND0_CONTROL], lcr->GetRawView()[Latte::REGADDR::PA_CL_CLIP_CNTL],
						lcr->GetRawView()[Latte::REGADDR::PA_CL_VTE_CNTL]);
			}
			vkDestroyPipeline(c.device, baked, nullptr);
			vkDestroyPipeline(c.device, dynamic, nullptr);
		}
		s_failures += differing;
		std::printf("%d register states drawn baked and dynamic: %d differ\n", states, differing);
	}
	DestroyContext(c);
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("dynstate: all passed\n");
	return 0;
}
