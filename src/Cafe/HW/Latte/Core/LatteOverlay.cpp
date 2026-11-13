#include "Cafe/HW/Latte/Core/LatteOverlay.h"

#include <compat/strl.h>

FixedList<LatteOverlayNotification, 64>& LatteOverlay_notifications()
{
	static FixedList<LatteOverlayNotification, 64> s_notifications;
	return s_notifications;
}

void LatteOverlay_pushNotification(const std::string& text, sint32 duration)
{
	LatteOverlayNotification n;
	strlcpy(n.text, text.c_str(), sizeof(n.text));
	n.duration = duration;
	LatteOverlay_notifications().Push(n);
}
