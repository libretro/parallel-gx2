/* A volume's files read and checked a block at a time (fst_volume.h) */

#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <lrc_hash.h>

#include "util/crypto/aes128.h"
#include "fst_volume.h"

#define HASH_RAW               0
#define HASH_RAW_STREAM        1
#define HASH_INTERLEAVED       2

#define BLOCK_SIZE             FST_HASHED_BLOCK_SIZE       /* 0x10000 */
#define BLOCK_HASH_SIZE        FST_HASHED_BLOCK_HASH_SIZE  /* 0x400 */
#define BLOCK_DATA_SIZE        (BLOCK_SIZE - BLOCK_HASH_SIZE)

/* Decrypted blocks kept for the reads that follow: a fixed set of slots,
 * their memory made at a cache's first use. A read that wants all of a
 * block, in a read of more than one, is decrypted straight to where it
 * goes instead. */
#define CACHE_SLOTS            32
#define CACHE_SLOT_SIZE        BLOCK_SIZE /* a hashed block, or a raw one */
#define CACHE_POOL             4
#define NO_KEY                 (~(uint64_t)0)

/* a raw cluster's next block to hash has this set while a reader hashes it */
#define HASHING                0x40000000

typedef struct cache_slot
{
   uint64_t key;      /* cache_key(); NO_KEY when empty */
   uint64_t last_use;
   uint8_t next_iv[16]; /* a raw block's: the next one's IV */
} cache_slot_t;

typedef struct block_cache
{
   uint8_t *memory;
   cache_slot_t slots[CACHE_SLOTS];
   uint64_t uses;
   /* the IV of the raw block after the last one decrypted outside it */
   uint64_t next_iv_key;
   uint8_t next_iv[16];
} block_cache_t;

typedef struct fst_cluster
{
   uint64_t offset;   /* in octets */
   uint8_t hash_mode;
   /* a raw cluster's content hash, from the TMD, and the hash of what has
    * been read of it in order so far */
   bool has_hash;
   bool sha1;
   uint8_t hash[32];
   uint64_t size;
   union
   {
      struct sha1_state sha1;
      struct sha256_state sha256;
   } running;
   retro_atomic_int_t next_block; /* | HASHING while one hashes it */
} fst_cluster_t;

struct fst_volume
{
   fst_source_t *src;
   bool own_source;
   fst_table_t *table;
   uint8_t key[16];
   retro_atomic_int_t corrupted;
   retro_atomic_ptr_t caches[CACHE_POOL];
   uint32_t cluster_count;
   fst_cluster_t clusters[1]; /* cluster_count of them */
};

static uint64_t cache_key(bool hashed, uint32_t cluster, uint32_t block)
{
   return ((uint64_t)hashed << 63) | ((uint64_t)cluster << 32) | block;
}

/* --- making and letting go --------------------------------------------- */

