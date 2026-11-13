# The interpreter's tw, run through the interpreter
# (src/Cafe/HW/Espresso/Interpreter/PPCInterpreterImpl.cpp) (run_tests.sh).
# SANITIZER=address,undefined for the sanitizer run.
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
CXX_SRCS := ppc_trap_test.cpp stubs.cpp $(ITP)/PPCInterpreterImpl.cpp $(ITP)/PPCInterpreterFPU.cpp \
	$(ITP)/PPCInterpreterPS.cpp $(ITP)/PPCInterpreterOPC.cpp

ppc_trap_test$(EXE): $(CXX_SRCS) $(ITP)/PPCInterpreterOPC.hpp
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) $(LIBS) -o $@

clean:
	rm -f ppc_trap_test ppc_trap_test.exe
