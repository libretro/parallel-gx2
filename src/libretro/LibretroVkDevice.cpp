// The Vulkan context negotiation the core offers the frontend: the core
// makes the device it and the frontend share, with the extensions the
// renderer uses. Version 2 where the frontend takes it (create_device2,
// the device made through the frontend's wrapper so that it adds the
// extensions it uses where the GPU has them - Reflex's
// VK_NV_low_latency2 among them), version 1 (create_device) otherwise.
#include "libretro/LibretroVkDevice.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static retro_log_printf_t s_log = nullptr;

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void Log(enum retro_log_level level, const char* fmt, ...)
{
	if (!s_log)
		return;
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof text, fmt, args);
	va_end(args);
	s_log(level, "Cemu: %s", text);
}

static const VkApplicationInfo* GetApplicationInfo()
{
	static VkApplicationInfo app_info{};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "Cemu";
	app_info.applicationVersion = VK_MAKE_VERSION(2, 6, 0);
	app_info.pEngineName = "Cemu";
	app_info.engineVersion = VK_MAKE_VERSION(2, 6, 0);
	app_info.apiVersion = VK_API_VERSION_1_1;
	return &app_info;
}

// The device extensions, and the extension features, the core enabled on the
// shared device: VulkanRenderer trusts these rather than the GPU's support.
std::vector<std::string> g_libretroVkDeviceExtensions;
bool g_libretroVkCustomBorderColorWithoutFormat = false;
bool g_libretroVkHasPipelineCreationCacheControl = false;
bool g_libretroVkHasCustomBorderColors = false;
bool g_libretroVkHasPipelineRobustness = false;
bool g_libretroVkHasDepthClipEnable = false;
bool g_libretroVkHasExtendedDynamicState = false;
bool g_libretroVkHasExtendedDynamicState3Blend = false;
bool g_libretroVkHasExtendedDynamicState2 = false;
bool g_libretroVkHasVertexInputDynamicState = false;
bool g_libretroVkHasGraphicsPipelineLibrary = false;

