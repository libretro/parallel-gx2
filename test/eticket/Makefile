# A Wii U eTicket (src/Cemu/ncrypto/eticket.c), in C89 as the C89 lane
# builds it (run_tests.sh). SANITIZER=address,undefined for the sanitizer
# run.
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
CORE_SRCS := eticket_test.c $(SRC)/Cemu/ncrypto/eticket.c

eticket_test$(EXE): $(CORE_SRCS) $(SRC)/Cemu/ncrypto/eticket.h
	$(CC) $(FLAGS) $(C89_FLAGS) $(CORE_SRCS) $(LIBS) -o $@

clean:
	rm -f eticket_test eticket_test.exe
