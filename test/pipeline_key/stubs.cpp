// What VulkanPipelineKey.cpp and VulkanAPI.cpp take from the rest of the core
#include <string_view>

#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

// the key reads fetch shaders the test builds; none is in a cache
LatteFetchShader::~LatteFetchShader()
{
}
