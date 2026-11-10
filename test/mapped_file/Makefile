# The core's whole-file reader (src/util/helpers/mapped_file.c), under
# MappedFile.h (run_tests.sh): built with mappings (HAVE_MMAP, where the core
# has them) and without, each over libretro-common's file I/O built the same
# way. C89, test and file both, as the C89 lane builds them.
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
CORE_SRCS := mapped_file_test.c $(MAPPED_FILE_C)
LRC_SRCS := $(FILE_IO_C) $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c
# the read build always; the mapped one where the core maps
TESTS := mapped_file_test_read$(EXE)
ifneq ($(FILE_IO_FLAGS),)
   TESTS += mapped_file_test_mapped$(EXE)
endif

all: $(TESTS)

mapped_file_test_%$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/util/helpers/mapped_file.h
	mkdir -p obj_$*
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) $(if $(filter mapped,$*),$(FILE_IO_FLAGS)) -std=gnu99 -c $$f -o obj_$*/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(if $(filter mapped,$*),$(FILE_IO_FLAGS)) $(C89_FLAGS) -c $$f -o obj_$*/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj_$*/*.o $(LIBS) -o $@
	rm -rf obj_$*

.PHONY: all clean

clean:
	rm -rf mapped_file_test_read mapped_file_test_mapped *.exe obj_*
