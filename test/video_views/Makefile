# The screen layouts' arithmetic (run_tests.sh): Both Screens' packing, the
# layout a frame is drawn with, and the video views map that describes it.
# SANITIZER=address,undefined for the sanitizer run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -Wall -I$(SRC) -I$(SRC)/libretro -I$(LRC)/include
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS :=
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif

video_views_test$(EXE): video_views_test.cpp $(SRC)/libretro/LibretroViews.h $(LRC)/include/libretro.h
	$(CXX) $(FLAGS) -std=gnu++20 video_views_test.cpp $(LIBS) -o $@

clean:
	rm -f video_views_test video_views_test.exe
