#!/bin/sh
# Fails if the build writes a path make made absolute into a file the
# compiler reads. Under MSYS2, make's absolute paths are /c/... ones: MSYS
# converts them on a native compiler's command line, never inside a file,
# and MinGW's g++ cannot open them (the precompiled header's stub broke the
# Windows build that way). The stub must reach the header through the
# include path, and the build files take no absolute paths from make.
cd "$(dirname "$0")/.." || exit 2
status=0
found=$(grep -nE '\$\((abspath|realpath)[[:space:]]|\$\((CURDIR|PWD)\)' Makefile Makefile.common)
if [ -n "$found" ]; then
	echo "make-made absolute paths in the build files:"
	echo "$found"
	status=1
fi
# the stub as the build writes it, into a scratch tree (a changed build tag
# empties the object tree it is given)
tmp=$(mktemp -d) || exit 2
make -n OBJ_DIR="$tmp" >/dev/null 2>&1
stub="$tmp/pch/precompiled.h"
if [ ! -f "$stub" ]; then
	echo "the precompiled header stub was not written"
	status=1
elif ! grep -qx '#include "Common/precompiled.h"' "$stub"; then
	echo "the precompiled header stub does not include through the include path:"
	cat "$stub"
	status=1
fi
rm -rf "$tmp"
[ $status -eq 0 ] && echo "ok: no absolute paths written for the compiler"
exit $status
