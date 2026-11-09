#include "Cafe/HW/Latte/Core/LatteOverlay.h"

MpscList<LatteOverlayNotification>& LatteOverlay_notifications()
{
	static MpscList<LatteOverlayNotification> s_notifications;
	return s_notifications;
}

void LatteOverlay_pushNotification(const std::string& text, sint32 duration)
{
	LatteOverlay_notifications().Push({text, duration});
}
