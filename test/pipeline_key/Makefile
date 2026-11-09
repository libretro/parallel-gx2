# The Vulkan pipelines' key (src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp)
# against a replayed sequence of register states (run_tests.sh).
# SANITIZER=address,undefined for the sanitizer run.
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
FLAGS := -O1 -g -I$(SRC) -I$(DEP)/Vulkan-Headers/include -DVK_NO_PROTOTYPES -DENABLE_VULKAN
# what the core's precompiled header, which the register definitions want, wants;
# the Vulkan headers for the state the draw sets
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -ldl
ifneq (,$(findstring mingw,$(CXX)))
   LIBS := -static
   EXE := .exe
endif
# the loader's entry points the key's dynamic state setters call through
SRCS := pipeline_key_test.cpp stubs.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp \
	$(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.cpp

pipeline_key_test$(EXE): $(SRCS) $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(SRCS) $(LIBS) -o $@

clean:
	rm -f pipeline_key_test pipeline_key_test.exe
