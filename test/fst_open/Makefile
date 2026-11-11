# The core's volume openers (src/Cafe/Filesystem/FST/fst_open.c): disc
# images and title folders made here, opened right and turned away wrong,
# and tickets and TMDs parsed (run_tests.sh). C89, test and file both, as
# the C89 lane builds them. SANITIZER=address,undefined for the sanitizer
# run.
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
LRC_SRCS := $(FILE_IO_C) $(LRC)/time/rtime.c $(LRC)/compat/compat_strl.c $(LRC)/hash/lrc_hash.c \
	$(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_eventcount.c $(AES_LRC_C)
CORE_SRCS := fst_open_test.c $(DISC_IMAGE_C) $(AES_C) $(FST_TABLE_C) $(FST_VOLUME_C) $(FST_OPEN_C)

fst_open_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/Filesystem/FST/fst_open.h $(SRC)/Cafe/Filesystem/FST/fst_volume.h $(SRC)/Cafe/Filesystem/FST/fst_table.h $(SRC)/Cafe/Filesystem/FST/fst_source.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf fst_open_test fst_open_test.exe obj
