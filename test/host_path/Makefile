# The core's host paths (src/Common/host_path.c), in C89 as the C89 lane
# builds it (run_tests.sh). SANITIZER=address,undefined for the sanitizer
# run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
FLAGS += $(FILE_IO_FLAGS)
LRC_SRCS := $(filter-out %/host_path.c,$(FILE_IO_C)) $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c
CORE_SRCS := host_path_test.c $(SRC)/Common/host_path.c

host_path_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Common/host_path.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf host_path_test host_path_test.exe obj host_path_test_dir host_path_test_outside
