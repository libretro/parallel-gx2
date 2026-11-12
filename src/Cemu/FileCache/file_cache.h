#ifndef CEMU_FILECACHE_FILE_CACHE_H
#define CEMU_FILECACHE_FILE_CACHE_H

/* A cache file: entries by a two-word name, in the format standalone Cemu
 * reads and writes (the shader, pipeline and graphic pack caches). C89.
 *
 * No lock. Every change to a file - a write, a delete, the file table's
 * growth - is made by one writer thread, shared by every cache, in the
 * order the changes were asked for. A reader looks an entry up in what
 * the writer has published and reads its data through a mapping of the
 * file, without waiting for anything.
 *
 * file_cache_global_init() before anything else, once (FileCache.h does it
 * from a function-local static). */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct file_cache file_cache_t;

/* the writer thread's queue and the wake for finished writes; false for
 * no memory. Calling it again does nothing. */
bool file_cache_global_init(void);
/* Writes out what is queued and ends the writer thread (the next write
 * starts it again). For a libretro core to call before it is unloaded. */
void file_cache_stop_writer(void);

/* path as the host file system has it (UTF-8). Create makes a new, empty
 * file in its place; open takes an existing one, if its extra version is
 * extra_version (any, for open_any), else creates one when allowed. NULL
 * for a file that cannot be made or read as one, and then *corrupt (when
 * not NULL) true if it was a cache file it could not read. */
file_cache_t *file_cache_create(const char *path, uint32_t extra_version);
file_cache_t *file_cache_open(const char *path, bool allow_create,
      uint32_t extra_version, bool *corrupt);
file_cache_t *file_cache_open_any(const char *path, bool *corrupt);
/* after its writes are done */
void file_cache_close(file_cache_t *fc);

/* on by default: entries written zlib-compressed */
void file_cache_use_compression(file_cache_t *fc, bool enable);

/* size octets at data copied and written by the writer thread; found by a
 * read once written, which file_cache_wait waits for */
void file_cache_add(file_cache_t *fc, uint64_t name1, uint64_t name2,
      const void *data, size_t size);
/* Whether it was there; gone when this returns (it waits for the writes
 * asked for before it, and for itself) */
bool file_cache_delete(file_cache_t *fc, uint64_t name1, uint64_t name2);
/* until every write asked of this cache so far is in the file */
void file_cache_wait(file_cache_t *fc);

/* Where an entry's data goes: alloc(ctx, size) is asked for size octets
 * and gives them, or NULL to give up */
typedef void *(*file_cache_alloc_t)(void *ctx, size_t size);
/* the entry's data, decompressed, into what alloc gives; false (and alloc
 * never asked, or its octets not to be used) for none, or a broken one */
bool file_cache_get(file_cache_t *fc, uint64_t name1, uint64_t name2,
      file_cache_alloc_t alloc, void *ctx);
/* the same by file table index, its name in *name1, *name2 */
bool file_cache_get_index(file_cache_t *fc, int32_t index, uint64_t *name1,
      uint64_t *name2, file_cache_alloc_t alloc, void *ctx);
bool file_cache_has(file_cache_t *fc, uint64_t name1, uint64_t name2);
/* the entries in it, and how far an index may go */
int32_t file_cache_count(file_cache_t *fc);
int32_t file_cache_max_index(file_cache_t *fc);

#ifdef FILECACHE_COUNT_PROBES
/* file table entries a lookup or a write has looked at (the PSO gate's) */
size_t file_cache_probes(void);
void file_cache_probes_reset(void);
#endif
#ifdef FILECACHE_TEST_HOOKS
/* called by a reader between finding an entry and reading it */
extern void (*file_cache_read_hook)(uint64_t name1, uint64_t name2);
#endif

RETRO_END_DECLS

#endif
