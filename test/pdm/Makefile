# The play statistics and diary (src/Cafe/IOSU/PDM/pdm_store.c), in C89 as
# the C89 lane builds it (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
FLAGS += $(FILE_IO_FLAGS)
LRC_SRCS := $(filter-out %/host_path.c,$(FILE_IO_C)) $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c \
	$(LRC)/features/features_cpu.c $(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_eventcount.c
CORE_SRCS := pdm_test.c $(SRC)/Cafe/IOSU/PDM/pdm_store.c $(SRC)/Common/host_path.c

pdm_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/IOSU/PDM/pdm_store.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf pdm_test pdm_test.exe obj pdm_test_dir
