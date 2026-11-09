#!/bin/sh
# Fails if the core's own sources or build files bring in a window system: an
# X11, XCB, Wayland, GL or EGL header, a VK_USE_PLATFORM_* define (which makes
# vulkan.h pull those headers in), or a platform surface entry point. The GL
# context and the Vulkan surface and swapchain are the frontend's under
# libretro; a core that includes these needs headers to build that nothing
# else needs, and has code that can never run.
cd "$(dirname "$0")/.." || exit 2
pattern='#[[:space:]]*include[[:space:]]*[<"](X11/|xcb/|wayland-|GL/|EGL/|GLES[0-9]*/|KHR/)|VK_USE_PLATFORM_|vkCreate(Xlib|Xcb|Wayland|Win32|Android|Metal|MacOS)Surface|(wgl|glX|egl)(CreateContext|MakeCurrent|SwapBuffers|GetProcAddress)'
found=$(grep -rnE "$pattern" src Makefile Makefile.common \
	--include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.mm' \
	--include='Makefile' --include='Makefile.common' 2>/dev/null \
	| grep -v -e '^src/Cafe/HW/Latte/Renderer/Metal/' \
		-e '^[^:]*:[0-9]*:[[:space:]]*\(\*\|//\|# \)')
# Metal: Apple's renderer, which has no window-system headers of its own to
# leave out.
# Comment lines are not code.
if [ -n "$found" ]; then
	echo "window-system code or headers in the core (the frontend owns these):"
	printf '%s\n' "$found"
	exit 1
fi
echo "no window-system headers, defines or surface entry points"
