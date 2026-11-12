# Miiverse's offline posts (src/Cafe/OS/libs/nn_olv/olv_offline.c), in C89 as
# the C89 lane builds it, against archives libretro-common's writer makes
# (run_tests.sh). SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_RZSTD -DHAVE_THREADS
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
	$(LRC)/formats/zar/zar.c $(LRC)/formats/zar/zar_write.c \
	$(LRC)/encodings/encoding_rzstd.c $(LRC)/encodings/encoding_xxh64.c $(LRC)/hash/lrc_hash.c \
	$(LRC)/encodings/encoding_deflate.c $(LRC)/encodings/encoding_crc32.c $(LRC)/features/features_cpu.c \
	$(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_eventcount.c
CORE_SRCS := olv_offline_test.c $(SRC)/Cafe/OS/libs/nn_olv/olv_offline.c \
	$(SRC)/Cafe/Filesystem/zarchive.c $(SRC)/Common/shared_file.c $(RCU_C) $(ZLIB_STREAM_C)

olv_offline_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/OS/libs/nn_olv/olv_offline.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf olv_offline_test olv_offline_test.exe obj olv_offline_test.zar
