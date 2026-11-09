# The error viewer answered through the frontend (src/Cafe/OS/libs/erreula/ErrEulaPrompt.h)
# (run_tests.sh). SANITIZER=address,undefined for the sanitizer run.
SRC := ../../src
CXX ?= g++
FLAGS := -O1 -g -I$(SRC)
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CXX)))
   LIBS += -static
   EXE := .exe
endif

erreula_test$(EXE): erreula_test.cpp $(SRC)/Cafe/OS/libs/erreula/ErrEulaPrompt.h
	$(CXX) $(FLAGS) -std=gnu++20 erreula_test.cpp $(LIBS) -o $@

clean:
	rm -f erreula_test erreula_test.exe
