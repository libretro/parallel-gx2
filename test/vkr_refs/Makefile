# The references Vulkan objects hold on one another
# (src/Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h) (run_tests.sh).
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
SRC := ../../src
DEP := ../../dependencies
LRC := $(DEP)/libretro-common
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -I$(DEP)/Vulkan-Headers/include -DVK_NO_PROTOTYPES -DENABLE_VULKAN
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CXX)))
   LIBS := -static
   EXE := .exe
endif
include ../file_io.mk
# the pools the references' blocks come from
C_SRCS := $(SRC)/util/helpers/obj_pool.c $(SRC)/util/helpers/lf_ring.c

vkr_refs_test$(EXE): vkr_refs_test.cpp $(C_SRCS) $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h \
	$(SRC)/util/helpers/PoolNew.h $(SRC)/util/helpers/obj_pool.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) $(C89_FLAGS) -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 vkr_refs_test.cpp *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f vkr_refs_test vkr_refs_test.exe *.o