fst_volume_t *fst_volume_open(fst_source_t *src, bool take_source,
      uint64_t fst_offset, uint32_t fst_size, const uint8_t *key,
      const fst_volume_content_t *contents, uint32_t content_count, int *why)
{
   uint8_t *fst = NULL;
   const uint8_t *encrypted;
   uint8_t iv[16];
   uint32_t padded = (fst_size + 15) & ~(uint32_t)15;
   fst_table_t *table;
   fst_volume_t *vol;
   uint32_t clusters, i;
   int dummy;
   if (!why)
      why = &dummy;
   *why = FST_VOLUME_OPEN_READ;
   if (!src || !key || fst_size < FST_TABLE_HEADER_SIZE || padded < fst_size)
      goto fail;
   /* read and decrypted, for the table to be made from; let go then */
   fst = (uint8_t*)malloc(padded);
   if (!fst)
   {
      *why = FST_VOLUME_OPEN_MEMORY;
      goto fail;
   }
   encrypted = fst_source_fetch(src, 0, 0, fst_offset, fst, padded);
   if (!encrypted)
      goto fail;
   memset(iv, 0, sizeof(iv));
   AES128_CBC_decrypt(fst, encrypted, padded, key, iv);
   table = fst_table_parse(fst, fst_size);
   free(fst);
   fst = NULL;
   if (!table)
   {
      *why = FST_VOLUME_OPEN_INVALID;
      goto fail;
   }
   clusters = fst_table_cluster_count(table);
   /* each content in the TMD is one cluster */
   if (contents && content_count != clusters)
   {
      fst_table_free(table);
      *why = FST_VOLUME_OPEN_CONTENTS;
      goto fail;
   }
   vol = (fst_volume_t*)calloc(1, sizeof(*vol)
         + (clusters ? clusters - 1 : 0) * sizeof(vol->clusters[0]));
   if (!vol)
   {
      fst_table_free(table);
      *why = FST_VOLUME_OPEN_MEMORY;
      goto fail;
   }
   vol->src = src;
   vol->own_source = take_source;
   vol->table = table;
   memcpy(vol->key, key, sizeof(vol->key));
   retro_atomic_int_init(&vol->corrupted, 0);
   for (i = 0; i < CACHE_POOL; i++)
      retro_atomic_ptr_init(&vol->caches[i], NULL);
   vol->cluster_count = clusters;
   for (i = 0; i < clusters; i++)
   {
      fst_cluster_t *c = &vol->clusters[i];
      const fst_table_cluster_t *t = fst_table_cluster(table, i);
      c->offset = (uint64_t)t->offset * FST_VOLUME_SECTOR_SIZE;
      c->hash_mode = t->hash_mode;
      retro_atomic_int_init(&c->next_block, 0);
      /* a hashed cluster checks itself; a raw one only with its TMD hash */
      if (contents && (c->hash_mode == HASH_RAW || c->hash_mode == HASH_RAW_STREAM))
      {
         c->has_hash = true;
         c->sha1 = contents[i].sha1;
         c->size = contents[i].size;
         memcpy(c->hash, contents[i].hash, sizeof(c->hash));
         if (c->sha1)
            sha1_stream_init(&c->running.sha1);
         else
            sha256_stream_init(&c->running.sha256, 0);
      }
   }
   *why = FST_VOLUME_OPEN_OK;
   return vol;

fail:
   free(fst);
   if (take_source && src)
      fst_source_close(src);
   return NULL;
}

static void cache_free(block_cache_t *cache)
{
   if (!cache)
      return;
   free(cache->memory);
   free(cache);
}

void fst_volume_free(fst_volume_t *vol)
{
   unsigned i;
   if (!vol)
      return;
   for (i = 0; i < CACHE_POOL; i++)
      cache_free((block_cache_t*)retro_atomic_exchange_ptr(&vol->caches[i], NULL));
   if (vol->own_source)
      fst_source_close(vol->src);
   fst_table_free(vol->table);
   free(vol);
}

const fst_table_t *fst_volume_table(const fst_volume_t *vol)
{
   return vol->table;
}

bool fst_volume_corrupted(const fst_volume_t *vol)
{
   return retro_atomic_load_acquire_int((retro_atomic_int_t*)&vol->corrupted) != 0;
}

static bool fail(fst_volume_t *vol, fst_volume_error_t *err, int kind,
      uint32_t cluster, uint32_t block)
{
   /* what is there is not what it should be: not so for no memory, nor
    * for a mode of hashing no reader knows */
   if (kind != FST_VOLUME_READ_MEMORY && kind != FST_VOLUME_READ_MODE)
      retro_atomic_store_release_int(&vol->corrupted, 1);
   err->kind = kind;
   err->cluster = cluster;
   err->block = block;
   return false;
}

/* --- caches -------------------------------------------------------------- */

/* One of the pool's, or one more made now and kept after: a reader's own
 * while it reads, so no two readers share one. */
static block_cache_t *cache_acquire(fst_volume_t *vol)
{
   block_cache_t *cache;
   unsigned i;
   for (i = 0; i < CACHE_POOL; i++)
   {
      if (!retro_atomic_load_relaxed_ptr(&vol->caches[i]))
         continue;
      cache = (block_cache_t*)retro_atomic_exchange_ptr(&vol->caches[i], NULL);
      if (cache)
         return cache;
   }
   cache = (block_cache_t*)calloc(1, sizeof(*cache));
   if (cache)
   {
      for (i = 0; i < CACHE_SLOTS; i++)
         cache->slots[i].key = NO_KEY;
      cache->next_iv_key = NO_KEY;
   }
   return cache;
}

