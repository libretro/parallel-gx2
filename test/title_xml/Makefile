# The core's title files (src/Cafe/TitleList/title_xml.c): meta.xml,
# meta.ini, app.xml and cos.xml parsed as the C++ readers before it did
# (run_tests.sh). C89, test and file both, as the C89 lane builds them.
# SANITIZER=address,undefined for the sanitizer run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
LRC_SRCS := $(TITLE_XML_LRC_C) $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c
CORE_SRCS := title_xml_test.c $(TITLE_XML_C)

title_xml_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/TitleList/title_xml.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf title_xml_test title_xml_test.exe obj
