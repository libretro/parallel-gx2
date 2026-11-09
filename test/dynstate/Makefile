# The state the Vulkan pipelines leave to the draw, drawn baked and dynamic
# on a real driver and compared (dynstate_test.cpp, run_tests.sh); lavapipe
# will do. SANITIZER=address,undefined for the sanitizer run.
SRC := ../../src
DEP := ../../dependencies
LRC := $(DEP)/libretro-common
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -I$(DEP)/Vulkan-Headers/include -DENABLE_VULKAN -DVK_NO_PROTOTYPES -DRSLC_EXTENDED -DRSLC_NO_TEXT
# what the core's precompiled header wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -ldl -lpthread -lm
ifneq (,$(findstring mingw,$(CXX)))
   LIBS := -static
   EXE := .exe
endif
LRC_SRCS := $(LRC)/gfx/shaders/rslc_front.c $(LRC)/gfx/shaders/rslc_ir.c $(LRC)/gfx/shaders/rslc_dxbc.c \
	$(LRC)/compat/compat_strl.c
SRCS := dynstate_test.cpp stubs.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp \
	$(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.cpp

dynstate_test$(EXE): $(SRCS) $(LRC_SRCS) $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(SRCS) *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f dynstate_test dynstate_test.exe *.o