static void cache_release(fst_volume_t *vol, block_cache_t *cache)
{
   unsigned i;
   for (i = 0; i < CACHE_POOL; i++)
   {
      if (!retro_atomic_load_relaxed_ptr(&vol->caches[i])
            && retro_atomic_cas_ptr(&vol->caches[i], NULL, cache))
         return;
   }
   cache_free(cache);
}

static cache_slot_t *cache_find(block_cache_t *cache, uint64_t key)
{
   unsigned i;
   for (i = 0; i < CACHE_SLOTS; i++)
   {
      if (cache->slots[i].key == key)
         return &cache->slots[i];
   }
   return NULL;
}

/* the least recently used slot, emptied; NULL when there is no memory -
 * made at the first use, not when a volume is only opened to be looked at
 * (the title list) */
static cache_slot_t *cache_take(block_cache_t *cache)
{
   cache_slot_t *oldest = &cache->slots[0];
   unsigned i;
   if (!cache->memory)
   {
      cache->memory = (uint8_t*)malloc((size_t)CACHE_SLOTS * CACHE_SLOT_SIZE);
      if (!cache->memory)
         return NULL;
   }
   for (i = 1; i < CACHE_SLOTS; i++)
   {
      if (cache->slots[i].last_use < oldest->last_use)
         oldest = &cache->slots[i];
   }
   oldest->key = NO_KEY;
   return oldest;
}

static uint8_t *cache_data(block_cache_t *cache, const cache_slot_t *slot)
{
   return cache->memory + (size_t)(slot - cache->slots) * CACHE_SLOT_SIZE;
}

/* --- blocks -------------------------------------------------------------- */

/* A raw block's IV: for the first, the cluster's index; for any other, the
 * last 16 encrypted octets of the one before (AES-CBC) - kept with that
 * block when it is cached or was the last decrypted outside the cache, and
 * otherwise read (or lent) from the source. */
static bool raw_block_iv(fst_volume_t *vol, block_cache_t *cache,
      uint32_t cluster, uint32_t block, uint8_t *iv, fst_volume_error_t *err)
{
   const cache_slot_t *prev;
   const uint8_t *at;
   uint64_t prev_key;
   memset(iv, 0, 16);
   if (block == 0)
   {
      iv[0] = (uint8_t)(cluster >> 8);
      iv[1] = (uint8_t)cluster;
      return true;
   }
   prev_key = cache_key(false, cluster, block - 1);
   prev = cache_find(cache, prev_key);
   if (prev)
   {
      memcpy(iv, prev->next_iv, 16);
      return true;
   }
   if (cache->next_iv_key == prev_key)
   {
      memcpy(iv, cache->next_iv, 16);
      return true;
   }
   at = fst_source_fetch(vol->src, (uint16_t)cluster, vol->clusters[cluster].offset,
         (uint64_t)block * FST_VOLUME_SECTOR_SIZE - 16, iv, 16);
   if (!at)
      return fail(vol, err, FST_VOLUME_READ_IV, cluster, block);
   memmove(iv, at, 16);
   return true;
}

/* The raw cluster's running hash, where block is the next of it: the reader
 * that claims the block hashes it, any other goes on, and the next block
 * waits for the next reader that decrypts it. False when the last block
 * leaves a hash that is not the TMD's. */
static bool raw_block_hash(fst_volume_t *vol, uint32_t cluster, uint32_t block,
      const uint8_t *data, fst_volume_error_t *err)
{
   fst_cluster_t *c = &vol->clusters[cluster];
   uint64_t blocks;
   uint64_t size = FST_VOLUME_SECTOR_SIZE;
   bool last;
   bool mismatch = false;
   if (!c->has_hash || block >= HASHING
         || retro_atomic_load_acquire_int(&c->next_block) != (int)block
         || !retro_atomic_cas_int(&c->next_block, (int)block, (int)(block | HASHING)))
      return true;
   /* the last block holds what is left of the content, a part of a block
    * where its size is not a whole number of them */
   blocks = (c->size + FST_VOLUME_SECTOR_SIZE - 1) / FST_VOLUME_SECTOR_SIZE;
   if (blocks == 0)
      blocks = 1;
   last = (uint64_t)block == blocks - 1;
   if (last)
      size = c->size - (uint64_t)block * FST_VOLUME_SECTOR_SIZE;
   if (c->sha1)
      sha1_stream_update(&c->running.sha1, data, (size_t)size);
   else
      sha256_stream_update(&c->running.sha256, data, (size_t)size);
   if (last)
   {
      uint8_t hash[32];
      if (c->sha1)
         sha1_stream_final(&c->running.sha1, hash);
      else
         sha256_stream_final(&c->running.sha256, hash);
      mismatch = memcmp(hash, c->hash, c->sha1 ? 20 : 32) != 0;
   }
   retro_atomic_store_release_int(&c->next_block, (int)block + 1);
   if (mismatch)
      return fail(vol, err, FST_VOLUME_READ_RAW_HASH, cluster, block);
   return true;
}

