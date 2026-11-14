#!/bin/sh
# The HLE call table. RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 60 $RUN"
$RUN ./hle_table_test${EXE:-}
