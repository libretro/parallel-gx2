# The core's file read from many threads at once (src/Common/shared_file.c):
# mapped and not, its streams kept and lent, eight threads at once
# (run_tests.sh). C89, test and file both, as the C89 lane builds them.
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
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
LRC_SRCS := $(filter-out %/retro_dirent.c %/compat_strcasestr.c %/host_path.c,$(FILE_IO_C)) \
	$(LRC)/time/rtime.c $(LRC)/compat/compat_strl.c $(LRC)/rthreads/rthreads.c \
	$(LRC)/rthreads/retro_eventcount.c
CORE_SRCS := shared_file_test.c $(SRC)/Common/shared_file.c

shared_file_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Common/shared_file.h
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f shared_file_test shared_file_test.exe *.o
