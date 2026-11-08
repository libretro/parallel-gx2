# What the core stops before the frontend unloads it (teardown_test.cpp):
# a thread of the core still running then wakes in unmapped code and crashes
# the frontend. SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -DHAVE_THREADS -DENABLE_VULKAN -I$(SRC) -I$(SRC)/Common -I$(SRC)/Cafe -I$(SRC)/util -I$(LRC)/include
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
CXX_SRCS := teardown_test.cpp stubs.cpp $(SRC)/Cafe/HW/Espresso/PPCTimer.cpp \
	$(SRC)/util/highresolutiontimer/HighResolutionTimer.cpp
HDRS := $(SRC)/Cafe/HW/Espresso/PPCState.h $(SRC)/Cafe/HW/Espresso/PPCTimerClock.h \
	$(SRC)/util/helpers/EventWake.h

all: teardown_test$(EXE)

teardown_test$(EXE): $(CXX_SRCS) $(LRC_SRCS) $(HDRS)
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

.PHONY: all clean

clean:
	rm -f teardown_test teardown_test.exe *.o
