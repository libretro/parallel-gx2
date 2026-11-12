/* Miiverse's offline posts (src/Cafe/OS/libs/nn_olv/olv_offline.c), against
 * an archive libretro-common's writer makes: a post's file names from an id
 * that may fill its field, sizes, reads that fit and ones that do not (once
 * copied whole past the end of the game's buffer), and memos made into the
 * TGA the post carries - the same zlib stream as compressing the image the
 * slow way, with every bit of the bitmap where it belongs. C89, as the C89
 * lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/zar.h>
#include <streams/file_stream.h>

#include "util/helpers/zlib_stream.h"
#include "Cafe/OS/libs/nn_olv/olv_offline.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define ZAR_PATH "olv_offline_test.zar"

static uint8_t s_bits[OLV_OFFLINE_MEMO_BITS_SIZE];
static uint8_t s_pattern[OLV_OFFLINE_MEMO_BITS_SIZE];
static uint8_t s_jpeg[5000];

static int write_to(void *ud, const void *data, size_t len)
{
   return fwrite(data, 1, len, (FILE*)ud) == len;
}

static void add(zar_writer_t *w, const char *path, const void *data, size_t len)
{
   CHECK(zar_writer_file(w, path) == ZAR_OK);
   CHECK(zar_writer_append(w, data, len) == ZAR_OK);
}

static void make_archive(void)
{
   size_t i;
   FILE *f = fopen(ZAR_PATH, "wb");
   zar_writer_t *w;
   CHECK(f != NULL);
   w = zar_writer_new(write_to, f, 3);
   CHECK(w != NULL);
   /* every other pixel, then runs: each bit read where it is */
   for (i = 0; i < sizeof(s_bits); i++)
      s_bits[i] = (uint8_t)(i * 37 + (i >> 3));
   memset(s_pattern, 0, sizeof(s_pattern));
   s_pattern[0] = 0x01;                        /* the first pixel */
   s_pattern[sizeof(s_pattern) - 1] = 0x80;    /* and the last */
   for (i = 0; i < sizeof(s_jpeg); i++)
      s_jpeg[i] = (uint8_t)(i ^ 0x5A);
   CHECK(zar_writer_dir(w, "memo") == ZAR_OK);
   CHECK(zar_writer_dir(w, "image") == ZAR_OK);
   add(w, "memo/AYMHAAABAAD4UV4Fa7QmFQ", s_bits, sizeof(s_bits));
   add(w, "memo/edges", s_pattern, sizeof(s_pattern));
   add(w, "memo/short", s_bits, 100);
   add(w, "memo/long", s_jpeg, sizeof(s_bits) + 1);
   add(w, "image/AYMHAAABAAD4UV4Fa7QmFQ.jpg", s_jpeg, sizeof(s_jpeg));
   add(w, "PostList_WindWakerHD.xml", "<posts></posts>", 15);
   CHECK(zar_writer_finish(w) == ZAR_OK);
   zar_writer_free(w);
   fclose(f);
}

static void test_name(void)
{
   char out[64];
   uint8_t full[32];
   /* an id that fills its 32 octets has no NUL: 32 of it, no more */
   memset(full, 'A', sizeof(full));
   CHECK(olv_offline_name(out, sizeof(out), "image/", full, sizeof(full), ".jpg"));
   CHECK(strlen(out) == 6 + 32 + 4 && strncmp(out + 38, ".jpg", 4) == 0);
   CHECK(olv_offline_name(out, sizeof(out), "memo/", (const uint8_t*)"abc\0zzz", 7, ""));
   CHECK(strcmp(out, "memo/abc") == 0);
   /* what does not fit is refused, not cut */
   CHECK(!olv_offline_name(out, 42, "image/", full, sizeof(full), ".jpg"));
   CHECK(olv_offline_name(out, 43, "image/", full, sizeof(full), ".jpg"));
}

