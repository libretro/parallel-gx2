#!/bin/sh
# Pipelines made ahead of the draw or copy that needs them. RUNNER prefixes
# the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./prepared_pipeline_test${EXE:-}
