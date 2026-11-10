# The core's virtual file system (src/Cafe/Filesystem/fsc.cpp) over host
# folders mounted at the base and a patch priority (fscDeviceHostFS.cpp):
# which mount a file comes from, a file read in chunks, and a directory
# listed through both; and a homebrew bundle mounted, listed and read
# (fscDeviceWuhb.cpp, WUHB/wuhb.c) (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
DEP := ../../dependencies
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -Wno-multichar
# stub/ first: ActiveSettings as the test has it
CXXFLAGS_CORE := -iquote stub -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
	-include $(SRC)/Common/precompiled.h
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
C_SRCS := $(FILE_IO_C) $(LRC)/time/rtime.c $(LRC)/compat/compat_strl.c
CXX_SRCS := fsc_test.cpp $(SRC)/Cafe/Filesystem/fsc.cpp $(SRC)/Cafe/Filesystem/fscDeviceHostFS.cpp \
	$(SRC)/Cafe/Filesystem/fscDeviceWuhb.cpp $(FILE_IO_CXX)
# the bundle reader and the file reader under it, C89
CORE_C_SRCS := $(SRC)/Cafe/Filesystem/WUHB/wuhb.c $(SRC)/Common/shared_file.c

fsc_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(CORE_C_SRCS) $(SRC)/Cafe/Filesystem/WUHB/wuhb.h ../disc_image/romfs_image.h $(SRC)/Cafe/Filesystem/fsc.h $(SRC)/Cafe/Filesystem/fscDeviceHostFS.h $(SRC)/Cafe/RPXHash.h stub/config/ActiveSettings.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(CORE_C_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f fsc_test fsc_test.exe *.o
