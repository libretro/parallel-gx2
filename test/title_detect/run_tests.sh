#!/bin/sh
# The core's title detection. RUNNER prefixes the test (wine for a MinGW
# build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./title_detect_test${EXE:-}
