#pragma once

// The screen layouts' arithmetic that needs nothing of the emulator: where the
// Both Screens layout packs the TV and GamePad pictures, which layout a frame
// is drawn with while the frontend shows both eyes, and the video views map
// that describes a frame to the frontend (RETRO_ENVIRONMENT_SET_VIDEO_VIEWS).
// Apart from CemuLibretro.cpp so test/video_views can check it on its own.

#include <algorithm>
#include <cstdint>

#include "libretro.h"

enum class LibretroScreenLayout
{
	Tv,               // TV only
	GamePad,          // GamePad only
	SideBySide,       // primary 80% + secondary 20%, side-by-side
	TopBottom,        // primary 70% on top, secondary 30% on bottom
	PictureInPicture, // primary fullscreen, secondary as small corner overlay
	BothScreens,      // both at their own size, the first above the second (LibretroViews_PlaceBoth)
};

struct LibretroScreenRect
{
	int x = 0, y = 0, width = 0, height = 0;
};

// The frame Both Screens packs two pictures into: as wide as the wider of
// them, as tall as both.
inline void LibretroViews_PackedSize(uint32_t firstW, uint32_t firstH, uint32_t secondW, uint32_t secondH,
	uint32_t& width, uint32_t& height)
{
	width = std::max(firstW, secondW);
	height = firstH + secondH;
}

// Where Both Screens puts the two pictures in a frame of frameW x frameH: the
// first on top, the second under it, each centred across. A frame that is not
// the packed size - a picture's size changed in the middle of a frame, and the
// frame is resized by the next one - gets the packing scaled into it, so both
// screens stay inside it and meet edge to edge.
inline void LibretroViews_PlaceBoth(uint32_t frameW, uint32_t frameH,
	uint32_t firstW, uint32_t firstH, uint32_t secondW, uint32_t secondH,
	LibretroScreenRect& first, LibretroScreenRect& second)
{
	uint32_t packedW = 0, packedH = 0;
	LibretroViews_PackedSize(firstW, firstH, secondW, secondH, packedW, packedH);
	if (packedW == 0 || packedH == 0)
	{
		first = second = LibretroScreenRect{};
		return;
	}
	// An edge of the packing, in the frame's pixels
	const auto sx = [&](uint32_t v) { return (int)(((uint64_t)v * frameW + packedW / 2) / packedW); };
	const auto sy = [&](uint32_t v) { return (int)(((uint64_t)v * frameH + packedH / 2) / packedH); };
	const uint32_t firstX = (packedW - firstW) / 2, secondX = (packedW - secondW) / 2;
	first = {sx(firstX), 0, sx(firstX + firstW) - sx(firstX), sy(firstH)};
	second = {sx(secondX), sy(firstH), sx(secondX + secondW) - sx(secondX), sy(packedH) - sy(firstH)};
}

// How many screens a frame drawn with this layout shows the frontend as views:
// one for a layout of one screen, two for Both Screens, and none for the
// layouts that draw one picture beside, under or over the other at sizes of
// their own, which no frontend arrangement reproduces.
inline unsigned LibretroViews_ScreenCount(LibretroScreenLayout layout)
{
	switch (layout)
	{
	case LibretroScreenLayout::Tv:
	case LibretroScreenLayout::GamePad:
		return 1;
	case LibretroScreenLayout::BothScreens:
		return 2;
	case LibretroScreenLayout::SideBySide:
	case LibretroScreenLayout::TopBottom:
	case LibretroScreenLayout::PictureInPicture:
		break;
	}
	return 0;
}

// The layout a frame is drawn with: the one chosen, except that while the
// frontend shows both eyes (its stereo modes, a headset) the layouts that
// composite the two screens hand them over as Both Screens, as the Nintendo DS
// and 3DS cores do there. viewsStatus: RETRO_VIDEO_VIEWS_STATUS_ flags.
inline LibretroScreenLayout LibretroViews_EffectiveLayout(LibretroScreenLayout chosen, unsigned viewsStatus)
{
	if ((viewsStatus & RETRO_VIDEO_VIEWS_STATUS_STEREO) && LibretroViews_ScreenCount(chosen) == 0)
		return LibretroScreenLayout::BothScreens;
	return chosen;
}

