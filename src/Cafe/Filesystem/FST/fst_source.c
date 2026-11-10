/* Where an FST volume's encrypted octets come from (fst_source.h) */

#include <stdlib.h>
#include <string.h>

#include "Cafe/Filesystem/WUD/wud.h"
#include "Common/shared_file.h"
#include "util/crypto/aes128.h"
#include "fst_source.h"

struct fst_source
{
   fst_source_ops_t ops;
   void *ctx;
   uint64_t base;
};

/* --- a disc image --------------------------------------------------------- */

static uint64_t fst_disc_read(void *ctx, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, void *dst, uint64_t size)
{
   (void)cluster;
   if (size > 0xFFFFFFFFUL)
      return 0;
   return wud_readData((wud_t*)ctx, dst, (unsigned)size, (int64_t)(cluster_offset + offset));
}

static const uint8_t *fst_disc_lend(void *ctx, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint64_t size)
{
   (void)cluster;
   if (size > 0xFFFFFFFFUL)
      return NULL;
   return wud_lend((const wud_t*)ctx, (unsigned)size, (int64_t)(cluster_offset + offset));
}

static void fst_disc_close(void *ctx)
{
   wud_close((wud_t*)ctx);
}

fst_source_t *fst_source_open_disc(const char *path, unsigned flags)
{
   fst_source_ops_t ops;
   fst_source_t *src;
   wud_t *wud = wud_open(path, (flags & FST_SOURCE_NO_MAP) ? WUD_OPEN_NO_MAP : 0);
   if (!wud)
      return NULL;
   ops.read = fst_disc_read;
   ops.lend = fst_disc_lend;
   ops.close = fst_disc_close;
   src = fst_source_new(&ops, wud);
   if (!src)
      wud_close(wud);
   return src;
}

/* --- a title's content files ---------------------------------------------- */

struct fst_app
{
   shared_file_t **files; /* each cluster's, NULL for one not there */
   size_t count;
};

/* clusterOffset is not used: each file starts at its cluster's start */
static uint64_t fst_app_read(void *ctx, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, void *dst, uint64_t size)
{
   const struct fst_app *app = (const struct fst_app*)ctx;
   int64_t got;
   (void)cluster_offset;
   if (cluster >= app->count || !app->files[cluster] || size > 0xFFFFFFFFUL)
      return 0;
   got = shared_file_read_at(app->files[cluster], dst, (int64_t)size, offset);
   return got > 0 ? (uint64_t)got : 0;
}

static const uint8_t *fst_app_lend(void *ctx, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint64_t size)
{
   const struct fst_app *app = (const struct fst_app*)ctx;
   const uint8_t *mapped;
   uint64_t file_size;
   (void)cluster_offset;
   if (cluster >= app->count || !app->files[cluster])
      return NULL;
   mapped = shared_file_mapped(app->files[cluster]);
   file_size = shared_file_size(app->files[cluster]);
   if (!mapped || offset > file_size || file_size - offset < size)
      return NULL;
   return mapped + offset;
}

static void fst_app_close(void *ctx)
{
   struct fst_app *app = (struct fst_app*)ctx;
   size_t i;
   for (i = 0; i < app->count; i++)
      shared_file_close(app->files[i]);
   free(app->files);
   free(app);
}

fst_source_t *fst_source_open_app(const char *const *paths, const uint16_t *clusters,
      size_t count, unsigned flags)
{
   fst_source_ops_t ops;
   fst_source_t *src;
   struct fst_app *app;
   size_t i, n = 0;
   for (i = 0; i < count; i++)
      if ((size_t)clusters[i] + 1 > n)
         n = (size_t)clusters[i] + 1;
   app = (struct fst_app*)calloc(1, sizeof(*app));
   if (!app)
      return NULL;
   app->count = n;
   app->files = n ? (shared_file_t**)calloc(n, sizeof(*app->files)) : NULL;
   if (n && !app->files)
   {
      free(app);
      return NULL;
   }
   for (i = 0; i < count; i++)
   {
      shared_file_t **file = &app->files[clusters[i]];
      if (*file) /* a cluster twice */
         break;
      *file = shared_file_open(paths[i], (flags & FST_SOURCE_NO_MAP) ? SHARED_FILE_NO_MAP : 0);
      if (!*file)
         break;
   }
   if (i < count)
   {
      fst_app_close(app);
      return NULL;
   }
   ops.read = fst_app_read;
   ops.lend = fst_app_lend;
   ops.close = fst_app_close;
   src = fst_source_new(&ops, app);
   if (!src)
      fst_app_close(app);
   return src;
}

