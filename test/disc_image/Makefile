# The core's disc image, title content and homebrew bundle readers
# (src/Cafe/Filesystem/WUD, C89, src/Cafe/Filesystem/FST/FSTDataSource.cpp,
# src/Cafe/Filesystem/WUHB): .wud, .wux and .app files read and their blocks
# decrypted, .wuhb files looked up and read, mapped and not (run_tests.sh). SANITIZER=thread
# or SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
DEP := ../../dependencies
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -Wno-multichar
# what the core's precompiled header, which FileStream.h includes, wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY
CXXFLAGS_CORE += -include $(SRC)/Common/precompiled.h
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
# filestream: the only FileStream call left is HostPath, which the test has
C_SRCS := $(filter-out %/retro_dirent.c %/compat_strcasestr.c,$(FILE_IO_C)) \
	$(LRC)/time/rtime.c $(LRC)/compat/compat_strl.c $(AES_LRC_C)
CORE_C_SRCS := $(DISC_IMAGE_C) $(SRC)/Cafe/Filesystem/WUHB/wuhb.c $(AES_C)
CXX_SRCS := disc_image_test.cpp $(SRC)/Cafe/Filesystem/FST/FSTDataSource.cpp

disc_image_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(CORE_C_SRCS) $(SRC)/Cafe/Filesystem/WUD/wud.h $(SRC)/Common/shared_file.h $(SRC)/Cafe/Filesystem/FST/FSTDataSource.h \
	$(SRC)/Cafe/Filesystem/WUHB/wuhb.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(CORE_C_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f disc_image_test disc_image_test.exe *.o
