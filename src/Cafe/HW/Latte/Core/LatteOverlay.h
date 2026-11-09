#pragma once

#include <string>

// Cemu's notifications - an asynchronous shader compile, the friend service,
// the error viewer's text - for the frontend to show. Any thread pushes; the
// frontend's thread takes them once a frame (LatteOverlay_takeNotifications),
// as its message interface wants to be called from there.
void LatteOverlay_pushNotification(const std::string& text, sint32 duration);

// Every notification pushed since the last call, oldest first
template<typename TFn>
void LatteOverlay_takeNotifications(TFn fn);

#include "util/helpers/MpscList.h"

struct LatteOverlayNotification
{
	std::string text;
	sint32 duration;
};

MpscList<LatteOverlayNotification>& LatteOverlay_notifications();

template<typename TFn>
void LatteOverlay_takeNotifications(TFn fn)
{
	LatteOverlay_notifications().TakeAll([&](LatteOverlayNotification&& n) { fn(n.text, n.duration); });
}
