# The core's FST volume reader (src/Cafe/Filesystem/FST/FST.cpp): files read
# from unhashed and hashed clusters of a volume the test makes, through its
# block cache and straight to the destination (run_tests.sh).
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
DEP := ../../dependencies
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -Wno-multichar
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
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
C_SRCS := $(FILE_IO_C) $(LRC)/time/rtime.c $(LRC)/compat/compat_strl.c $(LRC)/hash/lrc_hash.c $(LRC)/features/features_cpu.c
CXX_SRCS := fst_test.cpp $(SRC)/Cafe/Filesystem/FST/FST.cpp $(SRC)/Cafe/Filesystem/FST/FSTDataSource.cpp \
	$(SRC)/Cafe/Filesystem/WUD/wud.cpp $(SRC)/util/helpers/MappedFile.cpp $(FILE_IO_CXX) \
	$(SRC)/util/crypto/aes128.cpp $(SRC)/Common/cpu_features.cpp

fst_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(SRC)/Cafe/Filesystem/FST/FST.h $(SRC)/Cafe/Filesystem/FST/FSTDataSource.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f fst_test fst_test.exe *.o
