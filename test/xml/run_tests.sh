#!/bin/sh
# The core's XML layer and the settings files on it. RUNNER prefixes the test
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./xml_test${EXE:-}
