# The IOSU kernel without locks (iosu_kernel_test.cpp): message queues,
# timers and IPC from several threads at once, against the kernel's own
# source. SANITIZER=thread or SANITIZER=address,undefined for the sanitizer
# runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -DHAVE_THREADS -I$(SRC) -I$(SRC)/Common -I$(SRC)/Cafe -I$(SRC)/util -I$(LRC)/include
# what the core's precompiled header wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring undefined,$(SANITIZER)))
   FLAGS += -fno-sanitize-recover=undefined
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
LRC_SRCS := $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c
# RcuCell.h's and FixedQueue.h's C, built as the C89 lane builds it
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement -Wno-long-long -Wno-variadic-macros -Wall -Werror
CORE_C_SRCS := $(SRC)/util/helpers/rcu_cell.c $(SRC)/util/helpers/lf_ring.c
CXX_SRCS := iosu_kernel_test.cpp stubs.cpp $(SRC)/Cafe/IOSU/kernel/iosu_kernel.cpp \
	$(SRC)/Common/SysAllocator.cpp $(SRC)/util/highresolutiontimer/HighResolutionTimer.cpp

all: iosu_kernel_test$(EXE)

iosu_kernel_test$(EXE): $(CXX_SRCS) $(LRC_SRCS) $(CORE_C_SRCS)
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(CORE_C_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

.PHONY: all clean

clean:
	rm -f iosu_kernel_test iosu_kernel_test.exe *.o