/* a raw block decrypted into dst, its successor's IV into next_iv */
static bool raw_block(fst_volume_t *vol, block_cache_t *cache, uint32_t cluster,
      uint32_t block, uint8_t *dst, uint8_t *next_iv, fst_volume_error_t *err)
{
   uint8_t iv[16];
   if (!raw_block_iv(vol, cache, cluster, block, iv, err))
      return false;
   if (!fst_source_read_raw_block(vol->src, (uint16_t)cluster, vol->clusters[cluster].offset,
            (uint64_t)block * FST_VOLUME_SECTOR_SIZE, dst, FST_VOLUME_SECTOR_SIZE,
            vol->key, iv, next_iv))
      return fail(vol, err, FST_VOLUME_READ_RAW, cluster, block);
   return raw_block_hash(vol, cluster, block, dst, err);
}

/* a hashed block's hashes and data, its data checked against its H0 hash */
static bool hashed_block(fst_volume_t *vol, uint32_t cluster, uint32_t block,
      uint8_t *hash_dst, uint8_t *data_dst, fst_volume_error_t *err)
{
   uint8_t h0[20];
   if (!fst_source_read_hashed_block(vol->src, (uint16_t)cluster, vol->clusters[cluster].offset,
            (uint64_t)block * BLOCK_SIZE, hash_dst, data_dst, vol->key, block % 16))
      return fail(vol, err, FST_VOLUME_READ_HASHED, cluster, block);
   SHA1Digest(data_dst, BLOCK_DATA_SIZE, h0);
   if (memcmp(h0, hash_dst + 20 * (block % 16), 20) != 0)
      return fail(vol, err, FST_VOLUME_READ_H0, cluster, block);
   return true;
}

/* from the cache, decrypted into it first when not there */
static const uint8_t *cached_raw_block(fst_volume_t *vol, block_cache_t *cache,
      uint32_t cluster, uint32_t block, fst_volume_error_t *err)
{
   uint64_t key = cache_key(false, cluster, block);
   cache_slot_t *slot = cache_find(cache, key);
   if (!slot)
   {
      slot = cache_take(cache);
      if (!slot)
      {
         fail(vol, err, FST_VOLUME_READ_MEMORY, cluster, block);
         return NULL;
      }
      if (!raw_block(vol, cache, cluster, block, cache_data(cache, slot), slot->next_iv, err))
         return NULL;
      slot->key = key;
   }
   slot->last_use = ++cache->uses;
   return cache_data(cache, slot);
}

static const uint8_t *cached_hashed_data(fst_volume_t *vol, block_cache_t *cache,
      uint32_t cluster, uint32_t block, fst_volume_error_t *err)
{
   uint64_t key = cache_key(true, cluster, block);
   cache_slot_t *slot = cache_find(cache, key);
   if (!slot)
   {
      uint8_t *data;
      slot = cache_take(cache);
      if (!slot)
      {
         fail(vol, err, FST_VOLUME_READ_MEMORY, cluster, block);
         return NULL;
      }
      data = cache_data(cache, slot);
      if (!hashed_block(vol, cluster, block, data, data + BLOCK_HASH_SIZE, err))
         return NULL;
      slot->key = key;
   }
   slot->last_use = ++cache->uses;
   return cache_data(cache, slot) + BLOCK_HASH_SIZE;
}

/* --- reads --------------------------------------------------------------- */

/* A raw cluster's file: blocks of a sector, each its own IV chain link.
 * What could be read before a failure is kept. */
