# The core's FST table (src/Cafe/Filesystem/FST/fst_table.c), which FST.cpp
# looks entries up in (run_tests.sh). C89, test and file both, as the C89
# lane builds them. SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement \
	-Wno-long-long -Wno-variadic-macros -Wall -Werror
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
CORE_SRCS := fst_table_test.c $(SRC)/Cafe/Filesystem/FST/fst_table.c

fst_table_test$(EXE): $(CORE_SRCS) $(SRC)/Cafe/Filesystem/FST/fst_table.h
	$(CC) $(FLAGS) $(C89_FLAGS) $(CORE_SRCS) $(LIBS) -o $@

clean:
	rm -f fst_table_test fst_table_test.exe *.o