static void test_read(const zarchive_t *ar)
{
   static uint8_t buf[8192];
   uint32_t size = 0xFFFF;
   CHECK(olv_offline_size(ar, "image/AYMHAAABAAD4UV4Fa7QmFQ.jpg", &size) && size == sizeof(s_jpeg));
   CHECK(!olv_offline_size(ar, "image/none.jpg", &size));
   CHECK(!olv_offline_size(ar, "memo", &size));
   CHECK(!olv_offline_size(NULL, "memo/edges", &size));
   CHECK(olv_offline_size(ar, "memo/edges", NULL));

   memset(buf, 0xEE, sizeof(buf));
   CHECK(olv_offline_read(ar, "image/AYMHAAABAAD4UV4Fa7QmFQ.jpg", buf, sizeof(s_jpeg), &size) == OLV_OFFLINE_OK);
   CHECK(size == sizeof(s_jpeg) && memcmp(buf, s_jpeg, sizeof(s_jpeg)) == 0);
   CHECK(buf[sizeof(s_jpeg)] == 0xEE);
   /* a byte short of room: refused, nothing written */
   memset(buf, 0xEE, sizeof(buf));
   CHECK(olv_offline_read(ar, "image/AYMHAAABAAD4UV4Fa7QmFQ.jpg", buf, sizeof(s_jpeg) - 1, &size) == OLV_OFFLINE_TOO_LARGE);
   CHECK(size == 0 && buf[0] == 0xEE && buf[sizeof(s_jpeg) - 2] == 0xEE);
   CHECK(olv_offline_read(ar, "image/none.jpg", buf, sizeof(buf), &size) == OLV_OFFLINE_MISSING);
   CHECK(olv_offline_read(ar, "image", buf, sizeof(buf), &size) == OLV_OFFLINE_MISSING);
   CHECK(olv_offline_read(NULL, "memo/edges", buf, sizeof(buf), &size) == OLV_OFFLINE_MISSING);
}

/* the image the memo is, made the plain way */
static uint8_t *expected_tga(const uint8_t *bits)
{
   static const uint8_t header[18] = {0,0,2,0,0,0,0,0,0,0,0,0,0x40,0x01,0x78,0,0x20,0x08};
   uint8_t *tga = (uint8_t*)malloc(OLV_OFFLINE_MEMO_TGA_SIZE);
   size_t i;
   memcpy(tga, header, sizeof(header));
   for (i = 0; i < 320 * 120; i++)
   {
      const int white = (bits[i / 8] & (1 << (i % 8))) != 0;
      uint8_t *p = tga + 18 + i * 4;
      p[0] = p[1] = p[2] = white ? 0xFF : 0;
      p[3] = 0xFF;
   }
   return tga;
}

static void check_memo(const zarchive_t *ar, const char *name, const uint8_t *bits)
{
   static uint8_t out[40960];
   static uint8_t want[40960];
   uint8_t *tga = expected_tga(bits);
   uint8_t *back = (uint8_t*)malloc(OLV_OFFLINE_MEMO_TGA_SIZE);
   uint32_t size = 0;
   size_t want_len = sizeof(want);
   size_t back_len = OLV_OFFLINE_MEMO_TGA_SIZE;
   size_t src_len;
   CHECK(olv_offline_memo(ar, name, out, sizeof(out), &size));
   /* the stream it was before: the same image, the same level */
   CHECK(zlib_compress(want, &want_len, tga, OLV_OFFLINE_MEMO_TGA_SIZE, 6));
   CHECK(size == want_len && memcmp(out, want, want_len) == 0);
   src_len = size;
   CHECK(zlib_uncompress(back, &back_len, out, &src_len, 15) == ZLIB_OK);
   CHECK(back_len == OLV_OFFLINE_MEMO_TGA_SIZE && memcmp(back, tga, back_len) == 0);
   free(back);
   free(tga);
}

