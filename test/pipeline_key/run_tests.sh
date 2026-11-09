#!/bin/sh
# The Vulkan pipelines' key against a replayed state sequence. RUNNER prefixes the test
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./pipeline_key_test${EXE:-}