// The device the core and the frontend share. Made with vkCreateDevice
// (version 1: the frontend's extensions come in as required ones) or
// through the frontend's wrapper (version 2: the frontend adds its own to
// the core's, those it only uses where the GPU has them included -
// VK_NV_low_latency2, which Reflex needs, among them).
static bool CreateDevice(
	struct retro_vulkan_context* context,
	VkInstance instance,
	VkPhysicalDevice gpu,
	VkSurfaceKHR surface,
	const char** required_device_extensions,
	unsigned num_required_device_extensions,
	const VkPhysicalDeviceFeatures* required_features,
	retro_vulkan_create_device_wrapper_t create_device_wrapper,
	void* opaque)
{
	Log(RETRO_LOG_INFO, "Vulkan %s called (gpu=%p, surface=%p)\n", create_device_wrapper ? "create_device2" : "create_device", (void*)gpu, (void*)surface);

	// Load instance functions
	if (!InitializeGlobalVulkan())
		return false;
	if (!InitializeInstanceVulkan(instance))
		return false;

	// Find suitable physical device if not provided
	if (gpu == VK_NULL_HANDLE)
	{
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance, &count, devices.data());
		for (auto& d : devices)
		{
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(d, &props);
			if (props.apiVersion >= VK_API_VERSION_1_1)
			{
				gpu = d;
				break;
			}
		}
		if (gpu == VK_NULL_HANDLE && !devices.empty())
			gpu = devices[0];
	}

	// Find graphics queue family
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueFamilyCount, queueFamilies.data());

	uint32_t graphicsFamily = 0;
	for (uint32_t i = 0; i < queueFamilyCount; i++)
	{
		if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			graphicsFamily = i;
			break;
		}
	}

	// Build extension list: RetroArch required + Cemu needs
	std::vector<const char*> extensions;
	for (unsigned i = 0; i < num_required_device_extensions; i++)
		extensions.push_back(required_device_extensions[i]);

	// Add Cemu-specific extensions if available
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> availableExts(extCount);
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, availableExts.data());

	auto hasExt = [&](const char* name) {
		for (auto& e : availableExts)
			if (strcmp(e.extensionName, name) == 0) return true;
		return false;
	};

	// The extensions standalone Cemu enables (VulkanRenderer::CreateDeviceCreateInfo)
	// wherever the renderer would use them. The renderer decides from what the
	// GPU supports, not from what this device has; before, half of them were
	// left out, and the renderer used depth clip, pipeline creation feedback and
	// cubic filtering on a device without them - undefined, which Turnip lets
	// pass and Qualcomm's own driver answers by failing pipeline creation
	// (#29, Adreno 840: "Failed to create graphics pipeline. Error -13").
	// Transform feedback is not among them: upstream dropped that path
	// (#1919), streamout goes through a storage buffer the vertex shader
	// writes, and nothing uses the extension any more - while Qualcomm's
	// driver still failed exactly the pipelines with such a vertex shader (#29).
	const char* cemuExtensions[] = {
		VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
		VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
		VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
		VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME,
		VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
		VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
		VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME,
		VK_EXT_FILTER_CUBIC_EXTENSION_NAME,
		VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
		// cull mode, front face and the depth and stencil tests as dynamic
		// state: fewer pipelines (VulkanPipelineKey.h)
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
		// blend enables, equations and write masks as dynamic state
		VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
		// depth bias enable and rasterizer discard as dynamic state
		VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
		// vertex buffer strides and attribute offsets as dynamic state
		VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
		// pipelines linked from parts compiled ahead, a link at the draw
		// (VulkanPipelineLibrary.h)
		VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
		VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
	};
	for (auto ext : cemuExtensions)
	{
		const bool listed = std::any_of(extensions.begin(), extensions.end(),
			[&](const char* e) { return strcmp(e, ext) == 0; });
		if (!listed && hasExt(ext))
			extensions.push_back(ext);
	}
	const auto enabledExt = [&](const char* name) {
		return std::any_of(extensions.begin(), extensions.end(),
			[&](const char* e) { return strcmp(e, name) == 0; });
	};

	// Device features. Asking for one the driver does not have is not a
	// warning, it is VK_ERROR_FEATURE_NOT_PRESENT and no device at all - and
	// the frontend then quietly builds its own, which enables none of the
	// extensions asked for above. That is what mobile looked like: every one of
	// these is missing on Adreno and Mali (geometryShader and logicOp in
	// particular), so the negotiation failed on every load and Cemu ran on a
	// device without transform feedback, without custom border colours and
	// without pipeline_creation_cache_control - the last of which is what
	// decides whether pipelines may be compiled asynchronously at all.
	// vkGetPhysicalDeviceFeatures2 is the one this build loads (VulkanAPI.h);
	// its .features member is the same core feature set.
	// It comes back null on a 1.0 instance, and asking for nothing extra is the
	// safe reading of "cannot tell what this GPU has".
	VkPhysicalDeviceFeatures2 supportedFeatures2{};
	supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

	// The extension features go into the same query, for the chain below
	VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pccSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT};
	VkPhysicalDeviceCustomBorderColorFeaturesEXT bcfSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
	VkPhysicalDevicePipelineRobustnessFeaturesEXT robustSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
	VkPhysicalDeviceDepthClipEnableFeaturesEXT clipSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2Supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
	VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vidsSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
	{
		void* chain = nullptr;
		const auto query = [&](auto& feature, const char* ext) {
			if (enabledExt(ext))
			{
				feature.pNext = chain;
				chain = &feature;
			}
		};
		query(pccSupported, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
		query(bcfSupported, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
		query(robustSupported, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
		query(clipSupported, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
		query(edsSupported, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
		query(eds3Supported, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
		query(eds2Supported, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
		query(vidsSupported, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
		if (enabledExt(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME))
			query(gplSupported, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
		supportedFeatures2.pNext = chain;
	}

	if (vkGetPhysicalDeviceFeatures2)
		vkGetPhysicalDeviceFeatures2(gpu, &supportedFeatures2);
	else
	Log(RETRO_LOG_WARN, "cannot query device features, asking for none beyond the frontend's\n");
	const VkPhysicalDeviceFeatures& supported = supportedFeatures2.features;

	VkPhysicalDeviceFeatures features{};
	if (required_features)
		features = *required_features;

	std::string missingFeatures;
	const auto wantFeature = [&](VkBool32 VkPhysicalDeviceFeatures::*member, const char* name) {
		if (supported.*member)
			features.*member = VK_TRUE;
		else
		{
			if (!missingFeatures.empty())
				missingFeatures += ", ";
			missingFeatures += name;
		}
	};
	wantFeature(&VkPhysicalDeviceFeatures::independentBlend, "independentBlend");
	wantFeature(&VkPhysicalDeviceFeatures::samplerAnisotropy, "samplerAnisotropy");
	wantFeature(&VkPhysicalDeviceFeatures::imageCubeArray, "imageCubeArray");
	wantFeature(&VkPhysicalDeviceFeatures::logicOp, "logicOp");
	wantFeature(&VkPhysicalDeviceFeatures::geometryShader, "geometryShader");
	wantFeature(&VkPhysicalDeviceFeatures::occlusionQueryPrecise, "occlusionQueryPrecise");
	wantFeature(&VkPhysicalDeviceFeatures::depthClamp, "depthClamp");
	wantFeature(&VkPhysicalDeviceFeatures::depthBiasClamp, "depthBiasClamp");
	// Transform feedback (streamout) writes from the vertex stage, which a
	// shader may do only with this enabled
	wantFeature(&VkPhysicalDeviceFeatures::vertexPipelineStoresAndAtomics, "vertexPipelineStoresAndAtomics");
	// As standalone: robust buffer access is the fallback without
	// VK_EXT_pipeline_robustness
	if (!(enabledExt(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) && robustSupported.pipelineRobustness) &&
		supported.robustBufferAccess)
		features.robustBufferAccess = VK_TRUE;
	if (!missingFeatures.empty())
		Log(RETRO_LOG_INFO, "this GPU does not have %s - carrying on without them\n", missingFeatures.c_str());

	float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = graphicsFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;

	// The features of the extensions enabled above, those the GPU has - as
	// standalone's constructor chains them. An extension enabled without its
	// feature is one the renderer must not use either.
	VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pccFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT};
	VkPhysicalDeviceCustomBorderColorFeaturesEXT bcfFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
	VkPhysicalDevicePipelineRobustnessFeaturesEXT robustFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
	VkPhysicalDeviceDepthClipEnableFeaturesEXT clipFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
	VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vidsFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
	void* featureChain = nullptr;
	const auto chainFeature = [&](auto& feature) {
		feature.pNext = featureChain;
		featureChain = &feature;
	};
	if (enabledExt(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME) && pccSupported.pipelineCreationCacheControl)
	{
		pccFeatures.pipelineCreationCacheControl = VK_TRUE;
		chainFeature(pccFeatures);
	}
	if (enabledExt(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) && bcfSupported.customBorderColors)
	{
		bcfFeatures.customBorderColors = VK_TRUE;
		bcfFeatures.customBorderColorWithoutFormat = bcfSupported.customBorderColorWithoutFormat;
		chainFeature(bcfFeatures);
	}
	if (enabledExt(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) && robustSupported.pipelineRobustness)
	{
		robustFeatures.pipelineRobustness = VK_TRUE;
		chainFeature(robustFeatures);
	}
	if (enabledExt(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) && clipSupported.depthClipEnable)
	{
		clipFeatures.depthClipEnable = VK_TRUE;
		chainFeature(clipFeatures);
	}
	if (enabledExt(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) && edsSupported.extendedDynamicState)
	{
		edsFeatures.extendedDynamicState = VK_TRUE;
		chainFeature(edsFeatures);
	}
	// the three blend states, all or none: the draw sets them together
	if (enabledExt(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) && eds3Supported.extendedDynamicState3ColorBlendEnable &&
		eds3Supported.extendedDynamicState3ColorBlendEquation && eds3Supported.extendedDynamicState3ColorWriteMask)
	{
		eds3Features.extendedDynamicState3ColorBlendEnable = VK_TRUE;
		eds3Features.extendedDynamicState3ColorBlendEquation = VK_TRUE;
		eds3Features.extendedDynamicState3ColorWriteMask = VK_TRUE;
		chainFeature(eds3Features);
	}
	if (enabledExt(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) && eds2Supported.extendedDynamicState2)
	{
		eds2Features.extendedDynamicState2 = VK_TRUE;
		chainFeature(eds2Features);
	}
	if (enabledExt(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) && vidsSupported.vertexInputDynamicState)
	{
		vidsFeatures.vertexInputDynamicState = VK_TRUE;
		chainFeature(vidsFeatures);
	}
	// libraries only where linking them is fast: a slow link at the draw
	// would be the compile it is there to avoid
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
	if (enabledExt(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && enabledExt(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
		gplSupported.graphicsPipelineLibrary && vkGetPhysicalDeviceProperties2)
	{
		VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		properties2.pNext = &gplProperties;
		vkGetPhysicalDeviceProperties2(gpu, &properties2);
		if (gplProperties.graphicsPipelineLibraryFastLinking)
		{
			gplFeatures.graphicsPipelineLibrary = VK_TRUE;
			chainFeature(gplFeatures);
		}
	}

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = 1;
	createInfo.pQueueCreateInfos = &queueInfo;
	createInfo.enabledExtensionCount = (uint32_t)extensions.size();
	createInfo.ppEnabledExtensionNames = extensions.data();
	createInfo.pEnabledFeatures = &features;
	createInfo.pNext = featureChain;

	VkDevice device = VK_NULL_HANDLE;
	if (create_device_wrapper)
	{
		device = create_device_wrapper(gpu, opaque, &createInfo);
		if (device == VK_NULL_HANDLE)
		{
			Log(RETRO_LOG_ERROR, "the frontend could not create the Vulkan device\n");
			return false;
		}
	}
	else
	{
		VkResult result = vkCreateDevice(gpu, &createInfo, nullptr, &device);
		if (result != VK_SUCCESS)
		{
			Log(RETRO_LOG_ERROR, "Failed to create Vulkan device: %d\n", result);
			return false;
		}
	}

	InitializeDeviceVulkan(device);

	// What the renderer may use (VulkanRenderer's libretro constructor)
	g_libretroVkDeviceExtensions.clear();
	for (const char* ext : extensions)
		g_libretroVkDeviceExtensions.emplace_back(ext);
	g_libretroVkCustomBorderColorWithoutFormat = bcfFeatures.customBorderColorWithoutFormat == VK_TRUE;
	g_libretroVkHasPipelineCreationCacheControl = pccFeatures.pipelineCreationCacheControl == VK_TRUE;
	g_libretroVkHasCustomBorderColors = bcfFeatures.customBorderColors == VK_TRUE;
	g_libretroVkHasPipelineRobustness = robustFeatures.pipelineRobustness == VK_TRUE;
	g_libretroVkHasDepthClipEnable = clipFeatures.depthClipEnable == VK_TRUE;
	g_libretroVkHasExtendedDynamicState = edsFeatures.extendedDynamicState == VK_TRUE;
	g_libretroVkHasExtendedDynamicState3Blend = eds3Features.extendedDynamicState3ColorBlendEnable == VK_TRUE;
	g_libretroVkHasExtendedDynamicState2 = eds2Features.extendedDynamicState2 == VK_TRUE;
	g_libretroVkHasVertexInputDynamicState = vidsFeatures.vertexInputDynamicState == VK_TRUE;
	g_libretroVkHasGraphicsPipelineLibrary = gplFeatures.graphicsPipelineLibrary == VK_TRUE;
	{
		std::string list;
		for (const std::string& ext : g_libretroVkDeviceExtensions)
			list += (list.empty() ? "" : ", ") + ext;
		Log(RETRO_LOG_INFO, "Vulkan device extensions: %s\n", list.c_str());
	}

	VkQueue queue;
	vkGetDeviceQueue(device, graphicsFamily, 0, &queue);

	context->gpu = gpu;
	context->device = device;
	context->queue = queue;
	context->queue_family_index = graphicsFamily;
	context->presentation_queue = queue;
	context->presentation_queue_family_index = graphicsFamily;

	Log(RETRO_LOG_INFO, "Vulkan device created (queue family %u)\n", graphicsFamily);
	return true;
}

static bool CreateDeviceV1(
	struct retro_vulkan_context* context,
	VkInstance instance,
	VkPhysicalDevice gpu,
	VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const char** required_device_extensions,
	unsigned num_required_device_extensions,
	const char** required_device_layers,
	unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	return CreateDevice(context, instance, gpu, surface, required_device_extensions, num_required_device_extensions,
		required_features, nullptr, nullptr);
}

static bool CreateDeviceV2(
	struct retro_vulkan_context* context,
	VkInstance instance,
	VkPhysicalDevice gpu,
	VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper,
	void* opaque)
{
	if (!create_device_wrapper)
		return false;
	return CreateDevice(context, instance, gpu, surface, nullptr, 0, nullptr, create_device_wrapper, opaque);
}

const retro_hw_render_context_negotiation_interface_vulkan* LibretroVk_NegotiationInterface(unsigned frontendVersion, retro_log_printf_t log)
{
	static retro_hw_render_context_negotiation_interface_vulkan s_iface;
	s_log = log;
	s_iface = {};
	s_iface.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
	// A frontend that cannot say which versions it takes takes version 1
	// only: RetroArch 1.14 and older refuse any other
	s_iface.interface_version = frontendVersion >= 2 ? 2 : 1;
	s_iface.get_application_info = GetApplicationInfo;
	s_iface.create_device = CreateDeviceV1;
	// No destroy_device: RetroArch calls it after it has unloaded the core,
	// when this library is no longer there to answer. Without one it destroys
	// the device it got from create_device itself.
	s_iface.destroy_device = nullptr;
	if (s_iface.interface_version >= 2)
	{
		// the frontend makes the instance
		s_iface.create_instance = nullptr;
		s_iface.create_device2 = CreateDeviceV2;
	}
	return &s_iface;
}
