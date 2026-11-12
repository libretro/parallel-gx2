# The core's ini reader (src/util/IniParser/ini_file.c), in C89 as the C89
# lane builds it, and the C++ walk over it (IniParser.h), as rules.txt,
# game profiles and a WUHB's meta.ini are read (run_tests.sh).
# SANITIZER=address,undefined for the sanitizer run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
CXX ?= g++
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
INI_C := $(SRC)/util/IniParser/ini_file.c
HEADERS := $(SRC)/util/IniParser/ini_file.h $(SRC)/util/IniParser/IniParser.h

all: ini_file_test$(EXE) ini_parser_test$(EXE)

ini_file_test$(EXE): ini_file_test.c $(INI_C) $(HEADERS)
	$(CC) $(FLAGS) $(C89_FLAGS) ini_file_test.c $(INI_C) $(LIBS) -o $@

ini_parser_test$(EXE): ini_parser_test.cpp $(INI_C) $(HEADERS)
	$(CC) $(FLAGS) $(C89_FLAGS) -c $(INI_C) -o ini_file_cxx.o
	$(CXX) $(FLAGS) -std=gnu++20 -Wall -Werror ini_parser_test.cpp ini_file_cxx.o $(LIBS) -o $@
	rm -f ini_file_cxx.o

clean:
	rm -f ini_file_test ini_parser_test *.exe *.o
