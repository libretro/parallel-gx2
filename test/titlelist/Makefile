# The core's title list (src/Cafe/TitleList/TitleList.cpp) over stand-in
# titles (stub_titleinfo.cpp): scans, the cache file, the list read with no
# lock while scans change it, and Refresh/WaitForScan never missing a scan
# (run_tests.sh). SANITIZER=thread or SANITIZER=address,undefined for the
# sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
DEP := ../../dependencies
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -Wno-multichar -DHAVE_THREADS -DTITLELIST_TEST_HOOKS
# stub/ first: CemuConfig as the test has it
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
	$(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c $(LRC)/formats/xml/rxml.c
CXX_SRCS := titlelist_test.cpp stub_titleinfo.cpp $(SRC)/Cafe/TitleList/TitleList.cpp \
	$(SRC)/util/xml/Xml.cpp $(FILE_IO_CXX)
HEADERS := $(SRC)/Cafe/TitleList/TitleList.h $(SRC)/Cafe/TitleList/TitleInfo.h $(SRC)/util/helpers/RcuCell.h \
	$(SRC)/util/helpers/OnceGate.h stub/config/CemuConfig.h

titlelist_test$(EXE): $(MAPPED_FILE_C) $(CXX_SRCS) $(C_SRCS) $(RCU_C) $(HEADERS)
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(MAPPED_FILE_C); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(RCU_C); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f titlelist_test titlelist_test.exe *.o
