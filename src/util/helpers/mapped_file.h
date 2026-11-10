#ifndef UTIL_HELPERS_MAPPED_FILE_H
#define UTIL_HELPERS_MAPPED_FILE_H

/* A file's octets, read-only, from open to close: lent from the file's
 * mapping where libretro-common's filestream maps it, read into memory
 * once where it cannot (a file only the frontend's VFS reaches, or a
 * platform without mappings). For a blob handed to an API that copies it
 * anyway, and for readers that must not share a stream's position: no
 * read buffer, and where mapped no copy.
 *
 * C, for the core's C; MappedFile.h is the C++ over it. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct mapped_file
{
   struct RFILE *stream; /* while its mapping is lent */
   const uint8_t *data;  /* size octets; NULL when empty */
   size_t size;
   uint8_t *copy;        /* what was read where it could not be mapped */
   bool opened;          /* whether there was a file to open, empty or not */
   bool mapped;          /* whether data is the file's pages */
} mapped_file_t;

/* path as the host file system has it (UTF-8). False, and mf empty, if it
 * is not a file that could be opened and read. */
bool mapped_file_open(mapped_file_t *mf, const char *path);
/* mf empty after, and closing it again does nothing */
void mapped_file_close(mapped_file_t *mf);

RETRO_END_DECLS

#endif
