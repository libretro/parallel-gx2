# zlib and Zstandard through libretro-common (compression_test.cpp): the
# core's Deflate helpers, the console's zlib125 on the host side, the shader
# and pipeline cache entries and the cache file's async writer, and the
# graphic packs compiled in, with the pack tool built as the core's build
# builds it (run_tests.sh).
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
CC ?= gcc
HOSTCC ?= cc
# FILECACHE_TEST_HOOKS: a reader held between finding an entry and reading it
FLAGS := -O1 -g -DHAVE_THREADS -DFILECACHE_TEST_HOOKS -I$(SRC) -I$(SRC)/Common -I$(SRC)/Cafe -I$(SRC)/util -I$(LRC)/include
# what the core's precompiled header, which the serializers take, wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring undefined,$(SANITIZER)))
   FLAGS += -fno-sanitize-recover=undefined
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
LRC_SRCS := $(LRC)/encodings/encoding_deflate.c $(LRC)/encodings/encoding_crc32.c \
	$(LRC)/encodings/encoding_rzstd.c $(LRC)/encodings/encoding_xxh64.c \
	$(LRC)/features/features_cpu.c $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c \
	$(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c
include ../file_io.mk
LRC_SRCS += $(FILE_IO_C)
FLAGS += $(FILE_IO_FLAGS)
CXX_SRCS := compression_test.cpp \
	$(SRC)/Cafe/OS/libs/zlib125/zlib125_stream.cpp \
	$(SRC)/Cafe/HW/Latte/Common/ShaderSerializer.cpp \
	$(SRC)/Cafe/HW/Latte/Common/RegisterSerializer.cpp \
	$(FILE_IO_CXX) stubs.cpp

all: compression_test$(EXE)

# a pack tree of four files, two of them noise, as the packs repository lays
# them out
fixture/stamp:
	rm -rf fixture
	mkdir -p fixture/Enhancements/PackA fixture/Enhancements/PackB/sub
	printf '[Definition]\ntitleIds = 0005000010101c00\nname = Pack A\n' > fixture/Enhancements/PackA/rules.txt
	printf '[Definition]\nname = Pack B\n' > fixture/Enhancements/PackB/rules.txt
	head -c 200000 /dev/urandom > fixture/Enhancements/PackB/sub/noise.bin
	printf 'licence text\n' > fixture/LICENSE.md
	touch $@

pack_graphic_packs_host: ../../tools/pack_graphic_packs.c
	$(HOSTCC) -O2 -I$(LRC)/include ../../tools/pack_graphic_packs.c \
		$(LRC)/encodings/encoding_deflate.c $(LRC)/encodings/encoding_crc32.c \
		$(LRC)/features/features_cpu.c $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c -o $@

graphic_packs.c: pack_graphic_packs_host fixture/stamp
	./pack_graphic_packs_host fixture $@

compression_test$(EXE): $(FILE_CACHE_C) $(MEM_STREAM_C) $(CXX_SRCS) $(LRC_SRCS) graphic_packs.c golden_deflate.h golden_entries.h
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(FILE_CACHE_C) $(MEM_STREAM_C); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	# as the core builds it: C++
	$(CXX) $(FLAGS) -x c++ -c graphic_packs.c -o graphic_packs.o
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

.PHONY: all clean

clean:
	rm -rf compression_test compression_test.exe filecache_test.bin pack_graphic_packs_host pack_graphic_packs_host.exe graphic_packs.c fixture *.o
