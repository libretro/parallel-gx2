#!/bin/sh
# Fails if the core builds a source file of its own (src/) that nothing
# reaches from what the frontend calls (retro_*): every object is followed
# through the symbols it uses, and a file none of whose symbols are used is
# code that only costs build time. Work done at load time by a static
# constructor does not count as a use: it is to be called from where it
# belongs instead. Run after a build, with the object directory it used.
set -e
dir=${1%/}
[ -d "$dir" ] || { echo "usage: $0 <object directory, e.g. obj/unix-x86_64>"; exit 2; }
NM=${NM:-nm}
# the objects of sources that are still there, not of ones since removed
objs=$(find "$dir" -name '*.o' | sed "s#^$dir/##" | while read -r o; do
	s=${o%.o}
	if [ -f "$s" ]; then printf '%s\n' "$o"; fi
done)
[ -n "$objs" ] || { echo "$dir: no objects"; exit 2; }
cd "$dir"
# shellcheck disable=SC2086
$NM -A -P $objs 2>/dev/null | awk '
{
	o = $1; sub(/:$/, "", o); s = $2; t = $3
	if (!(o in seen)) { seen[o] = 1; objs[++n] = o }
	if (t == "U") { refs[o, ++nref[o]] = s; next }
	if (s ~ /^retro_/ && t == "T") root[o] = 1
	if (t ~ /^[TDBRGSC]$/) def[s] = o
	# a vtable or type info is weak: the object of the key function of the class
	# has it, or every one that needs it does. Any of them may be the one
	# linked, so each is taken as used
	else if (t ~ /^[VW]$/ && s ~ /^_ZT[VIS]/) weak[s] = weak[s] " " o
}
END {
	for (i = 1; i <= n; i++) if (root[objs[i]]) { live[objs[i]] = 1; q[++qn] = objs[i] }
	if (!qn) { print "no retro_* entry points found"; exit 2 }
	for (h = 1; h <= qn; h++) {
		o = q[h]
		for (j = 1; j <= nref[o]; j++) {
			r = refs[o, j]
			if (r in def) nd = split(def[r], ds, " ")
			else if (r in weak) nd = split(weak[r], ds, " ")
			else nd = 0
			for (k = 1; k <= nd; k++)
				if (!live[ds[k]]) { live[ds[k]] = 1; q[++qn] = ds[k] }
		}
	}
	bad = 0
	for (i = 1; i <= n; i++) {
		o = objs[i]
		if (live[o] || o !~ /^src\//) continue
		sub(/\.o$/, "", o)
		print "nothing uses " o
		bad = 1
	}
	if (!bad) print n " objects, every one of the core'"'"'s own used"
	exit bad
}'
