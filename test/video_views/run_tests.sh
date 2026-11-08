#!/bin/sh
# The screen layouts' arithmetic. RUNNER prefixes the test (wine for a MinGW
# build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
$RUN ./video_views_test${EXE:-}
