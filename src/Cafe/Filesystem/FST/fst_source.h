#ifndef CAFE_FILESYSTEM_FST_SOURCE_H
#define CAFE_FILESYSTEM_FST_SOURCE_H

/* Where an FST volume's encrypted octets come from: a disc image (.wud,
 * .wux, wud.h), a title's content files (.app, one a cluster, shared_file.h),
 * or a reader of the caller's own (fst_source_new). The two files are
 * mapped where they can be, and then a block is decrypted straight from
 * the file's pages into the cache that keeps it - no read, no buffer, no
 * copy before the decryption, which has to write the octets anyway. Where
 * they cannot be (a file only the frontend's VFS reaches), the octets are
 * read into that same destination and decrypted there.
 *
 * Read from any number of threads at once: what is mapped is only read,
 * and what is not is read through a stream lent to each reader. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct fst_source fst_source_t;

#define FST_HASHED_BLOCK_SIZE 0x10000
#define FST_HASHED_BLOCK_HASH_SIZE 0x400

/* fst_source_open_* flags */
#define FST_SOURCE_NO_MAP 1 /* through streams even where it could be mapped */

/* A reader of the caller's own: offset is in the cluster at cluster_offset */
typedef struct fst_source_ops
{
   /* up to size octets into dst; how many there were */
   uint64_t (*read)(void *ctx, uint16_t cluster, uint64_t cluster_offset,
         uint64_t offset, void *dst, uint64_t size);
   /* the same octets where they lie, for as long as the source lives;
    * NULL where they cannot all be lent in one piece. May be NULL. */
   const uint8_t *(*lend)(void *ctx, uint16_t cluster, uint64_t cluster_offset,
         uint64_t offset, uint64_t size);
   /* the source is closed; may be NULL */
   void (*close)(void *ctx);
} fst_source_ops_t;

fst_source_t *fst_source_new(const fst_source_ops_t *ops, void *ctx);

/* A disc image (path as the host file system has it, UTF-8) */
fst_source_t *fst_source_open_disc(const char *path, unsigned flags);
/* A title's content: paths[i] the file of cluster clusters[i]; NULL if one
 * does not open or a cluster comes twice */
fst_source_t *fst_source_open_app(const char *const *paths, const uint16_t *clusters,
      size_t count, unsigned flags);

void fst_source_close(fst_source_t *src);

/* Added to every offset read: where a disc's partition starts */
void fst_source_set_base_offset(fst_source_t *src, uint64_t base);
uint64_t fst_source_get_base_offset(const fst_source_t *src);

uint64_t fst_source_read(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, void *dst, uint64_t size);
const uint8_t *fst_source_lend(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint64_t size);
/* size octets, lent where they can be and otherwise read into scratch:
 * where they are, or NULL when not all of them could be had */
const uint8_t *fst_source_fetch(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint8_t *scratch, uint64_t size);

/* An unhashed block of size octets (a multiple of 16), decrypted into dst
 * with iv; next_iv gets its last 16 encrypted octets, the next block's IV */
bool fst_source_read_raw_block(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint8_t *dst, uint32_t size, const uint8_t *key,
      const uint8_t *iv, uint8_t *next_iv);
/* A hashed block, FST_HASHED_BLOCK_SIZE octets: the hashes decrypted with
 * no IV into hash_dst, then the data with the first 16 octets of the
 * block's H0 hash (h0_index of the 16) as the IV into data_dst. data_dst
 * may be hash_dst + FST_HASHED_BLOCK_HASH_SIZE, the block in one piece. */
bool fst_source_read_hashed_block(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint8_t *hash_dst, uint8_t *data_dst, const uint8_t *key,
      uint32_t h0_index);

RETRO_END_DECLS

#endif
