# The Vulkan context negotiation the core offers the frontend
# (src/libretro/LibretroVkDevice.cpp) (run_tests.sh). SANITIZER=address,undefined
# for the sanitizer run. The device part wants a Vulkan driver (lavapipe will do).
SRC := ../../src
DEP := ../../dependencies
LRC := $(DEP)/libretro-common
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(SRC)/libretro -I$(LRC)/include -I$(DEP)/Vulkan-Headers/include -DENABLE_VULKAN -DVK_NO_PROTOTYPES
# what the core's precompiled header wants
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
SRCS := vknegotiate_test.cpp stubs.cpp $(SRC)/libretro/LibretroVkDevice.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.cpp

vknegotiate_test$(EXE): $(SRCS) $(SRC)/libretro/LibretroVkDevice.h
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(SRCS) $(LIBS) -o $@

clean:
	rm -f vknegotiate_test vknegotiate_test.exe
