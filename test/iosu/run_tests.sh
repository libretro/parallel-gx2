#!/bin/sh
# The IOSU kernel without locks. RUNNER prefixes the test (wine for a MinGW
# build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 600 $RUN"
$RUN ./iosu_kernel_test${EXE:-}
