# FileStream (src/Common/FileStream.cpp), the C under it (host_path.c) and
# the libretro-common file I/O they stand on, for the tests that link it; HAVE_MMAP as the core's Makefile
# sets it (every translation unit: vfs.h lays its file out by it). Wants SRC,
# LRC and CC.
FILE_IO_CXX := $(SRC)/Common/FileStream.cpp
FILE_IO_C := $(LRC)/streams/file_stream.c $(LRC)/vfs/vfs_implementation.c \
	$(LRC)/file/file_path.c $(LRC)/file/file_path_io.c $(LRC)/file/retro_dirent.c \
	$(LRC)/encodings/encoding_utf.c $(LRC)/compat/fopen_utf8.c \
	$(LRC)/compat/compat_posix_string.c $(LRC)/compat/compat_strcasestr.c \
	$(LRC)/string/stdstring.c $(LRC)/string/rstrtod.c $(SRC)/Common/host_path.c
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
# A volume's file system table (C89), for the tests that open FST volumes
FST_TABLE_C := $(SRC)/Cafe/Filesystem/FST/fst_table.c
# A volume's files read and checked a block at a time (C89), over the table
FST_VOLUME_C := $(SRC)/Cafe/Filesystem/FST/fst_volume.c
# Volumes opened from disc images and title folders (C89)
FST_OPEN_C := $(SRC)/Cafe/Filesystem/FST/fst_open.c
# A file's octets mapped or read once (C89), under MappedFile.h, for the
# tests that link anything that reads a file whole
MAPPED_FILE_C := $(SRC)/util/helpers/mapped_file.c
# The lock-free cell under RcuCell.h (C89), for the tests that link
# anything that keeps one
RCU_C := $(SRC)/util/helpers/rcu_cell.c
# Big-endian records read from and written to memory (C89), under
# Serializer.h, for the tests that link the shader and pipeline caches
MEM_STREAM_C := $(SRC)/util/helpers/mem_stream.c
# zlib streams through libretro-common's deflate (C89), under Deflate.h
ZLIB_STREAM_C := $(SRC)/util/helpers/zlib_stream.c
# The shader and pipeline cache files (C89), under FileCache.h, and the
# streams they compress and map entries with
FILE_CACHE_C := $(SRC)/Cemu/FileCache/file_cache.c $(ZLIB_STREAM_C) $(MAPPED_FILE_C)
# The XML tree (C89) under Xml.h, for the tests that read or write XML
XML_TREE_C := $(SRC)/util/xml/xml_tree.c
# A title's meta.xml, meta.ini, app.xml and cos.xml parsed (C89), under
# ParsedMetaXml.h and TitleInfo.h (with the ini reader the meta.ini takes),
# and what it needs of libretro-common
TITLE_XML_C := $(SRC)/Cafe/TitleList/title_xml.c $(SRC)/util/IniParser/ini_file.c
TITLE_XML_LRC_C := $(LRC)/formats/xml/rxml.c $(LRC)/encodings/encoding_crc32.c \
	$(LRC)/features/features_cpu.c
