#pragma once

// Shared DRC (GamePad) display-layout state used by both the libretro entry
// point (CemuLibretro.cpp) and the renderers that present into the libretro
// window FBO / shared Vulkan image. Defined in CemuLibretro.cpp.


#include "Common/precompiled.h"
#include "LibretroViews.h"

// The layout the renderers draw with and GamePad Position Swapped, in one word
// so a reader gets the two together: written on retro_run's thread, read on
// the GPU thread.
constexpr uint32 kLibretroScreenLayoutMask = 0xFFu;
constexpr uint32 kLibretroScreenSwapped = 1u << 8;
extern std::atomic<uint32> g_libretroScreenState;

inline uint32 LibretroDRC_State()
{
	return g_libretroScreenState.load(std::memory_order_relaxed);
}

inline LibretroScreenLayout LibretroDRC_Layout(uint32 state = LibretroDRC_State())
{
	return (LibretroScreenLayout)(state & kLibretroScreenLayoutMask);
}

inline bool LibretroDRC_Swapped(uint32 state = LibretroDRC_State())
{
	return (state & kLibretroScreenSwapped) != 0;
}

// Counts the times the renderer cleared what the screens had left in the
// frontend's image (a new image, or the screens moved in it). GPU thread only.
uint32 LibretroDRC_PresentClears();
void LibretroDRC_NotePresentClear();

// Returns true when Cemu should actually present the requested screen for the
// active layout. Used both by OpenGLCanvasCallbacks::ShouldRenderScreen and
// by the Vulkan presentation blit.
bool LibretroDRC_ShouldRenderScreen(bool padView);

// Computes the destination viewport (in pixels) for the given screen inside
// a window/image of size dstWidth x dstHeight. Coordinate convention: top-left
// origin (Vulkan-style). The GL path converts to bottom-up Y at the callsite.
void LibretroDRC_ComputeViewport(bool padView,
	int dstWidth, int dstHeight,
	int& outX, int& outY, int& outWidth, int& outHeight);

// Where the screens sit in a composite of dstWidth x dstHeight: its size, then
// the TV's and the GamePad's viewports. The renderers keep what is in the
// composite while this stays the same, and clear it when it changes.
std::array<int, 10> LibretroDRC_Arrangement(int dstWidth, int dstHeight);

// The screen whose size the frame handed to the frontend takes: the GamePad's
// with the GamePad Screen layout, both packed with Both Screens, the TV's with
// every other one.
bool LibretroDRC_OutputFollowsScreen(bool padView);

// Both Screens: the size a picture is drawn at (the GamePad's when padView),
// noted by the renderer as it draws it, and the size of the frame the two are
// packed into.
void LibretroDRC_NoteScreenSize(bool padView, uint32 width, uint32 height);
void LibretroDRC_PackedSize(uint32& width, uint32& height);

// The size the core's output started at (the Internal Resolution option),
// which the TV picture is handed over at unless a resolution pack sets it.
void LibretroDRC_DefaultOutputSize(uint32& width, uint32& height);

// The core's mouse cursor over the GamePad picture (RetroArch draws none).
// Where to draw it in an output image of width x height, top-left origin,
// and how many output pixels one cursor pixel takes; false while hidden or
// off the GamePad picture. Defined in CemuLibretro.cpp.
bool libretro_cursor_position(int width, int height, int& x, int& y, int& scale);
// The arrow, one string per row: 'B' outline, 'W' fill, '.' transparent.
extern const char* const g_libretro_cursor_shape[];
extern const int g_libretro_cursor_rows;
