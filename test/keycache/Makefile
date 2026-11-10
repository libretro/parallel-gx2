# The core's disc key search (src/Cafe/Filesystem/FST/KeyCache.cpp): keys.txt
# read again on reload, the key files beside an image (its own <name>.key and
# a dumper's game.key) tried, and what the user is told when none fits
# (run_tests.sh). SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
DEP := ../../dependencies
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -Wno-multichar -DHAVE_THREADS
# stub/ first: ActiveSettings and WindowSystem as the test has them
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
C_SRCS := $(FILE_IO_C) $(LRC)/time/rtime.c $(LRC)/compat/compat_strl.c \
	$(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c $(AES_LRC_C)
CXX_SRCS := keycache_test.cpp $(SRC)/Cafe/Filesystem/FST/KeyCache.cpp $(SRC)/util/helpers/MappedFile.cpp \
	$(FILE_IO_CXX)

keycache_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(AES_C) $(SRC)/Cafe/Filesystem/FST/KeyCache.h stub/config/ActiveSettings.h stub/WindowSystem.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(AES_C); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f keycache_test keycache_test.exe *.o
