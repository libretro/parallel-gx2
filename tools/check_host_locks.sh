#!/bin/sh
# Fails if a host lock (a mutex, a condition variable, a lock guard) is in a
# source file of the core that is not on the list below, or a file on it
# has none left. The list only shrinks: what comes off it has been made
# lock-free, and a lock must not come back there or anywhere else.
cd "$(dirname "$0")/.." || exit 2

allowed='src/Cafe/GraphicPack/GraphicPack2.h
src/Cafe/GraphicPack/GraphicPack2Patches.cpp
src/Cafe/GraphicPack/GraphicPack2PatchesApply.cpp
src/Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64.cpp
src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp
src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp
src/Cafe/IOSU/nn/boss/boss_service.cpp
src/Cafe/OS/libs/coreinit/coreinit_Scheduler.cpp
src/Cafe/OS/libs/nsyshid/BackendLibusb.cpp
src/Cafe/OS/libs/nsyshid/BackendLibusb.h
src/Cemu/napi/napi_act.cpp
src/Cemu/nex/nexFriends.cpp
src/Cemu/nex/nexFriends.h
src/Common/unix/platform.h
src/config/ConfigValue.h
src/config/XMLConfig.h
src/gui/interface/WindowSystem.h
src/libretro/CemuLibretro.cpp
src/util/helpers/ClassWrapper.h
src/util/helpers/ConcurrentQueue.h'

pattern='std::(recursive_|shared_|timed_)?mutex|std::condition_variable|std::(lock_guard|unique_lock|scoped_lock|shared_lock)|slock_new|scond_new|pthread_mutex_lock'

# code only: comment lines out
found=$(grep -rlE "$pattern" src --include='*.c' --include='*.cpp' --include='*.h' 2>/dev/null | sort | while read -r f; do
	if grep -nE "$pattern" "$f" | grep -qv -e '^[0-9]*:[[:space:]]*\(\*\|//\|/\*\)'; then
		echo "$f"
	fi
done)

fail=0
new=$(printf '%s\n' "$found" | grep -vxF "$allowed" | grep -v '^$')
if [ -n "$new" ]; then
	echo "a host lock where there was none:"
	for f in $new; do
		grep -nE "$pattern" "$f" | sed "s#^#$f:#"
	done
	fail=1
fi
gone=$(printf '%s\n' "$allowed" | grep -vxF "$found")
if [ -n "$gone" ]; then
	echo "no host lock left in these: take them off the list in $0"
	printf '%s\n' "$gone"
	fail=1
fi
[ $fail = 0 ] && echo "host locks only where listed ($(printf '%s\n' "$allowed" | wc -l | tr -d ' ') files)"
exit $fail
