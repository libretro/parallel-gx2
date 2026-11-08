DEBUG = 0
LINK_STATIC_LIBCPLUSPLUS = 1
LTO ?=

CORE_DIR := .
TARGET_NAME := parallel_gx2

# The libraries and the graphic packs are git submodules. A checkout made
# without them (a clone without --recursive, or one older than a submodule)
# fetches them here, on the build that would otherwise stop at the first
# header it cannot find. NO_SUBMODULE_FETCH=1 to leave the checkout alone.
ifneq ($(NO_SUBMODULE_FETCH),1)
SUBMODULE_STATUS := $(shell cd $(CORE_DIR) && [ -e .git ] && [ -f .gitmodules ] && \
	for p in `git config -f .gitmodules --get-regexp '\.path$$' | cut -d' ' -f2`; do \
		[ -n "`ls -A "$$p" 2>/dev/null`" ] && continue; \
		echo "Fetching the git submodules (this checkout has none in $$p)" >&2; \
		git submodule update --init --recursive >&2 || echo failed; \
		break; \
	done)
ifeq ($(SUBMODULE_STATUS),failed)
$(error Could not fetch the git submodules; run: git submodule update --init --recursive)
endif
endif

ifeq ($(ECHO),1)
   E := 1
endif

# Windows is recognised from the environment every Windows shell sets (all
# MSYS2 environments included, whatever uname -s reports there) or from a
# MinGW-w64 compiler, so a cross build needs only CC/CXX.
ifeq ($(platform),)
   platform = unix
   UNAME_S := $(shell uname -s 2>/dev/null)
   ifeq ($(OS),Windows_NT)
      platform = win
   else ifeq ($(UNAME_S),)
      platform = win
   else ifneq ($(findstring Darwin,$(UNAME_S)),)
      platform = osx
   else ifneq (,$(findstring mingw,$(shell $(CC) -dumpmachine 2>/dev/null)))
      platform = win
   endif
endif

# make's built-in default compiler is cc, which MinGW-w64 does not ship.
ifneq (,$(filter win win64,$(platform)))
   ifeq ($(origin CC),default)
      CC := gcc
   endif
   ifeq ($(origin CXX),default)
      CXX := g++
   endif
endif

# webOS SDK toolchains are plain Linux cross compilers; their name is what
# tells them apart.
ifneq (,$(findstring webos,$(CC)))
   ifneq (,$(findstring unix,$(platform)))
      override platform := webos
   endif
endif

# The graphic packs are packed by a tool built for, and run on, the
# build machine.
ifeq ($(OS),Windows_NT)
   HOSTCC ?= gcc
   HOST_EXE := .exe
else
   HOSTCC ?= cc
endif

# ----------------------------------------------------------------------------
# Target architecture: taken from the compiler (or the Apple -arch/-target
# selection), not the host, so cross builds pick the right recompiler.
# ----------------------------------------------------------------------------
ifeq ($(platform), osx)
   ifeq ($(CROSS_COMPILE),1)
      arch := $(firstword $(subst -, ,$(LIBRETRO_APPLE_PLATFORM)))
   endif
   ifeq ($(arch),)
      arch := $(shell uname -m)
   endif
else ifneq (,$(filter ios-arm64 tvos-arm64 android-arm64,$(platform)))
   arch := arm64
else ifeq ($(platform), android-x86_64)
   arch := x86_64
else ifeq ($(arch),)
   arch := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine 2>/dev/null)))
   ifeq ($(arch),)
      arch := $(shell uname -m)
   endif
endif

ifneq (,$(filter x86_64 amd64 x64,$(arch)))
   ARCH_X86_64 := 1
   arch := x86_64
else ifneq (,$(filter aarch64 arm64 arm,$(arch)))
   ARCH_ARM64 := 1
   arch := arm64
else
   $(error Unsupported target architecture '$(arch)': the core builds for x86_64 and arm64)
endif

HAVE_LIBUSB := 1
HAVE_VULKAN := 1
HAVE_OPENGL := 0
HAVE_METAL  := 0

