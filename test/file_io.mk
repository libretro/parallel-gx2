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
