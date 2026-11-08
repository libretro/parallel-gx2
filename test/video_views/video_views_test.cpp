// The screen layouts' arithmetic (LibretroViews.h), each tested for what its
// users rely on:
//  - Both Screens packs the TV and the GamePad one under the other at their
//    own sizes, the narrower centred, swapped or not, at every Internal
//    Resolution from 0.25x to 3x (which resizes the TV and leaves the
//    GamePad's 854x480 alone), the GamePad the wider one below 0.75x;
//  - a frame that is not yet the packed size (a picture's size changed in the
//    middle of a frame) still has both screens inside it, edge to edge;
//  - the layouts that composite the two screens become Both Screens only while
//    the frontend shows both eyes;
//  - a frame's views map has one view per screen, numbered from 0, and none
//    for a frame that has no screens to describe or whose screens do not fit;
//  - what is left of the mouse cursor's box after the screens blitted in a
//    frame (what the Vulkan renderer puts back) is every pixel of the box no
//    screen covers, each once, and nothing else.
#include "libretro/LibretroViews.h"

#include <cstdio>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

using L = LibretroScreenLayout;

static bool same(const LibretroScreenRect& r, int x, int y, int w, int h)
{
	return r.x == x && r.y == y && r.width == w && r.height == h;
}

static void test_packed_size()
{
	uint32_t w = 0, h = 0;
	LibretroViews_PackedSize(1280, 720, 854, 480, w, h);
	CHECK(w == 1280 && h == 1200);
	LibretroViews_PackedSize(3840, 2160, 2562, 1440, w, h);
	CHECK(w == 3840 && h == 3600);
	// The GamePad first: still as wide as the TV
	LibretroViews_PackedSize(854, 480, 1280, 720, w, h);
	CHECK(w == 1280 && h == 1200);
}

static void test_place_both()
{
	LibretroScreenRect a, b;
	LibretroViews_PlaceBoth(1280, 1200, 1280, 720, 854, 480, a, b);
	CHECK(same(a, 0, 0, 1280, 720));
	CHECK(same(b, 213, 720, 854, 480));
	// GamePad Position Swapped: the GamePad first, centred on top
	LibretroViews_PlaceBoth(1280, 1200, 854, 480, 1280, 720, a, b);
	CHECK(same(a, 213, 0, 854, 480));
	CHECK(same(b, 0, 480, 1280, 720));
	// A frame still 1280x720 while 1280x1200 is wanted: scaled into it, edge to
	// edge, nothing outside
	LibretroViews_PlaceBoth(1280, 720, 1280, 720, 854, 480, a, b);
	CHECK(same(a, 0, 0, 1280, 432));
	CHECK(same(b, 213, 432, 854, 288));
	// Nothing to pack
	LibretroViews_PlaceBoth(1280, 720, 0, 0, 0, 0, a, b);
	CHECK(same(a, 0, 0, 0, 0) && same(b, 0, 0, 0, 0));
}

static void test_effective_layout()
{
	const unsigned flat = RETRO_VIDEO_VIEWS_STATUS_PRESENTS;
	const unsigned stereo = RETRO_VIDEO_VIEWS_STATUS_PRESENTS | RETRO_VIDEO_VIEWS_STATUS_STEREO;
	for (L l : {L::SideBySide, L::TopBottom, L::PictureInPicture})
	{
		CHECK(LibretroViews_EffectiveLayout(l, 0) == l);
		CHECK(LibretroViews_EffectiveLayout(l, flat) == l);
		CHECK(LibretroViews_EffectiveLayout(l, stereo) == L::BothScreens);
	}
	for (L l : {L::Tv, L::GamePad, L::BothScreens})
	{
		CHECK(LibretroViews_EffectiveLayout(l, 0) == l);
		CHECK(LibretroViews_EffectiveLayout(l, stereo) == l);
	}
}

