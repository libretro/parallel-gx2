#!/bin/sh
# Fails if a title can be started or stopped from anywhere but the frontend's
# thread: a title's start and stop (CafeSystem's Prepare, Launch and
# Shutdown) called outside CafeSystem itself and the libretro frontend, a
# host thread in coreinit's launching and exit (they ask retro_run to do it,
# as a reset does), or a lock back in the GPU thread's start and stop - which
# one thread alone runs, so needs none.
cd "$(dirname "$0")/.." || exit 2
fail=0

# code only: comment lines out
code()
{
	grep -rnE "$1" $2 --include='*.c' --include='*.cpp' --include='*.h' 2>/dev/null \
		| grep -v -e '^[^:]*:[0-9]*:[[:space:]]*\(\*\|//\|/\*\)'
}

found=$(code 'CafeSystem::(ShutdownTitle|LaunchForegroundTitle|PrepareForegroundTitle[A-Za-z]*)[[:space:]]*\(' src \
	| grep -v -e '^src/Cafe/CafeSystem\.cpp:' -e '^src/libretro/CemuLibretro\.cpp:')
if [ -n "$found" ]; then
	echo "a title started or stopped outside the frontend's thread:"
	printf '%s\n' "$found"
	fail=1
fi

found=$(code 'std::thread|sthread_create' src/Cafe/OS/libs/coreinit/coreinit_Misc.cpp)
if [ -n "$found" ]; then
	echo "coreinit's launching or exit on a thread of its own:"
	printf '%s\n' "$found"
	fail=1
fi

found=$(code 'std::(recursive_)?mutex|std::(unique|scoped)_lock|lock_guard' src/Cafe/HW/Latte/Core/LatteThread.cpp)
if [ -n "$found" ]; then
	echo "a lock in the GPU thread's start and stop:"
	printf '%s\n' "$found"
	fail=1
fi

[ $fail -eq 0 ] && echo "titles start and stop on the frontend's thread alone, the GPU thread's start and stop take no lock"
exit $fail
