# The core's XML layer (src/util/xml/Xml.h over libretro-common's rxml) and
# the settings files' reader and writer on it (src/config/XMLConfig.h)
# (run_tests.sh). SANITIZER=address,undefined for the sanitizer run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include
# what the core's precompiled header, which XMLConfig.h includes, wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif

xml_test$(EXE): xml_test.cpp $(SRC)/util/xml/Xml.cpp $(SRC)/util/xml/Xml.h $(SRC)/config/XMLConfig.h $(LRC)/formats/xml/rxml.c
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/formats/xml/rxml.c -o rxml.o
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 xml_test.cpp $(SRC)/util/xml/Xml.cpp rxml.o $(LIBS) -o $@
	rm -f rxml.o

clean:
	rm -f xml_test xml_test.exe rxml.o
