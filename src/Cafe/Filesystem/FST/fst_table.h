#ifndef CAFE_FILESYSTEM_FST_TABLE_H
#define CAFE_FILESYSTEM_FST_TABLE_H

/* A volume's file system table (FST), decrypted: its clusters, and its
 * entries - the directories and files - with their names. Parsed and checked
 * once, into one allocation; nothing changes after that, so any number of
 * threads look up, list and name entries at once, and none of it allocates:
 * a lookup walks the path where it lies, a listing is an index the caller
 * keeps, and a path is built in the caller's buffer.
 *
 * An entry is its index in the table; the root directory is 0. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct fst_table fst_table_t;

/* no entry: the end of a listing, or a lookup that found nothing */
#define FST_TABLE_NONE 0xFFFFFFFFUL

/* how a cluster's octets are laid out */
#define FST_HASH_RAW              0 /* encrypted, no hashes */
#define FST_HASH_RAW_STREAM       1 /* encrypted, its hash in the TMD */
#define FST_HASH_INTERLEAVED      2 /* blocks of 0x400 octets of hashes and 0xFC00 of data */

/* the FST's own header, and what it says of each cluster, are this size */
#define FST_TABLE_HEADER_SIZE     0x20

typedef struct fst_table_cluster
{
   uint32_t offset;   /* in sectors */
   uint32_t size;
   uint8_t  hash_mode; /* FST_HASH_*, or a mode no reader knows */
} fst_table_cluster_t;

typedef struct fst_table_file
{
   uint32_t offset;   /* in units of fst_table_offset_factor() */
   uint32_t size;
   uint16_t cluster;  /* below fst_table_cluster_count() */
} fst_table_file_t;

/* a directory's entries, a name at a time (fst_table_dir_open) */
typedef struct fst_table_dir
{
   uint32_t next;
   uint32_t end;
} fst_table_dir_t;

/* size octets of a decrypted FST; NULL if they are not a valid one */
fst_table_t *fst_table_parse(const void *fst, uint32_t size);
void fst_table_free(fst_table_t *table);

uint32_t fst_table_offset_factor(const fst_table_t *table);
bool fst_table_hash_disabled(const fst_table_t *table);
uint32_t fst_table_cluster_count(const fst_table_t *table);
/* NULL past the last */
const fst_table_cluster_t *fst_table_cluster(const fst_table_t *table, uint32_t index);
uint32_t fst_table_entry_count(const fst_table_t *table);
uint32_t fst_table_file_count(const fst_table_t *table);

/* The entry at path (len octets; '/' or '\\' between names, more than one
 * as one, "." for none, a leading and a trailing one let be; names compared
 * ignoring the case of A-Z): FST_TABLE_NONE if there is none, or if it is
 * not a file and only_files. An empty path is the root directory. */
uint32_t fst_table_lookup(const fst_table_t *table, const char *path, size_t len, bool only_files);

/* false for an index past the last */
bool fst_table_is_dir(const fst_table_t *table, uint32_t entry);
bool fst_table_is_file(const fst_table_t *table, uint32_t entry);
/* flagged as in another volume (an update's FST lists its base title's) */
bool fst_table_is_link(const fst_table_t *table, uint32_t entry);
/* "" for an index past the last */
const char *fst_table_name(const fst_table_t *table, uint32_t entry);
/* Its path from the root, '/' between names, into buf when it fits in size
 * octets with its terminator (else buf gets "" where size allows); its
 * length either way, 0 for an index past the last. */
size_t fst_table_path(const fst_table_t *table, uint32_t entry, char *buf, size_t size);
/* false if it is no file */
bool fst_table_file(const fst_table_t *table, uint32_t entry, fst_table_file_t *out);

/* false if dir is no directory */
bool fst_table_dir_open(const fst_table_t *table, uint32_t dir, fst_table_dir_t *it);
/* its next entry, the entries of a directory in it passed over;
 * FST_TABLE_NONE at the end */
uint32_t fst_table_dir_next(const fst_table_t *table, fst_table_dir_t *it);

RETRO_END_DECLS

#endif
