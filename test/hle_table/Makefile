# The HLE call table
# (src/Cafe/HW/Espresso/Interpreter/PPCInterpreterHLE.cpp) (run_tests.sh).
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
FLAGS := -O1 -g -I$(SRC) -I$(SRC)/Common -I$(SRC)/Cafe -I$(SRC)/util -I$(LRC)/include
# what the core's precompiled header wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CXX)))
   LIBS += -static
   EXE := .exe
endif
ITP := $(SRC)/Cafe/HW/Espresso/Interpreter
CXX_SRCS := hle_table_test.cpp stubs.cpp $(ITP)/PPCInterpreterHLE.cpp

hle_table_test$(EXE): $(CXX_SRCS) $(SRC)/Cafe/HW/Espresso/PPCState.h
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) $(LIBS) -o $@

clean:
	rm -f hle_table_test hle_table_test.exe
