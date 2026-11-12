# The core's HMAC-MD5 (src/util/crypto/hmac_md5.c) over libretro-common's
# MD5, in C89 as the C89 lane builds it (run_tests.sh).
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

hmac_md5_test$(EXE): hmac_md5_test.c $(SRC)/util/crypto/hmac_md5.c $(SRC)/util/crypto/hmac_md5.h
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/utils/md5.c -o md5.o
	$(CC) $(FLAGS) $(C89_FLAGS) hmac_md5_test.c $(SRC)/util/crypto/hmac_md5.c md5.o $(LIBS) -o $@
	rm -f md5.o

clean:
	rm -f hmac_md5_test hmac_md5_test.exe md5.o
