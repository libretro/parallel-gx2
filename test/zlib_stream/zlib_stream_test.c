/* The core's zlib streams (src/util/helpers/zlib_stream.c) under
 * Deflate.h: a stream zlib wrote read back, what is written read back,
 * output too short and streams cut or broken told apart, raw and gzip
 * window bits, Adler-32 and CRC-32, and the pool of inflaters: reused one
 * call after another, and lent to eight threads decoding at once with
 * none lost or shared. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rthreads/rthreads.h>

#include "util/helpers/zlib_stream.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* "The quick brown fox jumps over the lazy dog. " 20 times, by zlib -9 */
static const uint8_t ZLIB_FOX[] = {
   0x78,0xda,0x0b,0xc9,0x48,0x55,0x28,0x2c,0xcd,0x4c,0xce,0x56,0x48,0x2a,0xca,0x2f,
   0xcf,0x53,0x48,0xcb,0xaf,0x50,0xc8,0x2a,0xcd,0x2d,0x28,0x56,0xc8,0x2f,0x4b,0x2d,
   0x52,0x28,0x01,0x4a,0xe7,0x24,0x56,0x55,0x2a,0xa4,0xe4,0xa7,0xeb,0x29,0x84,0x8c,
   0x2a,0x1e,0x55,0x3c,0xaa,0x98,0xda,0x8a,0x01,0x47,0xa5,0x43,0x1c};
#define FOX_LEN 900

static uint8_t fox[FOX_LEN];

static void make_fox(void)
{
   static const char line[] = "The quick brown fox jumps over the lazy dog. ";
   int i;
   for (i = 0; i < 20; i++)
      memcpy(fox + i * 45, line, 45);
}

static void test_streams(void)
{
   uint8_t out[FOX_LEN + 16], z[1200], back[FOX_LEN];
   size_t out_len, in_len, z_len;
   int level;

   /* what zlib wrote */
   out_len = sizeof(out);
   in_len = sizeof(ZLIB_FOX);
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) == ZLIB_OK);
   CHECK(out_len == FOX_LEN && in_len == sizeof(ZLIB_FOX) && memcmp(out, fox, FOX_LEN) == 0);
   /* the stream decoded, not what follows it (which the decoder may
    * read a few octets into) */
   {
      uint8_t more[sizeof(ZLIB_FOX) + 100];
      memcpy(more, ZLIB_FOX, sizeof(ZLIB_FOX));
      memset(more + sizeof(ZLIB_FOX), 0xEE, 100);
      out_len = sizeof(out);
      in_len = sizeof(more);
      CHECK(zlib_uncompress(out, &out_len, more, &in_len, 15) == ZLIB_OK);
      CHECK(in_len >= sizeof(ZLIB_FOX) && in_len < sizeof(ZLIB_FOX) + 16 && out_len == FOX_LEN);
   }
   /* 32+: zlib or gzip, told by the header */
   out_len = sizeof(out);
   in_len = sizeof(ZLIB_FOX);
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 47) == ZLIB_OK && out_len == FOX_LEN);
   /* too little room, a stream cut short, a broken one */
   out_len = 100;
   in_len = sizeof(ZLIB_FOX);
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) == ZLIB_BUFFER_TOO_SMALL);
   CHECK(out_len == 100 && memcmp(out, fox, 100) == 0);
   out_len = sizeof(out);
   in_len = sizeof(ZLIB_FOX) - 3;
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) == ZLIB_DATA_ERROR);
   {
      uint8_t bad[sizeof(ZLIB_FOX)];
      memcpy(bad, ZLIB_FOX, sizeof(bad));
      bad[sizeof(bad) - 1] ^= 1; /* the Adler-32 */
      out_len = sizeof(out);
      in_len = sizeof(bad);
      CHECK(zlib_uncompress(out, &out_len, bad, &in_len, 15) == ZLIB_DATA_ERROR);
      /* broken, with room for exactly what it decodes: not "too small" */
      out_len = FOX_LEN;
      in_len = sizeof(bad);
      CHECK(zlib_uncompress(out, &out_len, bad, &in_len, 15) == ZLIB_DATA_ERROR);
   }
   /* a zlib stream is not a raw one */
   out_len = sizeof(out);
   in_len = sizeof(ZLIB_FOX);
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, -15) == ZLIB_DATA_ERROR);

   /* written and read back, each level; a raw read of the deflate inside */
   for (level = -1; level <= 9; level++)
   {
      z_len = zlib_compress_room(FOX_LEN);
      CHECK(z_len <= sizeof(z));
      CHECK(zlib_compress(z, &z_len, fox, FOX_LEN, level));
      CHECK(z_len <= zlib_compress_bound(FOX_LEN));
      /* -1 is 6, not stored as 0 is */
      CHECK(level == 0 ? z_len > FOX_LEN : z_len < 200);
      CHECK(z[0] == 0x78 && ((z[0] << 8) | z[1]) % 31 == 0);
      out_len = sizeof(back);
      in_len = z_len;
      CHECK(zlib_uncompress(back, &out_len, z, &in_len, 15) == ZLIB_OK);
      CHECK(out_len == FOX_LEN && in_len == z_len && memcmp(back, fox, FOX_LEN) == 0);
      out_len = sizeof(back);
      in_len = z_len - 6;
      CHECK(zlib_uncompress(back, &out_len, z + 2, &in_len, -15) == ZLIB_OK && out_len == FOX_LEN);
   }
   /* no room to write it in */
   z_len = 10;
   CHECK(!zlib_compress(z, &z_len, fox, FOX_LEN, 6) && z_len == 10);
   z_len = sizeof(z);
   CHECK(zlib_compress(z, &z_len, fox, 0, 6) && z_len > 0);

   CHECK(zlib_adler32(1, fox, FOX_LEN) == 0x47A5431CUL);
   CHECK(zlib_adler32(1, (const uint8_t*)"Wikipedia", 9) == 0x11E60398UL);
   CHECK(zlib_crc32(0, fox, FOX_LEN) == 0xB0664AE6UL);
   CHECK(zlib_crc32(0x1234, fox, 0) == 0x1234);
   {
      /* past zlib's NMAX in one go */
      static uint8_t ff[20000];
      uint32_t a, b;
      memset(ff, 0xFF, sizeof(ff));
      a = zlib_adler32(1, ff, sizeof(ff));
      b = zlib_adler32(zlib_adler32(1, ff, 7), ff + 7, sizeof(ff) - 7);
      CHECK(a == b && a == 0x9F51D664UL);
   }
   CHECK(zlib_compress_bound((size_t)1 << 25) == ((size_t)1 << 25) + 8192 + 2048 + 1 + 13);
   CHECK(zlib_compress_room((size_t)1 << 20) == ((size_t)1 << 20) + 512 + 64);
   CHECK(zlib_compress_bound(0) == 13 && zlib_compress_bound(65536) == 65536 + 16 + 4 + 13);
}

