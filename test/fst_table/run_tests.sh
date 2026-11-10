#!/bin/sh
# The core's FST table. RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./fst_table_test${EXE:-}
