# The core's AES-128 (src/util/crypto/aes128.c) over libretro-common's
# crypto: standard vectors, a 64 KiB CBC block against block-by-block, and
# CTR's counter (run_tests.sh). C89, test and file both, as the C89 lane
# builds them. SANITIZER=address,undefined for the sanitizer run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_CRYPTO
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement \
	-Wno-long-long -Wno-variadic-macros -Wall -Werror
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
LRC_SRCS := $(LRC)/crypto/crypto.c $(LRC)/features/features_cpu.c $(LRC)/compat/compat_strl.c \
	$(LRC)/time/rtime.c
CORE_SRCS := aes_test.c $(SRC)/util/crypto/aes128.c

aes_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/util/crypto/aes128.h
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f aes_test aes_test.exe *.o