FLAGS    :=
CFLAGS   :=
CXXFLAGS :=
LDFLAGS  :=
LIBS     :=

# Unix (Linux) and webOS
ifneq (,$(filter unix webos,$(platform)))
   OS_UNIX := 1
   OS_LINUX := 1
   TARGET  := $(TARGET_NAME)_libretro.so
   fpic    := -fPIC
   SHARED  := -shared -Wl,--no-undefined -Wl,--version-script=$(CORE_DIR)/src/libretro/link.T -Wl,-z,noexecstack
   ifeq ($(platform), webos)
      FLAGS += -DWEBOS
      HAVE_LIBUSB := 0
   else
      # No GL, EGL, GLX or X11 library: the context is the frontend's, and so
      # is every GL entry point (libretro's get_proc_address)
      HAVE_OPENGL := 1
   endif
   ifeq ($(LINK_STATIC_LIBCPLUSPLUS),1)
      LDFLAGS += -static-libgcc -static-libstdc++
   endif
   LIBS += -lpthread -ldl -lrt

# macOS
else ifeq ($(platform), osx)
   OS_UNIX  := 1
   OS_APPLE := 1
   OS_MACOS := 1
   TARGET   := $(TARGET_NAME)_libretro.dylib
   fpic     := -fPIC
   SHARED   := -dynamiclib
   HAVE_METAL := 1
   MACOS_MINVER ?= 13.4
   ifeq ($(CROSS_COMPILE),1)
      TARGET_RULE := -target $(arch)-apple-macos$(MACOS_MINVER) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
   else
      TARGET_RULE := -arch $(arch) -mmacosx-version-min=$(MACOS_MINVER)
   endif
   FLAGS   += $(TARGET_RULE)
   LDFLAGS += $(TARGET_RULE) -Wl,-dead_strip
   LIBS += -framework AppKit -framework CoreFoundation -framework Foundation \
	-framework IOKit -framework Security -framework Metal -framework QuartzCore \
	-lobjc

# iOS / tvOS
else ifneq (,$(filter ios-arm64 tvos-arm64,$(platform)))
   OS_UNIX  := 1
   OS_APPLE := 1
   OS_IOS   := 1
   fpic     := -fPIC
   SHARED   := -dynamiclib
   HAVE_METAL  := 1
   HAVE_VULKAN := 0
   HAVE_LIBUSB := 0
   ifeq ($(platform), tvos-arm64)
      TARGET := $(TARGET_NAME)_libretro_tvos.dylib
      MINVER ?= 16.4
      ifeq ($(IOSSDK),)
         IOSSDK := $(shell xcrun -sdk appletvos -show-sdk-path)
      endif
      MINVER_FLAG := -mappletvos-version-min=$(MINVER)
   else
      TARGET := $(TARGET_NAME)_libretro_ios.dylib
      IOS_MINVER ?= 16.4
      ifeq ($(IOSSDK),)
         IOSSDK := $(shell xcrun -sdk iphoneos -show-sdk-path)
      endif
      MINVER_FLAG := -miphoneos-version-min=$(IOS_MINVER)
   endif
   CC      = cc -arch arm64 -isysroot $(IOSSDK)
   CXX     = c++ -arch arm64 -isysroot $(IOSSDK)
   FLAGS   += $(MINVER_FLAG) -DIPHONEOS
   LDFLAGS += $(MINVER_FLAG)
   LIBS += -framework CoreFoundation -framework Foundation -framework Metal \
	-framework QuartzCore -framework IOKit -lobjc

