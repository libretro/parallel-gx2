# The core's XML tree (src/util/xml/xml_tree.c over libretro-common's rxml,
# in C89 as the C89 lane builds it), the layer over it (Xml.h) and the
# settings files' reader and writer on that (src/config/XMLConfig.h)
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
include ../file_io.mk
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif

all: xml_test$(EXE) xml_tree_test$(EXE)

xml_tree_test$(EXE): xml_tree_test.c $(XML_TREE_C) $(SRC)/util/xml/xml_tree.h $(LRC)/formats/xml/rxml.c
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/formats/xml/rxml.c -o rxml_c.o
	$(CC) $(FLAGS) $(C89_FLAGS) xml_tree_test.c $(XML_TREE_C) rxml_c.o $(LIBS) -o $@
	rm -f rxml_c.o

xml_test$(EXE): xml_test.cpp $(XML_TREE_C) $(SRC)/util/xml/xml_tree.h $(SRC)/util/xml/Xml.h $(SRC)/config/XMLConfig.h $(LRC)/formats/xml/rxml.c
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/formats/xml/rxml.c -o rxml.o
	$(CC) $(FLAGS) $(C89_FLAGS) -c $(XML_TREE_C) -o xml_tree.o
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 xml_test.cpp rxml.o xml_tree.o $(LIBS) -o $@
	rm -f rxml.o xml_tree.o

clean:
	rm -f xml_test xml_tree_test *.exe *.o
