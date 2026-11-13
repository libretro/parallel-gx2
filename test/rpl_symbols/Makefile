# The names of the console's addresses (src/Cafe/OS/RPL/rpl_symbols.c), in
# C89 as the C89 lane builds it (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS -DRPL_SYMBOLS_TEST_HOOKS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
LRC_SRCS := $(LRC)/rthreads/rthreads.c
CORE_SRCS := rpl_symbols_test.c $(SRC)/Cafe/OS/RPL/rpl_symbols.c

all: rpl_symbols_test$(EXE) rpl_symbols_test_crowded$(EXE)

# the core's build, and one with 4 buckets: every thread on the same lists
rpl_symbols_test$(EXE) rpl_symbols_test_crowded$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/OS/RPL/rpl_symbols.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o rpl_symbols_test$(EXE)
	$(CC) $(FLAGS) $(C89_FLAGS) -DRPL_SYMBOLS_BUCKETS=4 -c $(SRC)/Cafe/OS/RPL/rpl_symbols.c -o obj/rpl_symbols.o
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o rpl_symbols_test_crowded$(EXE)
	rm -rf obj

clean:
	rm -rf rpl_symbols_test rpl_symbols_test.exe rpl_symbols_test_crowded rpl_symbols_test_crowded.exe obj
