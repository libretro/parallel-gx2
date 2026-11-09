# The shader/PSO gate (pso_gate.cpp): what the shader path costs, against the
# baseline beside it. `make check` runs it; `make baseline` rewrites the
# baseline after a change that made the numbers better (or, accepted by the
# developer, worse).
LRC := ../../dependencies/libretro-common
SRC := ../../src
DEP := ../../dependencies
PACKS := $(DEP)/cemu_graphic_packs
CXX ?= g++
CC ?= gcc
FLAGS := -O2 -g -DHAVE_THREADS -DFILECACHE_COUNT_PROBES -DRSLC_EXTENDED -DRSLC_NO_TEXT -I$(SRC) -I$(SRC)/Common -I$(SRC)/Cafe -I$(SRC)/util -I$(LRC)/include
# the Vulkan pipeline key the replayed draws are counted with
FLAGS += -I$(DEP)/Vulkan-Headers/include -DVK_NO_PROTOTYPES -DENABLE_VULKAN
# what the core's precompiled header, which the file cache takes, wants
CXXFLAGS_CORE := -I$(DEP)/boost-compat -I$(DEP)/fmt/include -I$(DEP)/glm \
	-include $(SRC)/Common/precompiled.h
# malloc counted alongside operator new
LIBS := -lpthread -lm -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
FILESTREAM := $(SRC)/Common/unix/FileStream_unix.cpp
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
   FILESTREAM := $(SRC)/Common/windows/FileStream_win32.cpp
else
   # the Vulkan loader the key's setters would call through
   LIBS += -ldl
endif
LRC_SRCS := $(LRC)/gfx/shaders/rslc_front.c $(LRC)/gfx/shaders/rslc_ir.c $(LRC)/gfx/shaders/rslc_dxbc.c \
	$(LRC)/encodings/encoding_deflate.c $(LRC)/encodings/encoding_crc32.c \
	$(LRC)/features/features_cpu.c $(LRC)/compat/compat_strl.c $(LRC)/time/rtime.c \
	$(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c \
	$(LRC)/encodings/encoding_rzstd.c $(LRC)/encodings/encoding_xxh64.c
CXX_SRCS := pso_gate.cpp stubs.cpp $(SRC)/Cemu/FileCache/FileCache.cpp $(FILESTREAM) \
	$(SRC)/util/helpers/Deflate.cpp $(SRC)/Cafe/HW/Latte/Common/RegisterSerializer.cpp \
	$(SRC)/util/helpers/Serializer.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineKey.cpp \
	$(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.cpp $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineRecord.cpp \
	$(SRC)/util/helpers/MappedFile.cpp

all: pso_gate$(EXE)

pso_gate$(EXE): $(CXX_SRCS) ../shaders/shaders_test.cpp $(LRC_SRCS)
	for f in $(LRC_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) $(CXXFLAGS_CORE) -std=gnu++20 $(CXX_SRCS) *.o $(LIBS) -o $@
	rm -f *.o

check: pso_gate$(EXE)
	$(RUNNER) ./pso_gate$(EXE) $(PACKS) --baseline baseline.txt

baseline: pso_gate$(EXE)
	$(RUNNER) ./pso_gate$(EXE) $(PACKS) --write baseline.txt

.PHONY: all check baseline clean

clean:
	rm -f pso_gate pso_gate.exe *.o
