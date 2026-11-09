# h264.rpl's decoder engine (src/Cafe/OS/libs/h264_avc/H264DecodeEngine.cpp)
# over rh264 and a frontend's decoder, against ffmpeg's pictures of the
# streams in streams/ (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS
# what the parser's headers want from the core's precompiled header
CXXFLAGS_CORE := -include $(SRC)/Common/precompiled.h -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
C_SRCS := $(LRC)/formats/h264/rh264.c $(LRC)/rthreads/tpool.c $(LRC)/rthreads/rthreads.c \
	$(LRC)/rthreads/retro_eventcount.c $(LRC)/utils/md5.c
CXX_SRCS := h264_test.cpp $(SRC)/Cafe/OS/libs/h264_avc/H264DecodeEngine.cpp \
	$(SRC)/Cafe/OS/libs/h264_avc/parser/H264Parser.cpp
HDRS := $(SRC)/Cafe/OS/libs/h264_avc/H264DecodeEngine.h $(SRC)/Cafe/OS/libs/h264_avc/parser/H264Parser.h

h264_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(HDRS)
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f h264_test h264_test.exe *.o
