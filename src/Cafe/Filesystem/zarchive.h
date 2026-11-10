#ifndef CAFE_FILESYSTEM_ZARCHIVE_H
#define CAFE_FILESYSTEM_ZARCHIVE_H

/* A Wii U archive (.wua, and Miiverse's OfflineDB.zar) read with
 * libretro-common's zar, over shared_file.h: opened the way the rest of the
 * core opens a game image, so an archive the host application hands over -
 * an Android content:// URI behind SAF, say - reads like any other file.
 *
 * Mapped where it can be, once, for as long as it is open: lookups and
 * listings read the index where it lies, and a file's octets go from the
 * mapping straight to where the read wants them - a block kept as it is
 * copied once, a compressed one decoded from the pages. Where it cannot be
 * mapped (a file only the frontend's VFS reaches, or one too big for the
 * address space) the index - everything past the blocks: records, names,
 * tree and footer - is read into memory once on opening, and the blocks
 * through a stream lent to each read. Either way nothing changes after
 * opening, so any number of threads look up, list and read at once, each
 * with a zar_t of its own: lent from a pool the archive keeps, with no
 * lock, and given back after, so a lookup or a read makes no reader - and
 * no Zstandard decoder - of its own once the pool has one free, and a block
 * decoded for one file is there for the next read that wants it. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <formats/zar.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct zarchive zarchive_t;

/* zarchive_open flags */
#define ZARCHIVE_OPEN_NO_MAP 1 /* through streams even where it could be mapped */

/* The longest path a listing takes, its NUL included */
#define ZARCHIVE_DIR_PATH_MAX 1024

/* path as the host file system has it (UTF-8). NULL if it cannot be read
 * as an archive, and then in why_not, if given, what was found instead,
 * cut to why_cap. */
zarchive_t *zarchive_open(const char *path, unsigned flags, char *why_not, size_t why_cap);
void zarchive_close(zarchive_t *ar);

const char *zarchive_path(const zarchive_t *ar);
uint64_t zarchive_size(const zarchive_t *ar);
/* whether the octets are lent from the archive's mapping */
bool zarchive_is_mapped(const zarchive_t *ar);
/* how many readers have been made, the first one included */
unsigned zarchive_readers_made(const zarchive_t *ar);
/* streams opened for reads where it is not mapped (shared_file.h); 0 where
 * it is */
unsigned zarchive_streams_opened(const zarchive_t *ar);

/* A file or a directory at path (len octets; the empty path is the root) */
bool zarchive_lookup(const zarchive_t *ar, const char *path, size_t len, zar_file_t *out);

/* Up to size octets of file at offset into dst: how many, fewer only at
 * the file's end or on an error. Nothing allocated once the archive has a
 * reader free. */
uint64_t zarchive_read(const zarchive_t *ar, const zar_file_t *file, uint64_t offset,
      void *dst, uint64_t size);

/* A directory's entries, in name order, one at a time: the listing holds a
 * reader of the archive's from open to close. Nothing allocated. */
typedef struct zarchive_dir
{
   const zarchive_t *ar;
   zar_t *z;
   int err;
   char path[ZARCHIVE_DIR_PATH_MAX];
} zarchive_dir_t;

bool zarchive_dir_open(const zarchive_t *ar, zarchive_dir_t *dir, const char *path, size_t len);
/* The next entry: its name (NUL-terminated, valid until the next call) and
 * node; false past the last one, or on an error (zarchive_dir_failed) */
bool zarchive_dir_next(zarchive_dir_t *dir, const char **name, size_t *len, zar_file_t *node);
/* whether the listing ended on an error rather than its end */
bool zarchive_dir_failed(const zarchive_dir_t *dir);
void zarchive_dir_close(zarchive_dir_t *dir);

RETRO_END_DECLS

#endif
