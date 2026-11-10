#ifndef COMMON_SHARED_FILE_H
#define COMMON_SHARED_FILE_H

/* A file read at any offset from any number of threads at once, with no
 * lock, over libretro-common's filestream: a game image, a title's content
 * file, a homebrew bundle. Mapped where it can be: then its octets are lent
 * where they lie (shared_file_mapped) and a read is one copy from the
 * pages. Where it cannot be (a file only the frontend's VFS reaches, or one
 * too big for the address space), each read goes through a stream lent to
 * it from a few the file keeps, and a new one is opened only when every
 * kept one is out; one given back with no room to keep it is closed. */

#include <stdint.h>
#include <streams/file_stream.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct shared_file shared_file_t;

/* shared_file_open flags */
#define SHARED_FILE_NO_MAP 1 /* through streams even where it could be mapped */

/* path as the host file system has it (UTF-8); NULL for one that does not
 * open, and for a directory */
shared_file_t *shared_file_open(const char *path, unsigned flags);
void shared_file_close(shared_file_t *file);

uint64_t shared_file_size(const shared_file_t *file);
/* The whole file, valid until shared_file_close; NULL where not mapped */
const uint8_t *shared_file_mapped(const shared_file_t *file);
/* Up to len octets at offset into dst: how many, short at the end of the
 * file, 0 past it, -1 where the file could not be read */
int64_t shared_file_read_at(shared_file_t *file, void *dst, int64_t len, uint64_t offset);
/* Streams opened for reads, the one the file was opened with included;
 * 0 where mapped */
unsigned shared_file_streams_opened(const shared_file_t *file);

RETRO_END_DECLS

#endif