/* --- any source ------------------------------------------------------------ */

fst_source_t *fst_source_new(const fst_source_ops_t *ops, void *ctx)
{
   fst_source_t *src = (fst_source_t*)calloc(1, sizeof(*src));
   if (!src)
      return NULL;
   src->ops = *ops;
   src->ctx = ctx;
   return src;
}

void fst_source_close(fst_source_t *src)
{
   if (!src)
      return;
   if (src->ops.close)
      src->ops.close(src->ctx);
   free(src);
}

void fst_source_set_base_offset(fst_source_t *src, uint64_t base)
{
   src->base = base;
}

uint64_t fst_source_get_base_offset(const fst_source_t *src)
{
   return src->base;
}

uint64_t fst_source_read(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, void *dst, uint64_t size)
{
   return src->ops.read(src->ctx, cluster, cluster_offset, offset + src->base, dst, size);
}

const uint8_t *fst_source_lend(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint64_t size)
{
   if (!src->ops.lend)
      return NULL;
   return src->ops.lend(src->ctx, cluster, cluster_offset, offset + src->base, size);
}

const uint8_t *fst_source_fetch(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint8_t *scratch, uint64_t size)
{
   const uint8_t *lent = fst_source_lend(src, cluster, cluster_offset, offset, size);
   if (lent)
      return lent;
   return fst_source_read(src, cluster, cluster_offset, offset, scratch, size) == size ? scratch : NULL;
}

bool fst_source_read_raw_block(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint8_t *dst, uint32_t size, const uint8_t *key,
      const uint8_t *iv, uint8_t *next_iv)
{
   const uint8_t *in;
   if (size < 16 || (size % 16) != 0)
      return false;
   in = fst_source_fetch(src, cluster, cluster_offset, offset, dst, size);
   if (!in)
      return false;
   /* taken before an in-place decryption overwrites it */
   memcpy(next_iv, in + size - 16, 16);
   AES128_CBC_decrypt(dst, in, size, key, iv);
   return true;
}

bool fst_source_read_hashed_block(fst_source_t *src, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, uint8_t *hash_dst, uint8_t *data_dst, const uint8_t *key,
      uint32_t h0_index)
{
   uint8_t iv[16];
   /* lent in one piece where the source can, each part read where it goes
    * where it cannot */
   const uint8_t *in = fst_source_lend(src, cluster, cluster_offset, offset, FST_HASHED_BLOCK_SIZE);
   const uint8_t *hash_in = in;
   const uint8_t *data_in = in ? in + FST_HASHED_BLOCK_HASH_SIZE : NULL;
   if (h0_index >= 16)
      return false;
   if (!in)
   {
      hash_in = fst_source_fetch(src, cluster, cluster_offset, offset,
            hash_dst, FST_HASHED_BLOCK_HASH_SIZE);
      data_in = fst_source_fetch(src, cluster, cluster_offset, offset + FST_HASHED_BLOCK_HASH_SIZE,
            data_dst, FST_HASHED_BLOCK_SIZE - FST_HASHED_BLOCK_HASH_SIZE);
      if (!hash_in || !data_in)
         return false;
   }
   AES128_CBC_decrypt(hash_dst, hash_in, FST_HASHED_BLOCK_HASH_SIZE, key, NULL);
   /* the IV is the decrypted H0 hash: in hash_dst now, apart from the source */
   memcpy(iv, hash_dst + 20 * h0_index, 16);
   AES128_CBC_decrypt(data_dst, data_in, FST_HASHED_BLOCK_SIZE - FST_HASHED_BLOCK_HASH_SIZE, key, iv);
   return true;
}
