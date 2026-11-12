# The core's zlib streams (src/util/helpers/zlib_stream.c) under Deflate.h,
# in C89 as the C89 lane builds them, with eight threads sharing the pool
# of inflaters (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS -DZLIB_STREAM_TEST_HOOKS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
LRC_SRCS := $(LRC)/encodings/encoding_deflate.c $(LRC)/encodings/encoding_crc32.c \
	$(LRC)/features/features_cpu.c $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c \
	$(LRC)/rthreads/rthreads.c
CORE_SRCS := zlib_stream_test.c $(ZLIB_STREAM_C)

zlib_stream_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/util/helpers/zlib_stream.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf zlib_stream_test zlib_stream_test.exe obj
