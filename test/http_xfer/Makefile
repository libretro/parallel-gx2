# The core's HTTP transfers (src/Common/http_xfer.c) and nlibcurl's host
# side over them (src/Cafe/OS/libs/nlibcurl/curl_host.c), against local
# servers (run_tests.sh). Both C89, as the C89 lane builds them; the
# curl_host test is C++, as nlibcurl is. SANITIZER=address,undefined or
# SANITIZER=thread for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_SSL -DHAVE_RETROSSL -DHAVE_CRYPTO -DHAVE_THREADS
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement \
	-Wno-long-long -Wno-variadic-macros -Wall -Werror
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LRC_SOURCES := $(addprefix $(LRC)/,net/net_http.c net/net_socket.c net/net_compat.c \
	net/net_socket_ssl_retro.c crypto/crypto.c crypto/kdf.c crypto/pk.c crypto/x509.c \
	hash/lrc_hash.c utils/md5.c encodings/encoding_base64.c streams/file_stream.c \
	vfs/vfs_implementation.c file/file_path.c file/file_path_io.c string/rstrtod.c \
	string/stdstring.c encodings/encoding_utf.c features/features_cpu.c compat/compat_strl.c \
	compat/fopen_utf8.c compat/compat_strcasestr.c compat/compat_posix_string.c time/rtime.c \
	lists/string_list.c file/retro_dirent.c rthreads/rthreads.c rthreads/retro_eventcount.c)
CORE_SOURCES := $(SRC)/Common/http_xfer.c $(SRC)/Cafe/OS/libs/nlibcurl/curl_host.c
HEADERS := $(SRC)/Common/http_xfer.h $(SRC)/Cafe/OS/libs/nlibcurl/curl_host.h
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -lws2_32 -liphlpapi -lbcrypt -static
   EXE := .exe
endif

all: http_xfer_test$(EXE) curl_host_test$(EXE)

obj/.made: $(LRC_SOURCES) $(CORE_SOURCES) $(HEADERS)
	rm -rf obj && mkdir -p obj
	for f in $(LRC_SOURCES); do $(CC) $(FLAGS) -std=gnu11 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SOURCES); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	touch $@

http_xfer_test$(EXE): http_xfer_test.c obj/.made
	$(CC) $(FLAGS) $(C89_FLAGS) http_xfer_test.c $(filter-out obj/curl_host.o,$(wildcard obj/*.o)) $(LIBS) -o $@

curl_host_test$(EXE): curl_host_test.cpp obj/.made
	$(CXX) $(FLAGS) -std=gnu++20 curl_host_test.cpp obj/*.o $(LIBS) -o $@

.PHONY: all clean

clean:
	rm -rf http_xfer_test curl_host_test *.exe obj
