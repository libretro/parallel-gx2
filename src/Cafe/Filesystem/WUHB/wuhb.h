#ifndef CAFE_FILESYSTEM_WUHB_H
#define CAFE_FILESYSTEM_WUHB_H

/* A homebrew bundle (.wuhb): a RomFS image, read over shared_file.h. Mapped
 * where it can be: then an entry is read where it lies, a name compared
 * there, and a file's octets copied once from the pages to where the read
 * wants them. Where it cannot be mapped, through a stream lent to each
 * read. Nothing changes after opening, so any number of threads look up
 * and read at once; nothing is allocated past wuhb_open - a lookup walks
 * the path where it lies, and an entry's name goes into the caller's
 * buffer. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct wuhb wuhb_t;

/* no entry: the end of a list, or a lookup that found nothing */
#define WUHB_ENTRY_EMPTY 0xFFFFFFFFUL

/* wuhb_open flags */
#define WUHB_OPEN_NO_MAP 1 /* through streams even where it could be mapped */

typedef struct wuhb_dir
{
   uint32_t parent;
   uint32_t list_next;      /* the next directory in its parent's list */
   uint32_t dir_list_head;  /* its first directory */
   uint32_t file_list_head; /* its first file */
   uint32_t hash;           /* the next entry in its hash chain */
   uint32_t name_size;
} wuhb_dir_t;

typedef struct wuhb_file
{
   uint32_t parent;
   uint32_t list_next;      /* the next file in its directory's list */
   uint64_t offset;         /* in the file partition */
   uint64_t size;
   uint32_t hash;           /* the next entry in its hash chain */
   uint32_t name_size;
} wuhb_file_t;

/* path as the host file system has it (UTF-8); NULL if it does not open or
 * is no bundle */
wuhb_t *wuhb_open(const char *path, unsigned flags);
void wuhb_close(wuhb_t *wuhb);

/* whether its octets are lent from its mapping */
bool wuhb_is_mapped(const wuhb_t *wuhb);

/* The entry at path (len octets, '/' between names, a leading one and one
 * at the end of a directory's let be), in the file table when is_file,
 * else the directory table; WUHB_ENTRY_EMPTY where there is none. Names are
 * compared as they are; their hash, as RomFS has it, ignores case. */
uint32_t wuhb_lookup(const wuhb_t *wuhb, const char *path, size_t len, bool is_file);

/* An entry's fields, and its name into name - cut to name_cap - 1 octets
 * and NUL-terminated - when name is not NULL; false for no such entry */
bool wuhb_dir_entry(const wuhb_t *wuhb, uint32_t entry, wuhb_dir_t *out, char *name, size_t name_cap);
bool wuhb_file_entry(const wuhb_t *wuhb, uint32_t entry, wuhb_file_t *out, char *name, size_t name_cap);

/* 0 for no such file */
uint64_t wuhb_file_size(const wuhb_t *wuhb, uint32_t entry);
/* Up to len octets of the file at file_offset into dst: how many, short at
 * its end and where the image is */
uint64_t wuhb_read_file(wuhb_t *wuhb, uint32_t entry, uint64_t file_offset, uint64_t len, void *dst);

RETRO_END_DECLS

#endif