static void test_pool_reuse(void)
{
   uint8_t out[FOX_LEN];
   size_t out_len, in_len;
   int i;
   /* one call after another: one inflater, back in the pool each time */
   for (i = 0; i < 10; i++)
   {
      out_len = sizeof(out);
      in_len = sizeof(ZLIB_FOX);
      CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, i & 1 ? 15 : 47) == ZLIB_OK);
      CHECK(zlib_stream_pooled() == 1);
   }
   /* and reset after a stream that failed */
   out_len = sizeof(out);
   in_len = 5;
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) == ZLIB_DATA_ERROR);
   out_len = sizeof(out);
   in_len = sizeof(ZLIB_FOX);
   CHECK(zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) == ZLIB_OK && out_len == FOX_LEN);
   CHECK(zlib_stream_pooled() == 1);
}

#define THREADS 8
#define ROUNDS 400

static int thread_bad[THREADS];

static void decoder(void *arg)
{
   int t = (int)(size_t)arg;
   uint8_t out[FOX_LEN];
   int i;
   for (i = 0; i < ROUNDS; i++)
   {
      size_t out_len = sizeof(out), in_len = sizeof(ZLIB_FOX);
      if (zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) != ZLIB_OK
            || out_len != FOX_LEN || memcmp(out, fox, FOX_LEN) != 0)
         thread_bad[t]++;
      /* every few, one cut short, leaving its inflater mid-stream */
      if (i % 7 == 0)
      {
         out_len = sizeof(out);
         in_len = 20;
         if (zlib_uncompress(out, &out_len, ZLIB_FOX, &in_len, 15) != ZLIB_DATA_ERROR)
            thread_bad[t]++;
      }
   }
}

static void test_threads(void)
{
   sthread_t *threads[THREADS];
   size_t pooled;
   int i;
   for (i = 0; i < THREADS; i++)
      threads[i] = sthread_create(decoder, (void*)(size_t)i);
   for (i = 0; i < THREADS; i++)
   {
      CHECK(threads[i] != NULL);
      if (threads[i])
         sthread_join(threads[i]);
      CHECK(thread_bad[i] == 0);
   }
   /* what was lent came back, as far as the pool holds */
   pooled = zlib_stream_pooled();
   CHECK(pooled >= 1 && pooled <= 4);
}

int main(void)
{
   make_fox();
   test_streams();
   test_pool_reuse();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "zlib_stream_test: %d failed\n", failures);
      return 1;
   }
   printf("zlib_stream_test: all passed\n");
   return 0;
}
