#!/bin/sh
# Fails if the read ahead (LatteShaderAhead.cpp) stops deciding by one of
# the rules test/shader_ahead checks (LatteShaderAheadRules.h): the file is
# linked by no test, so a decision worked out there again, beside the rule,
# would leave the test checking something the core no longer runs.
cd "$(dirname "$0")/.." || exit 2
f=src/Cafe/HW/Latte/Core/LatteShaderAhead.cpp
missing=
for rule in $(grep -oE '^inline [a-zA-Z0-9_]+ (LatteAhead_[A-Za-z]+)\(' src/Cafe/HW/Latte/Core/LatteShaderAheadRules.h | sed 's/.* //; s/($//'); do
	grep -q "$rule(" "$f" || missing="$missing $rule"
done
# the programs as the draw before had them, forgotten as each read starts:
# kept from the last read, a program rewritten in place since would read
# as the one copied then
awk '/^	void RunJob\(Job\* job\)/ { inside = 1 }
	inside && /memo->Forget\(\)/ { found = 1 }
	inside && /^	}$/ { inside = 0 }
	END { exit !found }' "$f" || missing="$missing (the programs' memos not forgotten as a read starts)"
if [ -n "$missing" ]; then
	echo "the read ahead no longer decides by:$missing"
	exit 1
fi
echo "the read ahead decides by every rule test/shader_ahead checks"
