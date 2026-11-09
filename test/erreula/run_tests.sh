#!/bin/sh
# The error viewer answered through the frontend. RUNNER prefixes the test
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./erreula_test${EXE:-}
