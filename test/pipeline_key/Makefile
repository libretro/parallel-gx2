# The Vulkan pipelines' key (src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp)
# against a replayed sequence of register states (run_tests.sh).
# SANITIZER=address,undefined for the sanitizer run.
SRC := ../../src
DEP := ../../dependencies
CXX ?= g++
FLAGS := -O1 -g -I$(SRC)
# what the core's precompiled header, which the register definitions want, wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm -DFMT_HEADER_ONLY \
	-include $(SRC)/Common/precompiled.h
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CXX)))
   LIBS += -static
   EXE := .exe
endif
SRCS := pipeline_key_test.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp

pipeline_key_test$(EXE): $(SRCS) $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.h
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(SRCS) $(LIBS) -o $@

clean:
	rm -f pipeline_key_test pipeline_key_test.exe