# Android (NDK clang)
else ifneq (,$(filter android-arm64 android-x86_64,$(platform)))
   OS_UNIX    := 1
   OS_ANDROID := 1
   TARGET     := $(TARGET_NAME)_libretro_android.so
   fpic       := -fPIC
   SHARED     := -shared -Wl,--no-undefined -Wl,--version-script=$(CORE_DIR)/src/libretro/link.T -Wl,-z,noexecstack
   ANDROID_API ?= 28
   ifeq ($(platform), android-arm64)
      ANDROID_TRIPLE := aarch64-linux-android$(ANDROID_API)
   else
      ANDROID_TRIPLE := x86_64-linux-android$(ANDROID_API)
   endif
   NDK_ROOT ?= $(ANDROID_NDK_ROOT)
   ANDROID_NDK_LLVM ?= $(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64
   CC  = $(ANDROID_NDK_LLVM)/bin/clang --target=$(ANDROID_TRIPLE)
   CXX = $(ANDROID_NDK_LLVM)/bin/clang++ --target=$(ANDROID_TRIPLE)
   AR  = $(ANDROID_NDK_LLVM)/bin/llvm-ar
   FLAGS   += -DANDROID -funwind-tables
   LDFLAGS += -static-libstdc++
   LIBS += -landroid -llog -lm -ldl

# Windows (MinGW-w64, native MSYS2 or cross)
else
   OS_WIN := 1
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -Wl,--no-undefined -Wl,--version-script=$(CORE_DIR)/src/libretro/link.T
   FLAGS  += -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN
   HAVE_OPENGL := 1
   LDFLAGS += -static-libgcc -static-libstdc++ -static
   LIBS += -ldbghelp -lgdi32 -lntdll -lpsapi -liphlpapi -lws2_32 -lbcrypt \
	-ladvapi32 -lsetupapi -lole32 -luuid -lshell32 -luser32 -lkernel32 -lwinpthread
endif

GEN_DIR := $(CORE_DIR)/obj/gen
OBJ_DIR := $(CORE_DIR)/obj/$(platform)-$(arch)

include Makefile.common

# ----------------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------------
ifneq (,$(filter 1,$(OS_LINUX) $(OS_APPLE)))
   FLAGS += -D_FILE_OFFSET_BITS=64
endif
ifeq ($(OS_APPLE),1)
   FLAGS += -D_XOPEN_SOURCE -D_DARWIN_C_SOURCE
endif
FLAGS += -fno-strict-aliasing

# Everything the core does not export stays hidden; the libretro entry points
# carry visibility("default") through RETRO_API.
ifneq (,$(fpic))
   FLAGS += $(fpic)
endif
FLAGS    += -fvisibility=hidden
CXXFLAGS += -fvisibility-inlines-hidden

ifeq ($(DEBUG), 1)
   FLAGS += -O0 -g -D_DEBUG -DCEMU_DEBUG_ASSERT
else
   FLAGS += -O2 -DNDEBUG
endif

ifneq ($(LTO),)
   FLAGS   += $(LTO)
   LDFLAGS += $(LTO)
endif

ifneq ($(SANITIZER),)
   FLAGS   += -fsanitize=$(SANITIZER)
   LDFLAGS += -fsanitize=$(SANITIZER)
endif

# The shader/PSO trace (src/Cafe/HW/Latte/Core/PsoTrace.h): every compile a
# draw waits for, logged with its frame and duration. Never in release builds.
ifeq ($(PSO_TRACE),1)
   FLAGS += -DPSO_TRACE
endif

CFLAGS   := $(FLAGS) -std=gnu11 $(CFLAGS)
CXXFLAGS := $(FLAGS) -std=gnu++20 $(CXXFLAGS)
LDFLAGS  += $(fpic) $(SHARED)

# ----------------------------------------------------------------------------
# Objects
# ----------------------------------------------------------------------------
# Objects live under obj/<platform>-<arch>, one tree per target, so the
# submodules stay clean and a second architecture never links the first
# one's objects.
obj_of = $(patsubst %,$(OBJ_DIR)/%.o,$(1))

define LIB_template
$(1)_OBJECTS := $$(call obj_of,$$($(1)_SOURCES_C) $$($(1)_SOURCES_CXX) $$($(1)_SOURCES_ASM) $$($(1)_SOURCES_OBJCXX))
$$($(1)_OBJECTS): GROUP := $(1)
OBJECTS += $$($(1)_OBJECTS)
endef

OBJECTS :=
$(foreach g,$(LIB_GROUPS),$(eval $(call LIB_template,$(g))))
DEPS := $(OBJECTS:.o=.d)
ifneq ($(CORE_PCH),)
$(CORE_OBJECTS): $(CORE_PCH)
DEPS += $(CORE_PCH_STUB).d
endif

# The toolchain and configuration a tree was built with; a change wipes it,
# since nothing in an object's name says which compiler produced it.
BUILD_TAG := $(CC)|$(CXX)|$(platform)|$(arch)|$(DEBUG)|$(LTO)|$(SANITIZER)|$(HAVE_VULKAN)|$(HAVE_OPENGL)|$(HAVE_METAL)|$(HAVE_LIBUSB)|$(PSO_TRACE)
BUILD_TAG_STATUS := $(shell \
	if [ "`cat $(OBJ_DIR)/.build-tag 2>/dev/null`" != "$(BUILD_TAG)" ]; then \
		rm -rf $(OBJ_DIR); mkdir -p $(OBJ_DIR); \
		echo "$(BUILD_TAG)" > $(OBJ_DIR)/.build-tag; \
	fi)

# after the line above, which may have emptied the tree
ifneq ($(CORE_PCH),)
CORE_PCH_FLAGS_STATUS := $(shell mkdir -p $(OBJ_DIR)/pch; \
	echo '$(CORE_PCH_FLAGS)' > $(OBJ_DIR)/pch/flags.tmp; \
	cmp -s $(OBJ_DIR)/pch/flags.tmp $(OBJ_DIR)/pch/flags || mv -f $(OBJ_DIR)/pch/flags.tmp $(OBJ_DIR)/pch/flags; \
	rm -f $(OBJ_DIR)/pch/flags.tmp; \
	echo '$(CORE_PCH_STUB_LINE)' > $(CORE_PCH_STUB).tmp; \
	cmp -s $(CORE_PCH_STUB).tmp $(CORE_PCH_STUB) || mv -f $(CORE_PCH_STUB).tmp $(CORE_PCH_STUB); \
	rm -f $(CORE_PCH_STUB).tmp)
endif

.DEFAULT_GOAL := all

all: $(TARGET)

-include $(DEPS)


# The object list outgrows the 32 KiB Windows command line, so there the
# linker reads it from a response file ($(file) needs GNU make 4.0, which
# MSYS2 and the cross toolchains have; the Xcode make is 3.81).
ifeq ($(OS_WIN),1)
LINK_OBJECTS = @$(OBJ_DIR)/link-objects.rsp
else
LINK_OBJECTS = $(OBJECTS)
endif

$(TARGET): $(OBJECTS)
	@$(if $(E),echo LD $@,)
ifeq ($(OS_WIN),1)
	$(file >$(OBJ_DIR)/link-objects.rsp,$(OBJECTS))
endif
	$(CXX) -o $@ $(LINK_OBJECTS) $(LDFLAGS) $(LIBS)

$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@$(if $(E),echo CC $<,)
	$(CC) $(CFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	@$(if $(E),echo CXX $<,)
	$(CXX) $(CXXFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.cc.o: %.cc
	@mkdir -p $(@D)
	@$(if $(E),echo CXX $<,)
	$(CXX) $(CXXFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CXXFLAGS) -MMD -MP -c $< -o $@

# Objective-C++ cannot take the C++ precompiled header: the header itself.
$(OBJ_DIR)/%.mm.o: %.mm
	@mkdir -p $(@D)
	@$(if $(E),echo OBJCXX $<,)
	$(CXX) -x objective-c++ $(CXXFLAGS) $($(GROUP)_FLAGS) $(subst $(CORE_PCH_INCLUDE),$(CORE_PCH_HEADER),$($(GROUP)_CXXFLAGS)) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	@$(if $(E),echo AS $<,)
	$(CC) $(CFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_ASFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.s.o: %.s
	@mkdir -p $(@D)
	@$(if $(E),echo AS $<,)
	$(CC) -x assembler-with-cpp $(CFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_ASFLAGS) -c $< -o $@

clean:
	rm -rf $(CORE_DIR)/obj
	rm -f $(TARGET)

.PHONY: all clean
