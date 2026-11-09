# Ending a thread from deep inside what it runs (src/util/helpers/ThreadExit.h,
# thread_exit_test.cpp), linked as the core links its C++ runtime: statically.
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
SRC := ../../src
CXX ?= g++
FLAGS := -O1 -g -I$(SRC)
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
# the core's link (Makefile): libstdc++ and libgcc in the binary
LIBS := -static-libgcc -static-libstdc++ -lpthread
ifneq (,$(findstring mingw,$(CXX)))
   LIBS := -static
   EXE := .exe
endif

thread_exit_test$(EXE): thread_exit_test.cpp $(SRC)/util/helpers/ThreadExit.h
	$(CXX) $(FLAGS) -std=gnu++20 thread_exit_test.cpp $(LIBS) -o $@

clean:
	rm -f thread_exit_test thread_exit_test.exe
