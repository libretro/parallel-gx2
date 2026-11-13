#!/bin/sh
# The names of the console's addresses. RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./rpl_symbols_test${EXE:-}
$RUN ./rpl_symbols_test_crowded${EXE:-}
