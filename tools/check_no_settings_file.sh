#!/bin/sh
# Fails if the core's settings (CemuConfig) are given a file again: an
# XMLConfig or XMLDataConfig over CemuConfig, a Load or Save of it from XML,
# a config handle to load or save through, or settings.xml named in code.
# The core starts from Cemu's defaults and the core options set the rest; a
# settings.xml read as well decided things no option showed, and code kept
# for what only it could switch on (the boot sound, the game list's entries,
# graphic pack presets from standalone) can never run.
cd "$(dirname "$0")/.." || exit 2
pattern='XML(Data)?Config<[[:space:]]*CemuConfig|CemuConfig::(Load|Save)[[:space:]]*\(|GetConfigHandle[[:space:]]*\(|XMLCemuConfig_t|"settings\.xml"'
found=$(grep -rnE "$pattern" src \
	--include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.mm' 2>/dev/null \
	| grep -v -e '^[^:]*:[0-9]*:[[:space:]]*\(\*\|//\)')
# Comment lines are not code.
if [ -n "$found" ]; then
	echo "the core's settings given a file (they come from the core options):"
	printf '%s\n' "$found"
	exit 1
fi
echo "the core's settings have no file behind them"