static void test_frame_screens_and_map()
{
	retro_video_view v[2];

	// One screen: the whole frame, screen 0
	LibretroFrameScreens f = LibretroViews_FrameScreens(L::GamePad, false, 854, 480, 1280, 720, 854, 480);
	CHECK(LibretroViews_BuildMap(f, v) == 1);
	CHECK(v[0].x == 0 && v[0].y == 0 && v[0].width == 854 && v[0].height == 480);
	CHECK(v[0].screen == 0 && v[0].eye == RETRO_VIDEO_VIEW_EYE_NONE);
	f = LibretroViews_FrameScreens(L::Tv, true, 1280, 720, 1280, 720, 854, 480);
	CHECK(LibretroViews_BuildMap(f, v) == 1);
	CHECK(v[0].width == 1280 && v[0].height == 720 && v[0].screen == 0);

	// Both Screens: the TV is screen 0 on top, the GamePad screen 1 under it,
	// both 16:9
	f = LibretroViews_FrameScreens(L::BothScreens, false, 1280, 1200, 1280, 720, 854, 480);
	CHECK(LibretroViews_BuildMap(f, v) == 2);
	CHECK(v[0].x == 0 && v[0].y == 0 && v[0].width == 1280 && v[0].height == 720 && v[0].screen == 0);
	CHECK(v[1].x == 213 && v[1].y == 720 && v[1].width == 854 && v[1].height == 480 && v[1].screen == 1);
	CHECK(v[0].aspect_ratio == 16.0f / 9.0f && v[1].aspect_ratio == 16.0f / 9.0f);
	CHECK(v[0].eye == RETRO_VIDEO_VIEW_EYE_NONE && v[1].eye == RETRO_VIDEO_VIEW_EYE_NONE);

	// Swapped: the GamePad is screen 0, on top
	f = LibretroViews_FrameScreens(L::BothScreens, true, 1280, 1200, 1280, 720, 854, 480);
	CHECK(LibretroViews_BuildMap(f, v) == 2);
	CHECK(v[0].width == 854 && v[0].y == 0 && v[0].screen == 0);
	CHECK(v[1].width == 1280 && v[1].y == 480 && v[1].screen == 1);

	// The composite layouts: no map
	for (L l : {L::SideBySide, L::TopBottom, L::PictureInPicture})
		CHECK(LibretroViews_BuildMap(LibretroViews_FrameScreens(l, false, 1280, 720, 1280, 720, 854, 480), v) == 0);

	// A screen outside its frame: no map
	f = LibretroViews_FrameScreens(L::BothScreens, false, 1280, 1200, 1280, 720, 854, 480);
	f.height = 1000;
	CHECK(LibretroViews_BuildMap(f, v) == 0);

	// Every Internal Resolution step, swapped or not, packs inside its frame
	for (uint32_t quarter = 1; quarter <= 12; quarter++)
	{
		const uint32_t tvW = 1280 * quarter / 4, tvH = 720 * quarter / 4;
		const uint32_t padW = 854, padH = 480;
		uint32_t w = 0, h = 0;
		LibretroViews_PackedSize(tvW, tvH, padW, padH, w, h);
		for (bool swapped : {false, true})
		{
			f = LibretroViews_FrameScreens(L::BothScreens, swapped, w, h, tvW, tvH, padW, padH);
			CHECK(LibretroViews_BuildMap(f, v) == 2);
			CHECK(v[0].height + v[1].height == h);
			CHECK(v[swapped ? 1 : 0].width == tvW && v[swapped ? 0 : 1].width == padW);
		}
	}
}

// Every pixel of a 24x24 field: in the pieces exactly once where the box has
// it and no cover does, and in none elsewhere
static bool uncovered_exact(const LibretroScreenRect& box, const LibretroScreenRect* cover, unsigned coverCount)
{
	LibretroScreenRect out[16];
	const unsigned n = LibretroViews_Uncovered(box, cover, coverCount, out);
	const auto inside = [](const LibretroScreenRect& r, int x, int y)
	{
		return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
	};
	for (unsigned i = 0; i < n; i++)
		if (out[i].width <= 0 || out[i].height <= 0)
			return false;
	for (int y = -2; y < 22; y++)
		for (int x = -2; x < 22; x++)
		{
			bool want = inside(box, x, y);
			for (unsigned c = 0; c < coverCount; c++)
				want = want && !inside(cover[c], x, y);
			unsigned hits = 0;
			for (unsigned i = 0; i < n; i++)
				hits += inside(out[i], x, y) ? 1 : 0;
			if (hits != (want ? 1u : 0u))
				return false;
		}
	return true;
}

static void test_uncovered()
{
	const LibretroScreenRect box{4, 4, 10, 8};
	LibretroScreenRect out[16];

	// No screen blitted: the whole box
	CHECK(LibretroViews_Uncovered(box, nullptr, 0, out) == 1 && same(out[0], 4, 4, 10, 8));

	// The box inside a screen scanned out again: nothing to copy
	const LibretroScreenRect screen{0, 0, 20, 20};
	CHECK(LibretroViews_Uncovered(box, &screen, 1, out) == 0);

	// Over the edge between Both Screens' TV and GamePad, only the TV
	// scanned out: the GamePad's part
	const LibretroScreenRect tv{0, 0, 20, 8};
	CHECK(LibretroViews_Uncovered(box, &tv, 1, out) == 1 && same(out[0], 4, 8, 10, 4));

	// Every placement of two covers against the box, edges, corners and
	// holes included
	unsigned seed = 12345;
	const auto next = [&seed](int range) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 16) % (unsigned)range); };
	for (int i = 0; i < 20000; i++)
	{
		const LibretroScreenRect b{next(12), next(12), next(10), next(10)};
		const LibretroScreenRect c[2] = {
			{next(20) - 2, next(20) - 2, next(14), next(14)},
			{next(20) - 2, next(20) - 2, next(14), next(14)},
		};
		CHECK(uncovered_exact(b, c, (unsigned)next(3)));
	}
}

int main()
{
	test_uncovered();
	test_packed_size();
	test_place_both();
	test_effective_layout();
	test_frame_screens_and_map();
	if (s_failures)
	{
		std::fprintf(stderr, "video_views: %d failed\n", s_failures);
		return 1;
	}
	std::printf("video_views: all passed\n");
	return 0;
}
