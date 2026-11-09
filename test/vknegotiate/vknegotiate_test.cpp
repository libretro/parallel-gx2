// The Vulkan context negotiation the core offers the frontend
// (src/libretro/LibretroVkDevice.cpp):
//  - version 2 to a frontend that takes it, with create_device2, and
//    version 1 (create_device alone) to one that cannot say, as RetroArch
//    1.14 and older refuse any other;
//  - through create_device2 the device is made by the frontend's wrapper,
//    given the extensions the core wants, and comes back with what the
//    wrapper added (how the frontend gets the extensions Reflex needs);
//    a wrapper that fails is a failed negotiation;
//  - create_device (version 1) still makes the device itself, with the
//    frontend's required extensions.
// The device part runs on any Vulkan device there is (lavapipe will do),
// and is skipped, said so, without one - a failure with VN_REQUIRE_DEVICE
// set, as the CIs set it.
#include "libretro/LibretroVkDevice.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

static void RETRO_CALLCONV TestLog(enum retro_log_level level, const char* fmt, ...)
{
	if (!getenv("VN_VERBOSE"))
		return;
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static void TestVersions()
{
	const auto* v1 = LibretroVk_NegotiationInterface(0, TestLog);
	CHECK(v1->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN);
	CHECK(v1->interface_version == 1);
	CHECK(v1->get_application_info && v1->get_application_info());
	CHECK(v1->create_device != nullptr);
	CHECK(v1->create_device2 == nullptr && v1->create_instance == nullptr);
	CHECK(LibretroVk_NegotiationInterface(1, TestLog)->interface_version == 1);
	CHECK(LibretroVk_NegotiationInterface(1, TestLog)->create_device2 == nullptr);
	const auto* v2 = LibretroVk_NegotiationInterface(2, TestLog);
	CHECK(v2->interface_version == 2);
	CHECK(v2->create_device != nullptr && v2->create_device2 != nullptr);
	CHECK(v2->destroy_device == nullptr);
	CHECK(LibretroVk_NegotiationInterface(3, TestLog)->interface_version == 2);
}

struct Wrapper
{
	int calls = 0;
	bool fail = false;
	std::vector<std::string> asked;
	const char* added = nullptr;
	VkDevice made = VK_NULL_HANDLE;
};

static VkDevice WrapperCreate(VkPhysicalDevice gpu, void* opaque, const VkDeviceCreateInfo* info)
{
	Wrapper* w = (Wrapper*)opaque;
	w->calls++;
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
		w->asked.emplace_back(info->ppEnabledExtensionNames[i]);
	if (w->fail)
		return VK_NULL_HANDLE;
	// as RetroArch's does: the core's extensions and the frontend's own
	std::vector<const char*> exts(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
	if (w->added)
		exts.push_back(w->added);
	VkDeviceCreateInfo ci = *info;
	ci.enabledExtensionCount = (uint32_t)exts.size();
	ci.ppEnabledExtensionNames = exts.data();
	if (vkCreateDevice(gpu, &ci, nullptr, &w->made) != VK_SUCCESS)
		w->made = VK_NULL_HANDLE;
	return w->made;
}

static bool Has(const std::vector<std::string>& list, const char* name)
{
	return std::find(list.begin(), list.end(), name) != list.end();
}

static void Skip(const char* why)
{
	std::printf("skip: %s, the device part did not run\n", why);
	if (getenv("VN_REQUIRE_DEVICE"))
	{
		std::printf("FAIL: VN_REQUIRE_DEVICE is set\n");
		s_failures++;
	}
}

static void TestDevices()
{
	if (!InitializeGlobalVulkan())
	{
		Skip("no Vulkan loader");
		return;
	}
	const auto* iface = LibretroVk_NegotiationInterface(2, TestLog);
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = iface->get_application_info();
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS || !InitializeInstanceVulkan(instance))
	{
		Skip("no Vulkan instance");
		return;
	}
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	if (!count)
	{
		Skip("no Vulkan device");
		vkDestroyInstance(instance, nullptr);
		return;
	}
	std::vector<VkPhysicalDevice> gpus(count);
	vkEnumeratePhysicalDevices(instance, &count, gpus.data());
	VkPhysicalDevice gpu = gpus[0];
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> exts(extCount);
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, exts.data());

	// version 2: through the frontend's wrapper, which adds one of its own
	{
		Wrapper w;
		retro_vulkan_context ctx{};
		CHECK(iface->create_device2(&ctx, instance, gpu, VK_NULL_HANDLE, vkGetInstanceProcAddr, WrapperCreate, &w));
		CHECK(w.calls == 1);
		CHECK(ctx.device != VK_NULL_HANDLE && ctx.device == w.made);
		CHECK(ctx.gpu == gpu && ctx.queue != VK_NULL_HANDLE && ctx.presentation_queue == ctx.queue);
		// what the core wants went to the wrapper, and the renderer is told
		// what the core enabled
		for (const std::string& e : g_libretroVkDeviceExtensions)
			CHECK(Has(w.asked, e.c_str()));
		// cull, front face, depth and stencil as dynamic state where the GPU
		// has it: on, with its feature
		bool gpuEds = false;
		for (auto& e : exts)
			gpuEds = gpuEds || !strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
		if (gpuEds)
			CHECK(g_libretroVkHasExtendedDynamicState && Has(w.asked, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME));
		std::printf("extended dynamic state: %s\n", gpuEds ? (g_libretroVkHasExtendedDynamicState ? "on" : "NOT ON") : "the GPU has none");
		// blending as dynamic state where the GPU has all three of its features
		bool gpuEds3 = false;
		for (auto& e : exts)
			gpuEds3 = gpuEds3 || !strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
		bool gpuEds3Blend = false;
		if (gpuEds3)
		{
			VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
			VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			f2.pNext = &f3;
			vkGetPhysicalDeviceFeatures2(gpu, &f2);
			gpuEds3Blend = f3.extendedDynamicState3ColorBlendEnable && f3.extendedDynamicState3ColorBlendEquation && f3.extendedDynamicState3ColorWriteMask;
		}
		if (gpuEds3Blend)
			CHECK(g_libretroVkHasExtendedDynamicState3Blend && Has(w.asked, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
		else
			CHECK(!g_libretroVkHasExtendedDynamicState3Blend);
		std::printf("dynamic blending: %s\n", gpuEds3Blend ? (g_libretroVkHasExtendedDynamicState3Blend ? "on" : "NOT ON") : "the GPU has none");
		// depth bias enable and rasterizer discard as dynamic state where the
		// GPU has _2's feature
		bool gpuEds2 = false;
		for (auto& e : exts)
			gpuEds2 = gpuEds2 || !strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
		if (gpuEds2)
		{
			VkPhysicalDeviceExtendedDynamicState2FeaturesEXT f2e{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
			VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			f2.pNext = &f2e;
			vkGetPhysicalDeviceFeatures2(gpu, &f2);
			gpuEds2 = f2e.extendedDynamicState2 == VK_TRUE;
		}
		if (gpuEds2)
			CHECK(g_libretroVkHasExtendedDynamicState2 && Has(w.asked, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME));
		else
			CHECK(!g_libretroVkHasExtendedDynamicState2);
		std::printf("dynamic depth bias and discard: %s\n", gpuEds2 ? (g_libretroVkHasExtendedDynamicState2 ? "on" : "NOT ON") : "the GPU has none");
		// the vertex input as dynamic state where the GPU has the feature
		bool gpuVi = false;
		for (auto& e : exts)
			gpuVi = gpuVi || !strcmp(e.extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
		if (gpuVi)
		{
			VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
			VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			f2.pNext = &fvi;
			vkGetPhysicalDeviceFeatures2(gpu, &f2);
			gpuVi = fvi.vertexInputDynamicState == VK_TRUE;
		}
		if (gpuVi)
			CHECK(g_libretroVkHasVertexInputDynamicState && Has(w.asked, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME));
		else
			CHECK(!g_libretroVkHasVertexInputDynamicState);
		std::printf("dynamic vertex input: %s\n", gpuVi ? (g_libretroVkHasVertexInputDynamicState ? "on" : "NOT ON") : "the GPU has none");
		// graphics pipeline libraries where the GPU has them and links them fast
		bool gpuGpl = false, gpuLib = false;
		for (auto& e : exts)
		{
			gpuGpl = gpuGpl || !strcmp(e.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
			gpuLib = gpuLib || !strcmp(e.extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
		}
		gpuGpl = gpuGpl && gpuLib;
		if (gpuGpl)
		{
			VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
			VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			f2.pNext = &fg;
			vkGetPhysicalDeviceFeatures2(gpu, &f2);
			VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
			VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			p2.pNext = &pg;
			vkGetPhysicalDeviceProperties2(gpu, &p2);
			gpuGpl = fg.graphicsPipelineLibrary == VK_TRUE && pg.graphicsPipelineLibraryFastLinking == VK_TRUE;
		}
		if (gpuGpl)
			CHECK(g_libretroVkHasGraphicsPipelineLibrary && Has(w.asked, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
				Has(w.asked, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME));
		else
			CHECK(!g_libretroVkHasGraphicsPipelineLibrary);
		std::printf("graphics pipeline libraries: %s\n", gpuGpl ? (g_libretroVkHasGraphicsPipelineLibrary ? "on" : "NOT ON") : "the GPU has none with fast linking");
		const char* own = nullptr;
		for (auto& e : exts)
			if (!Has(w.asked, e.extensionName))
			{
				own = e.extensionName;
				break;
			}
		if (ctx.device)
			vkDestroyDevice(ctx.device, nullptr);
		// again with an extension of the frontend's the core did not ask for:
		// the device has it
		if (own)
		{
			Wrapper w2;
			w2.added = own;
			retro_vulkan_context ctx2{};
			CHECK(iface->create_device2(&ctx2, instance, gpu, VK_NULL_HANDLE, vkGetInstanceProcAddr, WrapperCreate, &w2));
			CHECK(w2.calls == 1 && ctx2.device == w2.made && ctx2.device != VK_NULL_HANDLE);
			CHECK(!Has(w2.asked, own));
			if (ctx2.device)
				vkDestroyDevice(ctx2.device, nullptr);
		}
		// no GPU named: the core picks one, through the wrapper all the same
		Wrapper w3;
		retro_vulkan_context ctx3{};
		CHECK(iface->create_device2(&ctx3, instance, VK_NULL_HANDLE, VK_NULL_HANDLE, vkGetInstanceProcAddr, WrapperCreate, &w3));
		CHECK(w3.calls == 1 && ctx3.device == w3.made && ctx3.gpu != VK_NULL_HANDLE);
		if (ctx3.device)
			vkDestroyDevice(ctx3.device, nullptr);
	}
	// a wrapper that fails: no device, a failed negotiation
	{
		Wrapper w;
		w.fail = true;
		retro_vulkan_context ctx{};
		CHECK(!iface->create_device2(&ctx, instance, gpu, VK_NULL_HANDLE, vkGetInstanceProcAddr, WrapperCreate, &w));
		CHECK(w.calls == 1 && ctx.device == VK_NULL_HANDLE);
	}
	// version 1: the core makes it, with the frontend's required extensions
	{
		const auto* old = LibretroVk_NegotiationInterface(0, TestLog);
		std::vector<const char*> required;
		for (auto& e : exts)
			if (!strcmp(e.extensionName, VK_KHR_MAINTENANCE1_EXTENSION_NAME) || !strcmp(e.extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))
				required.push_back(e.extensionName);
		retro_vulkan_context ctx{};
		CHECK(old->create_device(&ctx, instance, gpu, VK_NULL_HANDLE, vkGetInstanceProcAddr,
			required.data(), (unsigned)required.size(), nullptr, 0, nullptr));
		CHECK(ctx.device != VK_NULL_HANDLE && ctx.queue != VK_NULL_HANDLE);
		for (const char* r : required)
			CHECK(Has(g_libretroVkDeviceExtensions, r));
		if (ctx.device)
			vkDestroyDevice(ctx.device, nullptr);
	}
	vkDestroyInstance(instance, nullptr);
	std::printf("devices made on %u GPU(s), the first used\n", count);
}

int main()
{
	TestVersions();
	TestDevices();
	if (s_failures)
	{
		std::printf("%d failed\n", s_failures);
		return 1;
	}
	std::printf("vknegotiate: all passed\n");
	return 0;
}
