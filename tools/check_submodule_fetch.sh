#!/bin/sh
# Fails if a checkout without its git submodules (a clone without
# --recursive) does not get them from its first make, which otherwise stops
# at the first header it cannot find (glm/glm.hpp, in the precompiled
# header). Checks a clone of this checkout's HEAD made without them.
cd "$(dirname "$0")/.." || exit 2
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
git clone -q --no-recurse-submodules . "$tmp/core" || exit 2
# the submodules' own URLs, not this clone's path
git -C "$tmp/core" config -f .gitmodules --get-regexp '\.url$' >/dev/null || exit 2
if [ -e "$tmp/core/dependencies/glm/glm/glm.hpp" ]; then
	echo "the clone already has its submodules"
	exit 2
fi
make -C "$tmp/core" -n OBJ_DIR="$tmp/obj" >/dev/null 2>"$tmp/err"
status=0
for f in dependencies/glm/glm/glm.hpp dependencies/fmt/include/fmt/core.h \
	dependencies/Vulkan-Headers/include/vulkan/vulkan.h; do
	if [ ! -e "$tmp/core/$f" ]; then
		echo "not fetched: $f"
		status=1
	fi
done
[ $status -ne 0 ] && cat "$tmp/err"
# and a second make leaves them as they are
if [ $status -eq 0 ] && make -C "$tmp/core" -n OBJ_DIR="$tmp/obj" 2>&1 >/dev/null | grep -q "Fetching the git submodules"; then
	echo "the second make fetched again"
	status=1
fi
[ $status -eq 0 ] && echo "ok: a checkout without submodules gets them from make"
exit $status
