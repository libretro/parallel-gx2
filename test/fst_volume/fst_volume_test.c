/* The core's FST volume reader (src/Cafe/Filesystem/FST/fst_volume.c): a
 * volume made here of raw clusters, read and checked against the content
 * hashes a TMD gives - right, wrong, of a size not a whole number of
 * blocks - from one thread and from many (run_tests.sh, also under TSan).
 * The disc-shaped reads, hashed clusters and the caches are test/fst's.
 * C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lrc_hash.h>
#include <rthreads/rthreads.h>

#include "util/crypto/aes128.h"
#include "Cafe/Filesystem/FST/fst_volume.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define SECTOR    FST_VOLUME_SECTOR_SIZE
#define CLUSTERS  4
/* cluster 1: three whole blocks, SHA-1; cluster 2: one and a half blocks
 * of content in two, SHA-256; cluster 3: a mode of hashing none knows */
#define A_SIZE    (3 * SECTOR)
#define B_CONTENT (SECTOR + SECTOR / 2)
#define B_SIZE    (SECTOR + SECTOR / 4)

static const uint8_t KEY[16] = {
   0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
   0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

typedef struct source
{
   uint8_t *cluster[CLUSTERS];
   size_t size[CLUSTERS];
   int closed;
} source_t;

static uint64_t source_read(void *ctx, uint16_t cluster, uint64_t cluster_offset,
      uint64_t offset, void *dst, uint64_t size)
{
   source_t *s = (source_t*)ctx;
   (void)cluster_offset;
   if (cluster >= CLUSTERS || offset >= s->size[cluster])
      return 0;
   if (size > s->size[cluster] - offset)
      size = s->size[cluster] - offset;
   memcpy(dst, s->cluster[cluster] + offset, (size_t)size);
   return size;
}

static void source_close(void *ctx)
{
   ((source_t*)ctx)->closed++;
}

static fst_source_t *source_new(source_t *s)
{
   fst_source_ops_t ops;
   ops.read = source_read;
   ops.lend = NULL;
   ops.close = source_close;
   return fst_source_new(&ops, s);
}

static void be32(uint8_t *p, unsigned long v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static uint8_t plain(unsigned cluster, size_t at)
{
   unsigned long x = (unsigned long)(at * 2654435761UL) ^ (cluster * 40503UL);
   return (uint8_t)(x >> 11);
}

/* a raw cluster: one CBC stream, its IV the cluster's index */
static void encrypt_cluster(source_t *s, unsigned cluster, size_t size)
{
   uint8_t iv[16];
   uint8_t *p = (uint8_t*)malloc(size);
   size_t i;
   for (i = 0; i < size; i++)
      p[i] = plain(cluster, i);
   memset(iv, 0, sizeof(iv));
   iv[0] = (uint8_t)(cluster >> 8);
   iv[1] = (uint8_t)cluster;
   s->cluster[cluster] = (uint8_t*)malloc(size);
   s->size[cluster] = size;
   AES128_CBC_encrypt(s->cluster[cluster], p, (uint32_t)size, KEY, iv);
   free(p);
}

/* The FST, in cluster 0: the root and files a (cluster 1), b (2), c (3) */
static uint32_t make_fst(source_t *s)
{
   uint8_t f[0x200];
   uint8_t iv[16];
   uint32_t size = 0x20 + 0x20 * CLUSTERS + 0x10 * 4 + 8;
   unsigned i;
   uint8_t *e;
   memset(f, 0, sizeof(f));
   be32(f, 0x46535400UL);
   be32(f + 4, 0x20);
   be32(f + 8, CLUSTERS);
   for (i = 0; i < CLUSTERS; i++)
      f[0x20 + 0x20 * i + 0x14] = (uint8_t)(i == 3 ? 7 : (i == 2 ? 1 : 0));
   e = f + 0x20 + 0x20 * CLUSTERS;
   e[0] = 1; be32(e + 8, 4);                                   /* root */
   be32(e + 0x10, 1); be32(e + 0x18, A_SIZE); e[0x1F] = 1;      /* a */
   be32(e + 0x20, 3); be32(e + 0x28, B_SIZE); e[0x2F] = 2;      /* b */
   be32(e + 0x30, 5); be32(e + 0x38, 0x100); e[0x3F] = 3;       /* c */
   memcpy(e + 0x40, "\0a\0b\0c\0", 7);
   size = (uint32_t)(e + 0x47 - f);
   memset(iv, 0, sizeof(iv));
   s->size[0] = (size + 15) & ~15u;
   s->cluster[0] = (uint8_t*)malloc(s->size[0]);
   AES128_CBC_encrypt(s->cluster[0], f, (uint32_t)s->size[0], KEY, iv);
   return size;
}

static void make_source(source_t *s)
{
   memset(s, 0, sizeof(*s));
   encrypt_cluster(s, 1, A_SIZE);
   encrypt_cluster(s, 2, 2 * SECTOR); /* padded to whole blocks */
   encrypt_cluster(s, 3, SECTOR);
}

static void free_source(source_t *s)
{
   unsigned i;
   for (i = 0; i < CLUSTERS; i++)
      free(s->cluster[i]);
}

/* what the TMD says: right, or with one octet of a hash wrong */
static void make_contents(fst_volume_content_t *c, int wrong_cluster)
{
   uint8_t *p = (uint8_t*)malloc(A_SIZE);
   size_t i;
   struct sha256_state st;
   memset(c, 0, sizeof(*c) * CLUSTERS);
   for (i = 0; i < A_SIZE; i++)
      p[i] = plain(1, i);
   c[1].size = A_SIZE;
   c[1].sha1 = true;
   SHA1Digest(p, A_SIZE, c[1].hash);
   /* past the 20 octets of a SHA-1: not compared */
   memset(c[1].hash + 20, 0xEE, 12);
   for (i = 0; i < B_CONTENT; i++)
      p[i] = plain(2, i);
   c[2].size = B_CONTENT;
   sha256_stream_init(&st, 0);
   sha256_stream_update(&st, p, B_CONTENT);
   sha256_stream_final(&st, c[2].hash);
   if (wrong_cluster)
      c[wrong_cluster].hash[3] ^= 0x40;
   free(p);
}

static fst_volume_t *open_volume(source_t *s, uint32_t fst_size, const fst_volume_content_t *c)
{
   int why = -1;
   fst_volume_t *vol = fst_volume_open(source_new(s), true, 0, fst_size, KEY, c,
         c ? CLUSTERS : 0, &why);
   CHECK(vol != NULL && why == FST_VOLUME_OPEN_OK);
   return vol;
}

static bool matches(unsigned cluster, size_t at, const uint8_t *got, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
   {
      if (got[i] != plain(cluster, at + i))
         return false;
   }
   return true;
}

/* all of a file, in order, chunk octets at a time, to the first failure:
 * err what it was */
static uint32_t read_all(fst_volume_t *vol, uint32_t entry, unsigned cluster, uint32_t size,
      uint32_t chunk, fst_volume_error_t *err, bool *right)
{
   uint8_t *buf = (uint8_t*)malloc(size);
   uint32_t at = 0;
   *right = true;
   while (at < size)
   {
      uint32_t n = fst_volume_read(vol, entry, at, chunk, buf + at, err);
      at += n;
      if (n == 0 || err->kind != FST_VOLUME_READ_OK)
         break;
   }
   *right = matches(cluster, 0, buf, at);
   free(buf);
   return at;
}

static void test_hashes(void)
{
   source_t s;
   fst_volume_content_t c[CLUSTERS];
   uint32_t fst_size;
   fst_volume_t *vol;
   fst_volume_error_t err;
   bool right;
   unsigned wrong;

   /* right: read in order a block at a time, through the cache, and all at
    * once, straight where it goes */
   for (wrong = 0; wrong <= 2; wrong++)
   {
      uint32_t chunk;
      for (chunk = 0x1000; chunk <= A_SIZE; chunk = chunk == 0x1000 ? A_SIZE : A_SIZE + 1)
      {
         uint32_t got;
         make_source(&s);
         fst_size = make_fst(&s);
         make_contents(c, (int)wrong);
         vol = open_volume(&s, fst_size, c);
         if (!vol)
         {
            free_source(&s);
            continue;
         }
         got = read_all(vol, 1, 1, A_SIZE, chunk, &err, &right);
         CHECK(right);
         if (wrong == 1)
         {
            /* all but the last block, which leaves the hash not the TMD's */
            CHECK(got < A_SIZE && got >= 2 * SECTOR);
            CHECK(err.kind == FST_VOLUME_READ_RAW_HASH && err.cluster == 1 && err.block == 2);
            CHECK(fst_volume_corrupted(vol));
         }
         else
         {
            CHECK(got == A_SIZE && err.kind == FST_VOLUME_READ_OK);
            CHECK(!fst_volume_corrupted(vol));
         }
         /* b: its content ends half way through its second block */
         got = read_all(vol, 2, 2, B_SIZE, chunk == A_SIZE ? B_SIZE : chunk, &err, &right);
         CHECK(right);
         if (wrong == 2)
            CHECK(got < B_SIZE && err.kind == FST_VOLUME_READ_RAW_HASH && err.block == 1
                  && fst_volume_corrupted(vol));
         else if (wrong == 0)
            CHECK(got == B_SIZE && err.kind == FST_VOLUME_READ_OK && !fst_volume_corrupted(vol));
         fst_volume_free(vol);
         CHECK(s.closed == 1);
         free_source(&s);
      }
   }
}

static void test_reads(void)
{
   source_t s;
   fst_volume_content_t c[CLUSTERS];
   uint32_t fst_size;
   fst_volume_t *vol;
   fst_volume_error_t err;
   uint8_t buf[0x200];
   int why;

   make_source(&s);
   fst_size = make_fst(&s);
   vol = open_volume(&s, fst_size, NULL);
   if (vol)
   {
      CHECK(fst_table_entry_count(fst_volume_table(vol)) == 4);
      /* in the middle of a block, across two, past the end, at the end */
      CHECK(fst_volume_read(vol, 1, 0x7F80, 0x100, buf, &err) == 0x100 && matches(1, 0x7F80, buf, 0x100));
      CHECK(fst_volume_read(vol, 1, A_SIZE - 0x10, 0x100, buf, &err) == 0x10 && matches(1, A_SIZE - 0x10, buf, 0x10));
      CHECK(fst_volume_read(vol, 1, A_SIZE, 0x100, buf, &err) == 0);
      /* offset and size past 32 bits together: to the file's end */
      {
         uint8_t *all = (uint8_t*)malloc(A_SIZE);
         CHECK(all && fst_volume_read(vol, 1, 0x10, 0xFFFFFFF0UL, all, NULL) == A_SIZE - 0x10
               && matches(1, 0x10, all, A_SIZE - 0x10));
         free(all);
      }
      /* not a file; a mode of hashing none knows: nothing, and not taken
       * for what is there being wrong */
      CHECK(fst_volume_read(vol, 0, 0, 0x10, buf, &err) == 0);
      CHECK(fst_volume_read(vol, 3, 0, 0x10, buf, &err) == 0 && err.kind == FST_VOLUME_READ_MODE);
      CHECK(!fst_volume_corrupted(vol));
      fst_volume_free(vol);
   }
   CHECK(s.closed == 1);

   /* not one content for each cluster; an FST that is not one; one not
    * there: not opened, and the source it was given closed */
   make_contents(c, 0);
   s.closed = 0;
   why = -1;
   CHECK(fst_volume_open(source_new(&s), true, 0, fst_size, KEY, c, CLUSTERS - 1, &why) == NULL
         && why == FST_VOLUME_OPEN_CONTENTS && s.closed == 1);
   why = -1;
   {
      uint8_t wrong_key[16];
      memcpy(wrong_key, KEY, 16);
      wrong_key[5] ^= 1;
      CHECK(fst_volume_open(source_new(&s), true, 0, fst_size, wrong_key, NULL, 0, &why) == NULL
            && why == FST_VOLUME_OPEN_INVALID && s.closed == 2);
   }
   why = -1;
   CHECK(fst_volume_open(source_new(&s), true, 0x100000, fst_size, KEY, NULL, 0, &why) == NULL
         && why == FST_VOLUME_OPEN_READ && s.closed == 3);
   {
      fst_source_t *kept = source_new(&s);
      CHECK(fst_volume_open(kept, false, 0, 0x10, KEY, NULL, 0, NULL) == NULL && s.closed == 3);
      fst_source_close(kept);
   }
   free_source(&s);
}

/* Many readers at once, each its own order: each gets the file's octets,
 * and the running hash, advanced by whichever claims each block, comes out
 * the TMD's when they get to the end - or is let be */
#define THREADS 8
typedef struct reader
{
   fst_volume_t *vol;
   unsigned seed;
   bool right;
} reader_t;

static void reader_run(void *arg)
{
   reader_t *r = (reader_t*)arg;
   uint8_t *buf = (uint8_t*)malloc(SECTOR);
   unsigned round;
   r->right = buf != NULL;
   for (round = 0; buf && round < 200; round++)
   {
      uint32_t at, n;
      fst_volume_error_t err;
      r->seed = r->seed * 1103515245u + 12345u;
      at = (r->seed >> 8) % A_SIZE;
      n = 1 + (r->seed >> 3) % SECTOR;
      if (round % 3 == 0)
      {
         /* in order from the start: what advances the hash */
         at = (round / 3 % 3) * SECTOR;
         n = SECTOR;
      }
      n = fst_volume_read(r->vol, 1, at, n, buf, &err);
      r->right = r->right && err.kind == FST_VOLUME_READ_OK && matches(1, at, buf, n);
   }
   free(buf);
}

static void test_threads(void)
{
   source_t s;
   fst_volume_content_t c[CLUSTERS];
   uint32_t fst_size;
   fst_volume_t *vol;
   reader_t r[THREADS];
   sthread_t *t[THREADS];
   unsigned i;
   bool all = true;
   make_source(&s);
   fst_size = make_fst(&s);
   make_contents(c, 0);
   vol = open_volume(&s, fst_size, c);
   if (!vol)
   {
      free_source(&s);
      return;
   }
   for (i = 0; i < THREADS; i++)
   {
      r[i].vol = vol;
      r[i].seed = 77u + i * 1013u;
      t[i] = sthread_create(reader_run, &r[i]);
      CHECK(t[i] != NULL);
   }
   for (i = 0; i < THREADS; i++)
   {
      if (t[i])
         sthread_join(t[i]);
      all = all && r[i].right;
   }
   CHECK(all);
   CHECK(!fst_volume_corrupted(vol));
   fst_volume_free(vol);
   free_source(&s);
}

int main(void)
{
   test_hashes();
   test_reads();
   test_threads();
   if (failures)
   {
      printf("fst_volume_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("fst_volume_test: all passed\n");
   return 0;
}
