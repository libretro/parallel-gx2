// What PPCTimer.cpp takes from the rest of the core: the log, the timer
// shift setting and the CPU's features
#include <string_view>

#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "Common/cpu_features.h"

uint64 s_loggingFlagMask = 0;

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

uint8 ActiveSettings::GetTimerShiftFactor()
{
	return 3;
}

CPUFeaturesImpl::CPUFeaturesImpl()
{
}

CPUFeaturesImpl g_CPUFeatures;
