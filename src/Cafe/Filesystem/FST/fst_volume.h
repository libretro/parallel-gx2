#ifndef CAFE_FILESYSTEM_FST_VOLUME_H
#define CAFE_FILESYSTEM_FST_VOLUME_H

/* A volume of a disc image or of a title's content files: its FST decrypted
 * and parsed (fst_table.h), and its files read out of the source
 * (fst_source.h) a block at a time - decrypted, and checked against the
 * hashes the clusters carry or the TMD gives.
 *
 * Any number of threads read a volume at once, with no lock: each read
 * borrows one of the volume's block caches (made at the first read, then
 * kept), and a raw cluster's running content hash is advanced by the one
 * reader that claims its next block. A read allocates nothing once the
 * caches it uses are made. FST.h is the C++ over it. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "fst_source.h"
#include "fst_table.h"

RETRO_BEGIN_DECLS

typedef struct fst_volume fst_volume_t;

/* a cluster's content as its TMD gives it: what a raw cluster is hashed to */
typedef struct fst_volume_content
{
   uint64_t size;
   uint8_t hash[32];
   bool sha1;         /* SHA-1 in the first 20 octets, else SHA-256 */
} fst_volume_content_t;

/* why a volume did not open */
#define FST_VOLUME_OPEN_OK        0
#define FST_VOLUME_OPEN_READ      1 /* the FST could not be read */
#define FST_VOLUME_OPEN_INVALID   2 /* it is no valid FST */
#define FST_VOLUME_OPEN_CONTENTS  3 /* not one content for each cluster */
#define FST_VOLUME_OPEN_MEMORY    4

/* what went wrong in a read, where it did */
#define FST_VOLUME_READ_OK        0
#define FST_VOLUME_READ_IV        1 /* a raw block's IV could not be read */
#define FST_VOLUME_READ_RAW       2 /* a raw block could not be read */
#define FST_VOLUME_READ_RAW_HASH  3 /* a raw cluster is not what its TMD hash says */
#define FST_VOLUME_READ_HASHED    4 /* a hashed block could not be read */
#define FST_VOLUME_READ_H0        5 /* a hashed block's data is not its H0 hash */
#define FST_VOLUME_READ_MEMORY    6 /* no memory for a cache */
#define FST_VOLUME_READ_MODE      7 /* a cluster of a hash mode no reader knows */

typedef struct fst_volume_error
{
   int kind;          /* FST_VOLUME_READ_* */
   uint32_t cluster;
   uint32_t block;
} fst_volume_error_t;

/* the size of a sector, in which clusters are placed and raw blocks made */
#define FST_VOLUME_SECTOR_SIZE 0x8000

/* The FST of fst_size octets at fst_offset of cluster 0 of src, decrypted
 * with key (16 octets). contents, when not NULL, is one for each cluster
 * (content_count of them), from the TMD. take_source: the volume closes
 * src, also when it does not open. why, when not NULL, gets
 * FST_VOLUME_OPEN_*. */
fst_volume_t *fst_volume_open(fst_source_t *src, bool take_source,
      uint64_t fst_offset, uint32_t fst_size, const uint8_t *key,
      const fst_volume_content_t *contents, uint32_t content_count, int *why);
void fst_volume_free(fst_volume_t *vol);

const fst_table_t *fst_volume_table(const fst_volume_t *vol);
/* whether a read has found what is there not to be what it should be */
bool fst_volume_corrupted(const fst_volume_t *vol);

/* Up to size octets of the file entry is, from offset, into out: how many,
 * fewer at its end. A read cut short by what went wrong says what in err,
 * when err is not NULL (FST_VOLUME_READ_OK otherwise). */
uint32_t fst_volume_read(fst_volume_t *vol, uint32_t entry, uint32_t offset,
      uint32_t size, void *out, fst_volume_error_t *err);

RETRO_END_DECLS

#endif
