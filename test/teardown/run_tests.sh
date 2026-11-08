#!/bin/sh
# What the core stops before the frontend unloads it. RUNNER prefixes the
# test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 120 $RUN"
$RUN ./teardown_test${EXE:-}
