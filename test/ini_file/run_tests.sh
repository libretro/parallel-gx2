#!/bin/sh
# The core's ini reader and the C++ walk over it. RUNNER prefixes the tests
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./ini_file_test${EXE:-}
$RUN ./ini_parser_test${EXE:-}
