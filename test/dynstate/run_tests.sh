#!/bin/sh
# The state the pipelines leave to the draw, drawn baked and dynamic. RUNNER prefixes the test
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
# the driver's threads' allocations (lsan.supp)
export LSAN_OPTIONS="suppressions=$(pwd)/lsan.supp${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./dynstate_test${EXE:-}
