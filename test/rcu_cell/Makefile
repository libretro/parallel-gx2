# The core's lock-free cell (src/util/helpers/rcu_cell.c), which RcuCell.h is
# the C++ over (run_tests.sh). C89, test and file both, as the C89 lane
# builds them; the cell built with its test hook. SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS -DRCU_CELL_TEST_HOOKS
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement \
	-Wno-long-long -Wno-variadic-macros -Wall -Werror
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
LRC_SRCS := $(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_eventcount.c
CORE_SRCS := rcu_cell_test.c $(SRC)/util/helpers/rcu_cell.c

rcu_cell_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/util/helpers/rcu_cell.h
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f rcu_cell_test rcu_cell_test.exe *.o
