#!/bin/sh
# tools/check_unused_units.sh against a tree of a few objects whose answer
# is known: what the entry points reach, through functions, data and a
# class's vtable, is used; a file only a static constructor would run, a
# chain only it reaches and a file nothing names are not; a dependency's
# file is not the core's to answer for; and an object left over from a
# source since removed is not counted.
set -e
here=$(cd "$(dirname "$0")" && pwd)
check=$here/../../tools/check_unused_units.sh
CC=${CC:-cc}
CXX=${CXX:-c++}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
mkdir -p src/a dependencies/d obj
cat > src/a/entry.c <<'E'
int reached(void);
extern int table[];
int retro_api_version(void) { return reached() + table[0]; }
E
cat > src/a/reached.c <<'E'
int deeper(void);
int reached(void) { return deeper(); }
E
cat > src/a/deeper.c <<'E'
int deeper(void) { return 1; }
E
cat > src/a/table.c <<'E'
int table[] = { 1 };
E
cat > src/a/provider.cpp <<'E'
struct Provider { virtual int get(); virtual ~Provider() {} };
int Provider::get() { return 2; }
E
cat > src/a/user.cpp <<'E'
struct Provider { virtual int get(); virtual ~Provider() {} };
extern "C" int retro_get_region(void) { Provider* p = new Provider(); int r = p->get(); delete p; return r; }
E
# a template's vtable, instantiated in two files and used from a third
# that leaves it to them: either may be the one linked
cat > src/a/box.h <<'E'
template<class T> struct Box { virtual T get() { return T(); } virtual ~Box() {} };
E
cat > src/a/inst1.cpp <<'E'
#include "box.h"
template struct Box<int>;
E
cat > src/a/inst2.cpp <<'E'
#include "box.h"
template struct Box<int>;
E
cat > src/a/boxuser.cpp <<'E'
#include "box.h"
extern template struct Box<int>;
extern "C" int retro_get_memory_size(void) { Box<int>* b = new Box<int>(); int r = b->get(); delete b; return r; }
E
cat > src/a/selfreg.cpp <<'E'
int only_from_selfreg(void);
struct Reg { Reg() { only_from_selfreg(); } };
static Reg s_reg;
E
cat > src/a/chain.c <<'E'
int only_from_selfreg(void) { return 3; }
E
cat > src/a/nobody.c <<'E'
int nobody_calls_this(void) { return 4; }
E
cat > dependencies/d/unused_dep.c <<'E'
int dep_nobody_calls(void) { return 5; }
E
build() {
	for s in "$@"; do
		mkdir -p "obj/$(dirname "$s")"
		case $s in
		*.cpp) $CXX -c -O0 "$s" -o "obj/$s.o" ;;
		*) $CC -c -O0 "$s" -o "obj/$s.o" ;;
		esac
	done
}
build src/a/*.c src/a/*.cpp dependencies/d/unused_dep.c
# an object of a source no longer there
cp obj/src/a/nobody.c.o obj/src/a/removed.c.o
fail=0
expect() {
	if [ "$1" != "$2" ]; then
		echo "FAIL $3"; echo " expected: $2"; echo " got:      $1"; fail=1
	else
		echo "ok   $3"
	fi
}
out=$("$check" obj) && st=0 || st=$?
out=$(printf '%s\n' "$out" | sort)
expect "$st" 1 "unused files fail the check"
expect "$out" "nothing uses src/a/chain.c
nothing uses src/a/nobody.c
nothing uses src/a/selfreg.cpp" "the files nothing reaches, and only those"
# used once something reaches them
cat >> src/a/deeper.c <<'E'
int nobody_calls_this(void);
int only_from_selfreg(void);
int via_deeper(void) { return nobody_calls_this() + only_from_selfreg(); }
E
cat >> src/a/reached.c <<'E'
int via_deeper(void);
int reached2(void) { return via_deeper(); }
E
sed 's/return reached() + table\[0\];/return reached() + table[0]; } int reached2(void); int retro_init_probe(void) { return reached2();/' src/a/entry.c > src/a/entry.tmp && mv src/a/entry.tmp src/a/entry.c
rm src/a/selfreg.cpp obj/src/a/selfreg.cpp.o
build src/a/entry.c src/a/reached.c src/a/deeper.c
out=$("$check" obj) && st=0 || st=$?
expect "$st" 0 "every file reached passes"
case $out in *"every one of the core's own used"*) echo "ok   says so" ;; *) echo "FAIL says so: $out"; fail=1 ;; esac
# no entry points: refused rather than everything reported
rm obj/src/a/entry.c.o obj/src/a/user.cpp.o obj/src/a/boxuser.cpp.o
"$check" obj >/dev/null 2>&1 && st=0 || st=$?
expect "$st" 2 "no entry points is an error"
"$check" "$tmp/none" >/dev/null 2>&1 && st=0 || st=$?
expect "$st" 2 "no object directory is an error"
[ $fail -eq 0 ] && echo "unused_units: all passed"
exit $fail
