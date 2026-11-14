#!/bin/sh
# Under libretro the emulated controllers are updated once a frame from
# retro_run, right after the frontend's input is polled: no input thread
# waking every millisecond to update them in between.
#
# Both sources are run through the preprocessor as the libretro build sees
# them (includes dropped, so only this file's own conditionals decide), and
# the code that survives is checked.
cd "$(dirname "$0")/.." || exit 1
CXX=${CXX:-c++}
fail=0

pp()
{
	sed '/^[[:space:]]*#[[:space:]]*include/d' "$1" |
		$CXX -E -P -DENABLE_LIBRETRO -x c++ - 2>/dev/null
}

# the body of the function whose definition line matches $1
body()
{
	awk -v start="$1" '
		!on && $0 ~ start { on = 1 }
		on {
			print
			n += gsub(/\{/, "{"); n -= gsub(/\}/, "}")
			if (seen && n == 0) exit
			if (n > 0) seen = 1
		}'
}

ctor=$(pp src/input/InputManager.cpp | body '^InputManager::InputManager[(][)]')
if [ -z "$ctor" ]; then
	echo "InputManager::InputManager() not found"; fail=1
elif echo "$ctor" | grep -q 'std::thread'; then
	echo "InputManager starts a thread under libretro:"; echo "$ctor"; fail=1
fi

run=$(pp src/libretro/CemuLibretro.cpp | body '(^|[^_a-zA-Z0-9])retro_run[(][)]')
if [ -z "$run" ]; then
	echo "retro_run() not found"; fail=1
elif ! echo "$run" | awk '
		/libretro_poll_input[(][)]/ { polled = 1 }
		/update_controllers[(][)]/ { if (polled) ok = 1 }
		END { exit !ok }'; then
	echo "retro_run does not update the controllers after polling input"; fail=1
fi

[ $fail -eq 0 ] && echo "controllers updated once a frame from retro_run, no input thread"
exit $fail
