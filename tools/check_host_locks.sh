#!/bin/sh
# Fails if a host lock (a mutex, a condition variable, a lock guard, a
# platform's own: pthreads', a Windows critical section or SRW lock) is in a
# source file of the core that is not on the list below, or a file on it
# has none left. The list only shrinks: what comes off it has been made
# lock-free, and a lock must not come back there or anywhere else.
cd "$(dirname "$0")/.." || exit 2

allowed='src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp
src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp
src/Cafe/IOSU/nn/boss/boss_service.cpp
src/Cafe/OS/libs/coreinit/coreinit_Scheduler.cpp
src/Cemu/napi/napi_act.cpp
src/Cemu/nex/nexFriends.cpp
src/Cemu/nex/nexFriends.h
src/util/helpers/ConcurrentQueue.h'

pattern='std::(recursive_|shared_|timed_)?mutex|std::condition_variable|std::(lock_guard|unique_lock|scoped_lock|shared_lock)|slock_(new|lock)|scond_(new|wait)|pthread_(mutex|rwlock|cond)_|AcquireSRWLock|EnterCriticalSection'

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
