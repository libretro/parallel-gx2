# FileStream (src/Common/FileStream.cpp) and the libretro-common file I/O it
# stands on, for the tests that link it; HAVE_MMAP as the core's Makefile
# sets it (every translation unit: vfs.h lays its file out by it). Wants SRC,
# LRC and CC.
FILE_IO_CXX := $(SRC)/Common/FileStream.cpp
FILE_IO_C := $(LRC)/streams/file_stream.c $(LRC)/vfs/vfs_implementation.c \
	$(LRC)/file/file_path.c $(LRC)/file/file_path_io.c $(LRC)/file/retro_dirent.c \
	$(LRC)/encodings/encoding_utf.c $(LRC)/compat/fopen_utf8.c \
	$(LRC)/compat/compat_posix_string.c $(LRC)/compat/compat_strcasestr.c \
	$(LRC)/string/stdstring.c $(LRC)/string/rstrtod.c
FILE_IO_FLAGS :=
ifeq (,$(findstring mingw,$(CC)))
   FILE_IO_FLAGS += -DHAVE_MMAP
endif
# The core's own C over it - disc images (wud.c) on the shared file reader
# (shared_file.c), and an FST volume's sources over both (fst_source.c,
# which wants AES_C) - for the tests that link it, built as the C89 lane builds
# it: a C99 or C++ habit fails the build.
DISC_IMAGE_C := $(SRC)/Cafe/Filesystem/WUD/wud.c $(SRC)/Common/shared_file.c \
	$(SRC)/Cafe/Filesystem/FST/fst_source.c
C89_FLAGS := -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement \
	-Wno-long-long -Wno-variadic-macros -Wall -Werror
# The core's AES-128 (C89, built with C89_FLAGS) and libretro-common's
# crypto under it, for the tests that decrypt
AES_C := $(SRC)/util/crypto/aes128.c
AES_LRC_C := $(LRC)/crypto/crypto.c $(LRC)/features/features_cpu.c
# The lock-free cell under RcuCell.h (C89), for the tests that link
# anything that keeps one
RCU_C := $(SRC)/util/helpers/rcu_cell.c
