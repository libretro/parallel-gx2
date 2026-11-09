#!/bin/sh
# Ending a thread from deep inside what it runs, linked as the core links.
# RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 120 $RUN"
$RUN ./thread_exit_test${EXE:-}
