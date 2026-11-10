#!/bin/sh
# The core's whole-file reader, mapped and read. RUNNER prefixes the tests
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
for t in mapped_file_test_read mapped_file_test_mapped; do
   if [ -f "$t${EXE:-}" ]; then
      $RUN ./$t${EXE:-}
   fi
done
