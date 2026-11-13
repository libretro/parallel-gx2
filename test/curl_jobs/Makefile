# nlibcurl's transfer threads (src/Cafe/OS/libs/nlibcurl/curl_jobs.c), in C89 as the C89
# lane builds it (run_tests.sh). SANITIZER=thread or
# SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -DCURL_JOBS_TEST_HOOKS -I$(LRC)/include -DHAVE_THREADS
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
CORE_SRCS := curl_jobs_test.c $(SRC)/Cafe/OS/libs/nlibcurl/curl_jobs.c

curl_jobs_test$(EXE): $(CORE_SRCS) $(LRC_SRCS) $(SRC)/Cafe/OS/libs/nlibcurl/curl_jobs.h
	mkdir -p obj
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	for f in $(CORE_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o obj/$$(basename $$f .c).o || exit 1; done
	$(CC) $(FLAGS) obj/*.o $(LIBS) -o $@
	rm -rf obj

clean:
	rm -rf curl_jobs_test curl_jobs_test.exe obj