static void test_memo(const zarchive_t *ar)
{
   static uint8_t out[40960];
   uint32_t size = 0;
   check_memo(ar, "memo/AYMHAAABAAD4UV4Fa7QmFQ", s_bits);
   check_memo(ar, "memo/edges", s_pattern);
   /* not a memo's size; none at all; no room */
   CHECK(!olv_offline_memo(ar, "memo/short", out, sizeof(out), &size));
   CHECK(!olv_offline_memo(ar, "memo/long", out, sizeof(out), &size));
   CHECK(!olv_offline_memo(ar, "memo/none", out, sizeof(out), &size));
   CHECK(!olv_offline_memo(ar, "memo/AYMHAAABAAD4UV4Fa7QmFQ", out, 64, &size));
   CHECK(!olv_offline_memo(NULL, "memo/edges", out, sizeof(out), &size));
}

/* posts that can be taken: those whose index is not a multiple of skip */
struct picker
{
   uint32_t skip;
   uint32_t tries[64];
   uint32_t slots[64];
};

static bool take(void *ud, uint32_t index, uint32_t slot)
{
   struct picker *p = (struct picker*)ud;
   p->tries[index]++;
   if (p->skip && index % p->skip == 0)
      return false;
   p->slots[slot] = index;
   return true;
}

static void test_pick(void)
{
   uint32_t pool[64];
   struct picker p;
   uint32_t i, j, n;
   /* all taken, each once, at most max */
   memset(&p, 0, sizeof(p));
   n = olv_offline_pick(20, 50, 12345, pool, take, &p);
   CHECK(n == 20);
   for (i = 0; i < 20; i++)
      CHECK(p.tries[i] == 1);
   for (i = 0; i < n; i++)
      for (j = i + 1; j < n; j++)
         CHECK(p.slots[i] != p.slots[j]);
   memset(&p, 0, sizeof(p));
   CHECK(olv_offline_pick(20, 5, 7, pool, take, &p) == 5);
   /* none can be taken: every one tried once, then it ends */
   memset(&p, 0, sizeof(p));
   p.skip = 1;
   CHECK(olv_offline_pick(30, 10, 99, pool, take, &p) == 0);
   for (i = 0; i < 30; i++)
      CHECK(p.tries[i] == 1);
   /* some cannot: the rest fill the slots in order */
   memset(&p, 0, sizeof(p));
   p.skip = 3; /* 0, 3, 6, 9: 4 of 10 */
   CHECK(olv_offline_pick(10, 10, 5, pool, take, &p) == 6);
   for (i = 0; i < 6; i++)
      CHECK(p.slots[i] % 3 != 0);
   for (i = 0; i < 10; i++)
      CHECK(p.tries[i] == 1);
   /* the order follows the seed, and is not the posts' own */
   memset(&p, 0, sizeof(p));
   olv_offline_pick(40, 40, 1, pool, take, &p);
   for (i = 0, j = 0, n = 0; i < 40; i++)
   {
      if (p.slots[i] != i)
         j++;
      if (p.slots[i] != 39 - i)
         n++;
   }
   CHECK(j > 20 && n > 20);
   CHECK(olv_offline_pick(0, 10, 1, pool, take, &p) == 0);
   memset(&p, 0, sizeof(p));
   CHECK(olv_offline_pick(10, 0, 1, pool, take, &p) == 0);
   CHECK(p.tries[0] == 0);
}

int main(void)
{
   zarchive_t *ar;
   test_pick();
   make_archive();
   ar = zarchive_open(ZAR_PATH, 0, NULL, 0);
   CHECK(ar != NULL);
   if (ar)
   {
      test_name();
      test_read(ar);
      test_memo(ar);
      zarchive_close(ar);
   }
   /* and read through streams, not a mapping */
   ar = zarchive_open(ZAR_PATH, ZARCHIVE_OPEN_NO_MAP, NULL, 0);
   CHECK(ar != NULL);
   if (ar)
   {
      test_read(ar);
      test_memo(ar);
      zarchive_close(ar);
   }
   remove(ZAR_PATH);
   if (failures)
   {
      fprintf(stderr, "olv_offline_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("olv_offline_test: all passed\n");
   return 0;
}
