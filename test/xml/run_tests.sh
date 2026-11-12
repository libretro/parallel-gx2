#!/bin/sh
# The core's XML tree, the layer over it and the settings files on that.
# RUNNER prefixes the tests (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
$RUN ./xml_tree_test${EXE:-}
$RUN ./xml_test${EXE:-}