static uint32_t read_raw(fst_volume_t *vol, block_cache_t *cache, uint32_t cluster,
      uint64_t at, uint32_t size, uint8_t *out, fst_volume_error_t *err)
{
   uint32_t left = size;
   while (left > 0)
   {
      uint32_t block = (uint32_t)(at / FST_VOLUME_SECTOR_SIZE);
      uint32_t in_block = (uint32_t)(at % FST_VOLUME_SECTOR_SIZE);
      uint32_t n = FST_VOLUME_SECTOR_SIZE - in_block;
      if (n > left)
         n = left;
      if (n == FST_VOLUME_SECTOR_SIZE && size >= 2 * FST_VOLUME_SECTOR_SIZE
            && !cache_find(cache, cache_key(false, cluster, block)))
      {
         /* all of a block, in a read of more than one: decrypted where it
          * goes rather than kept, its successor's IV kept instead */
         uint8_t next_iv[16];
         if (!raw_block(vol, cache, cluster, block, out, next_iv, err))
            break;
         cache->next_iv_key = cache_key(false, cluster, block);
         memcpy(cache->next_iv, next_iv, 16);
      }
      else
      {
         const uint8_t *data = cached_raw_block(vol, cache, cluster, block, err);
         if (!data)
            break;
         memcpy(out, data + in_block, n);
      }
      out += n;
      left -= n;
      at += n;
   }
   return size - left;
}

/* A hashed cluster's file: blocks of 0x400 octets of hashes, then 0xFC00
 * of data. The hashes are decrypted with no IV, the data with its block's
 * H0 hash (the block's index modulo 16 of the 16 in the hashes); each H0
 * hashes one block's data, each H1 16 H0 and so on up to the TMD's H4 -
 * of which a read checks the H0. A failure fails all of the read. */
static uint32_t read_hashed(fst_volume_t *vol, block_cache_t *cache, uint32_t cluster,
      uint64_t at, uint32_t size, uint8_t *out, fst_volume_error_t *err)
{
   uint32_t block = (uint32_t)(at / BLOCK_DATA_SIZE);
   uint32_t in_block = (uint32_t)(at % BLOCK_DATA_SIZE);
   uint32_t left = size;
   while (left > 0)
   {
      uint32_t n = BLOCK_DATA_SIZE - in_block;
      if (n > left)
         n = left;
      if (n == BLOCK_DATA_SIZE && size >= 2 * BLOCK_DATA_SIZE
            && !cache_find(cache, cache_key(true, cluster, block)))
      {
         /* all of a block's data, in a read of more than one: decrypted
          * where it goes rather than kept, its hashes aside */
         uint8_t hashes[BLOCK_HASH_SIZE];
         if (!hashed_block(vol, cluster, block, hashes, out, err))
            return 0;
      }
      else
      {
         const uint8_t *data = cached_hashed_data(vol, cache, cluster, block, err);
         if (!data)
            return 0;
         memcpy(out, data + in_block, n);
      }
      out += n;
      left -= n;
      block++;
      in_block = 0;
   }
   return size;
}

uint32_t fst_volume_read(fst_volume_t *vol, uint32_t entry, uint32_t offset,
      uint32_t size, void *out, fst_volume_error_t *err)
{
   fst_volume_error_t ignored;
   fst_table_file_t file;
   block_cache_t *cache;
   uint64_t at;
   uint32_t read = 0;
   uint8_t mode;
   if (!err)
      err = &ignored;
   err->kind = FST_VOLUME_READ_OK;
   if (!fst_table_file(vol->table, entry, &file) || offset >= file.size)
      return 0;
   /* not past the file's end (offset + size may not fit 32 bits) */
   if (size > file.size - offset)
      size = file.size - offset;
   at = (uint64_t)file.offset * fst_table_offset_factor(vol->table) + offset;
   mode = vol->clusters[file.cluster].hash_mode;
   if (mode != HASH_RAW && mode != HASH_RAW_STREAM && mode != HASH_INTERLEAVED)
   {
      fail(vol, err, FST_VOLUME_READ_MODE, file.cluster, 0);
      return 0;
   }
   cache = cache_acquire(vol);
   if (!cache)
   {
      fail(vol, err, FST_VOLUME_READ_MEMORY, file.cluster, 0);
      return 0;
   }
   if (mode == HASH_INTERLEAVED)
      read = read_hashed(vol, cache, file.cluster, at, size, (uint8_t*)out, err);
   else
      read = read_raw(vol, cache, file.cluster, at, size, (uint8_t*)out, err);
   cache_release(vol, cache);
   return read;
}
