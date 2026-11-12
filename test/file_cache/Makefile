# The core's cache files (src/Cemu/FileCache/file_cache.c), from the inside:
# the file included by the test, in C89 as the C89 lane builds it
# (run_tests.sh). SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
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
LRC_SRCS := $(FILE_IO_C) $(LRC)/encodings/encoding_deflate.c $(LRC)/encodings/encoding_crc32.c \
	$(LRC)/features/features_cpu.c $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c \
	$(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_eventcount.c
CORE_SRCS := file_cache_test.c $(ZLIB_STREAM_C) $(MAPPED_FILE_C)

file_cache_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cemu/FileCache/file_cache.c $(SRC)/Cemu/FileCache/file_cache.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf file_cache_test file_cache_test.exe obj file_cache_test.bin
