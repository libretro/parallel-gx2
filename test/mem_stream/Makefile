# The core's memory streams (src/util/helpers/mem_stream.c) under
# Serializer.h, in C89 as the C89 lane builds them (run_tests.sh).
# SANITIZER=address,undefined for the sanitizer run.
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

mem_stream_test$(EXE): mem_stream_test.c $(MEM_STREAM_C) $(SRC)/util/helpers/mem_stream.h
	$(CC) $(FLAGS) $(C89_FLAGS) mem_stream_test.c $(MEM_STREAM_C) $(LIBS) -o $@

clean:
	rm -f mem_stream_test mem_stream_test.exe
