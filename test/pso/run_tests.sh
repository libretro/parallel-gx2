#!/bin/sh
# The shader/PSO gate: fails when the shader path costs more than the
# baseline says, or less without the baseline having been updated. The
# graphic packs are a submodule (dependencies/cemu_graphic_packs). RUNNER
# prefixes the gate (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 900 $RUN"
$RUN ./pso_gate${EXE:-} ../../dependencies/cemu_graphic_packs --baseline baseline.txt --record replayed.psorec "$@"
# the replay of a recording, as a title's is replayed: the gate's own
$RUN ./pso_gate${EXE:-} --replay replayed.psorec
rm -f replayed.psorec
