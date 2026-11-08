# The core's Wii U archive reader (src/Cafe/Filesystem/ZArchiveVFS.cpp) over
# libretro-common's zar, against archives libretro-common's writer makes
# (run_tests.sh). SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
DEP := ../../dependencies
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -I$(DEP)/pugixml/src -DHAVE_RZSTD -DENABLE_VULKAN
# what the core's precompiled header, which VFSFileStream.h includes, wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
C_SRCS := $(LRC)/formats/zar/zar.c $(LRC)/formats/zar/zar_write.c \
	$(LRC)/encodings/encoding_rzstd.c $(LRC)/encodings/encoding_xxh64.c \
	$(LRC)/hash/lrc_hash.c $(LRC)/features/features_cpu.c \
	$(LRC)/streams/file_stream.c $(LRC)/vfs/vfs_implementation.c \
	$(LRC)/file/file_path.c $(LRC)/file/file_path_io.c \
	$(LRC)/string/stdstring.c $(LRC)/string/rstrtod.c \
	$(LRC)/encodings/encoding_utf.c $(LRC)/compat/compat_strl.c \
	$(LRC)/compat/compat_posix_string.c $(LRC)/compat/fopen_utf8.c \
	$(LRC)/time/rtime.c
CXX_SRCS := zarchive_test.cpp $(SRC)/Cafe/Filesystem/ZArchiveVFS.cpp $(DEP)/pugixml/src/pugixml.cpp

zarchive_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(SRC)/Cafe/Filesystem/ZArchiveVFS.h $(SRC)/Cafe/TitleList/ParsedMetaXml.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f zarchive_test zarchive_test.exe *.o
