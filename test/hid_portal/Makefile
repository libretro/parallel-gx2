# An emulated toy portal's commands and reports (src/Cafe/OS/libs/nsyshid/hid_portal.c), in C89 as the C89
# lane builds it (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS -DHID_PORTAL_TEST_HOOKS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
include ../file_io.mk
LRC_SRCS := $(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_eventcount.c
CORE_SRCS := hid_portal_test.c $(SRC)/Cafe/OS/libs/nsyshid/hid_portal.c $(SRC)/util/helpers/lf_ring.c

hid_portal_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/OS/libs/nsyshid/hid_portal.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf hid_portal_test hid_portal_test.exe obj
