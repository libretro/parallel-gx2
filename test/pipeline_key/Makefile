# The Vulkan pipelines' key (src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp)
# against a replayed sequence of register states (run_tests.sh).
# SANITIZER=address,undefined for the sanitizer run.
SRC := ../../src
DEP := ../../dependencies
LRC := $(DEP)/libretro-common
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -I$(DEP)/Vulkan-Headers/include -DVK_NO_PROTOTYPES -DENABLE_VULKAN -DHAVE_THREADS
# what the core's precompiled header, which the register definitions want, wants;
# the Vulkan headers for the state the draw sets
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -ldl -lpthread
ifneq (,$(findstring mingw,$(CXX)))
   LIBS := -static
   EXE := .exe
endif
# the loader's entry points the key's dynamic state setters call through
SRCS := pipeline_key_test.cpp stubs.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp \
	$(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.cpp \
	$(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.cpp

# the pipeline libraries' handoff (FixedQueue.h): its ring and the eventcount
C_SRCS := $(SRC)/util/helpers/lf_ring.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c

pipeline_key_test$(EXE): $(SRCS) $(C_SRCS) $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h \
	$(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineLibrary.h $(SRC)/util/helpers/FixedQueue.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f pipeline_key_test pipeline_key_test.exe *.o