// What one frame handed to the frontend shows: its size, and where each of
// its screens is in it, screen 0 first. count is 0 for a frame with no screens
// to describe (LibretroViews_ScreenCount).
struct LibretroFrameScreens
{
	uint32_t width = 0, height = 0;
	uint32_t count = 0;
	LibretroScreenRect screens[2];
};

// The screens of a frame of frameW x frameH drawn with this layout. tv/pad:
// the size each picture is drawn at, which Both Screens packs; padFirst: the
// GamePad comes first (GamePad Position Swapped), and is screen 0.
inline LibretroFrameScreens LibretroViews_FrameScreens(LibretroScreenLayout layout, bool padFirst,
	uint32_t frameW, uint32_t frameH, uint32_t tvW, uint32_t tvH, uint32_t padW, uint32_t padH)
{
	LibretroFrameScreens frame;
	frame.width = frameW;
	frame.height = frameH;
	frame.count = LibretroViews_ScreenCount(layout);
	if (frame.count == 1)
		frame.screens[0] = {0, 0, (int)frameW, (int)frameH};
	else if (frame.count == 2 && padFirst)
		LibretroViews_PlaceBoth(frameW, frameH, padW, padH, tvW, tvH, frame.screens[0], frame.screens[1]);
	else if (frame.count == 2)
		LibretroViews_PlaceBoth(frameW, frameH, tvW, tvH, padW, padH, frame.screens[0], frame.screens[1]);
	return frame;
}

// The views map for a frame, written into out: the number of views, or 0 when
// the frame has no screens to describe or one of them is not inside it.
inline unsigned LibretroViews_BuildMap(const LibretroFrameScreens& frame, retro_video_view (&out)[2])
{
	if (frame.count == 0 || frame.count > 2)
		return 0;
	for (unsigned i = 0; i < frame.count; i++)
	{
		const LibretroScreenRect& r = frame.screens[i];
		if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
			(uint64_t)r.x + (uint64_t)r.width > frame.width || (uint64_t)r.y + (uint64_t)r.height > frame.height)
			return 0;
		out[i] = {};
		out[i].x = (unsigned)r.x;
		out[i].y = (unsigned)r.y;
		out[i].width = (unsigned)r.width;
		out[i].height = (unsigned)r.height;
		out[i].screen = i;
		out[i].eye = RETRO_VIDEO_VIEW_EYE_NONE;
		// The TV and the GamePad are both 16:9 screens, whatever size a
		// resolution pack draws them at
		out[i].aspect_ratio = 16.0f / 9.0f;
	}
	return frame.count;
}

// The parts of box that no rectangle in cover overlaps, as disjoint
// rectangles in out: the count. Two covers, the screens of a frame, cut a box
// into at most 16 pieces; covers past the second are not looked at.
inline unsigned LibretroViews_Uncovered(const LibretroScreenRect& box, const LibretroScreenRect* cover,
	unsigned coverCount, LibretroScreenRect (&out)[16])
{
	unsigned count = 0;
	if (box.width > 0 && box.height > 0)
		out[count++] = box;
	for (unsigned c = 0; c < coverCount && c < 2; c++)
	{
		if (cover[c].width <= 0 || cover[c].height <= 0)
			continue;
		const int cx0 = cover[c].x, cy0 = cover[c].y;
		const int cx1 = cx0 + cover[c].width, cy1 = cy0 + cover[c].height;
		LibretroScreenRect left[16];
		unsigned n = 0;
		for (unsigned i = 0; i < count; i++)
		{
			const LibretroScreenRect p = out[i];
			const int px1 = p.x + p.width, py1 = p.y + p.height;
			if (cx1 <= p.x || cx0 >= px1 || cy1 <= p.y || cy0 >= py1)
			{
				left[n++] = p;
				continue;
			}
			// Above and below the cover across the piece, then left and right
			// of it between those
			const int y0 = std::max(p.y, cy0), y1 = std::min(py1, cy1);
			if (p.y < y0)
				left[n++] = {p.x, p.y, p.width, y0 - p.y};
			if (y1 < py1)
				left[n++] = {p.x, y1, p.width, py1 - y1};
			if (p.x < cx0)
				left[n++] = {p.x, y0, cx0 - p.x, y1 - y0};
			if (cx1 < px1)
				left[n++] = {cx1, y0, px1 - cx1, y1 - y0};
		}
		std::copy(left, left + n, out);
		count = n;
	}
	return count;
}
