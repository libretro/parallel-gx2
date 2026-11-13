#!/bin/sh
# The camera library's state. RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./cam_state_test${EXE:-}
