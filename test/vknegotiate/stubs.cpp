// What LibretroVkDevice.cpp and VulkanAPI.cpp take from the rest of the core
#include <string_view>

#include "Cemu/Logging/CemuLogging.h"

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}
