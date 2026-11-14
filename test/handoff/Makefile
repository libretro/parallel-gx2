# The lock-free handoffs the core's threads use (run_tests.sh): the
# recompiler's work queue, the GPU thread's wake, the audio ring, the log
# file, the frontend's Vulkan queue lock, and the input and rumble handoffs.
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(SRC)/libretro -I$(LRC)/include -I../../dependencies/Vulkan-Headers/include -DHAVE_THREADS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static -lws2_32
   EXE := .exe
endif
# RcuCell.h's and FixedQueue.h's C, built as the C89 lane builds it
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement -Wno-long-long -Wno-variadic-macros -Wall -Werror
C_SRCS := $(SRC)/util/helpers/rcu_cell.c $(SRC)/util/helpers/lf_ring.c $(SRC)/util/helpers/obj_pool.c $(SRC)/util/helpers/u64_map.c $(LRC)/queues/retro_spsc.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c
CXX_SRCS := handoff_test.cpp \
	$(SRC)/Cafe/HW/Espresso/Recompiler/PPCRecompilerWorkQueue.cpp \
	$(SRC)/Cafe/HW/Latte/Core/LatteGpuWake.cpp \
	$(SRC)/libretro/LibretroVkQueue.cpp
HDRS := $(SRC)/libretro/LibretroAudioRing.h $(SRC)/Cafe/HW/Latte/Core/LatteGpuWake.h \
	$(SRC)/util/helpers/ThreadStartedFlag.h $(SRC)/util/helpers/EventWake.h $(SRC)/Cemu/Logging/LogFileSink.h \
	$(SRC)/util/helpers/SeqlockValue.h $(SRC)/util/helpers/MpscList.h $(SRC)/libretro/LibretroVkQueue.h \
	$(SRC)/audio/AudioDevicesGate.h $(SRC)/util/helpers/AtomicPageSet.h \
	$(SRC)/Cafe/HW/Espresso/PPCTimerClock.h $(SRC)/util/helpers/AtomicPtrBag.h \
	$(SRC)/util/helpers/FixedQueue.h $(SRC)/util/helpers/lf_ring.h $(SRC)/util/helpers/PoolNew.h $(SRC)/util/helpers/obj_pool.h $(SRC)/util/helpers/SmallVec.h $(SRC)/util/helpers/MpmcQueue.h \
	$(SRC)/util/helpers/ClaimTicket.h $(SRC)/Cemu/nex/nexWait.h \
	$(SRC)/util/helpers/RcuCell.h $(SRC)/util/helpers/rcu_cell.h $(SRC)/util/helpers/Semaphore.h $(SRC)/util/helpers/IdleWait.h \
	$(SRC)/Cafe/HW/Latte/Core/FetchShaderRegistry.h $(SRC)/util/helpers/u64_map.h

handoff_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(HDRS)
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/queues/retro_spsc.c -o retro_spsc.o
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/rthreads/retro_eventcount.c -o retro_eventcount.o
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/rthreads/rthreads.c -o rthreads.o
	$(CC) $(FLAGS) $(C89_FLAGS) -c $(SRC)/util/helpers/rcu_cell.c -o rcu_cell.o
	$(CC) $(FLAGS) $(C89_FLAGS) -c $(SRC)/util/helpers/lf_ring.c -o lf_ring.o
	$(CC) $(FLAGS) $(C89_FLAGS) -c $(SRC)/util/helpers/obj_pool.c -o obj_pool.o
	$(CC) $(FLAGS) $(C89_FLAGS) -c $(SRC)/util/helpers/u64_map.c -o u64_map.o
	$(CXX) $(FLAGS) -std=gnu++20 $(CXX_SRCS) retro_spsc.o retro_eventcount.o rthreads.o rcu_cell.o lf_ring.o obj_pool.o u64_map.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f handoff_test handoff_test.exe *.o
