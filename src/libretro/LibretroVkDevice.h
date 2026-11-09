#pragma once

#include "libretro.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "libretro/libretro_vulkan.h"

// The negotiation interface to hand the frontend
// (RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE), for a
// frontend that takes interface versions up to frontendVersion (0 when it
// cannot say). log may be null.
const retro_hw_render_context_negotiation_interface_vulkan* LibretroVk_NegotiationInterface(unsigned frontendVersion, retro_log_printf_t log);

// The device extensions, and the extension features, the core enabled on the
// shared device: VulkanRenderer trusts these rather than the GPU's support.
extern std::vector<std::string> g_libretroVkDeviceExtensions;
extern bool g_libretroVkCustomBorderColorWithoutFormat;
extern bool g_libretroVkHasPipelineCreationCacheControl;
extern bool g_libretroVkHasCustomBorderColors;
extern bool g_libretroVkHasPipelineRobustness;
extern bool g_libretroVkHasDepthClipEnable;
