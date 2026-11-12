/* The core's cache files (src/Cemu/FileCache/file_cache.c), the shader,
 * pipeline and graphic pack caches' format, from the inside: the index by
 * name against a plain list through random inserts and erases (runs that
 * wrap around its end included), the space between entries against a map
 * of every octet, the free entry the next write takes, the file table's
 * growth and where the file says it is, files opened again and written
 * to, a V2 file's flags, two entries under one name, an entry whose size
 * does not match its stream, the extra version, and compression turned
 * off. C89, as the C89 lane builds it; the file included, so that what it
 * keeps to itself can be looked at. */

#include <stdio.h>

#include "Cemu/FileCache/file_cache.c"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const char *PATH = "file_cache_test.bin";

static uint32_t rng_state = 12345;

static uint32_t rnd(void)
{
   rng_state = rng_state * 1103515245u + 12345u;
   return rng_state >> 8;
}

/* --- the index by name ---------------------------------------------------- */

#define MAX_ENTRIES 600

static void test_index(void)
{
   file_cache_t fc;
   static table_entry_t table[MAX_ENTRIES];
   static int live[MAX_ENTRIES];
   int round, i;
   memset(&fc, 0, sizeof(fc));
   fc.table = table;
   fc.table_count = MAX_ENTRIES;
   memset(table, 0, sizeof(table));
   memset(live, 0, sizeof(live));
   /* names whose homes crowd the last slots of a 64-slot index first, so
    * that runs wrap around its end */
   for (i = 0; i < MAX_ENTRIES; i++)
   {
      uint64_t n2 = (uint64_t)i;
      if (i < 24)
         while ((name_hash(7, n2) & 63) < 56)
            n2 += MAX_ENTRIES;
      table[i].name1 = 7;
      table[i].name2 = n2;
   }
   for (round = 0; round < 40000; round++)
   {
      int pick = (int)(rnd() % (round < 2000 ? 24 : MAX_ENTRIES));
      if (live[pick])
      {
         index_erase(&fc, table[pick].name1, table[pick].name2);
         live[pick] = 0;
      }
      else
      {
         CHECK(index_insert(&fc, pick));
         live[pick] = 1;
      }
      if (round % 97 == 0 || round < 3000)
         for (i = 0; i < MAX_ENTRIES; i++)
            if (find_entry(&fc, table[i].name1, table[i].name2) != (live[i] ? i : -1))
            {
               CHECK(!"index finds every live name and no other");
               round = 1 << 30;
               break;
            }
   }
   {
      size_t used = 0;
      for (i = 0; i < MAX_ENTRIES; i++)
         used += live[i];
      CHECK(fc.index_used == used);
   }
   CHECK(find_entry(&fc, 0, 0) == -1);
   free(fc.index);
}

/* --- the space between entries -------------------------------------------- */

#define SPACE 4096

typedef struct alloc_rec
{
   uint64_t offset;
   uint64_t size;
} alloc_rec_t;

static void test_space(void)
{
   file_cache_t fc;
   static alloc_rec_t recs[400];
   static unsigned char used[SPACE * 4];
   size_t nrec = 0, i, g;
   int round;
   memset(&fc, 0, sizeof(fc));
   memset(used, 0, sizeof(used));
   for (round = 0; round < 20000; round++)
   {
      if (nrec < 400 && (nrec == 0 || rnd() % 3))
      {
         uint64_t size = 1 + rnd() % 64, off, first_fit = fc.data_end, end_at = 0, run = 0;
         /* first fit by the map: the lowest run of free octets that holds it */
         for (off = 0; off < fc.data_end; off++)
         {
            if (used[off])
            {
               run = 0;
               continue;
            }
            if (!run)
               end_at = off;
            if (++run == size)
            {
               first_fit = end_at;
               break;
            }
         }
         off = allocate_space(&fc, size);
         CHECK(off == first_fit);
         if (off != first_fit || off + size > sizeof(used))
            break;
         for (i = 0; i < size; i++)
         {
            CHECK(!used[off + i]);
            used[off + i] = 1;
         }
         recs[nrec].offset = off;
         recs[nrec++].size = size;
      }
      else
      {
         size_t k = rnd() % nrec;
         release_space(&fc, recs[k].offset, recs[k].size);
         for (i = 0; i < recs[k].size; i++)
            used[recs[k].offset + i] = 0;
         recs[k] = recs[--nrec];
      }
      /* the gaps are the free runs below the end, sorted, none touching,
       * none at the end, and the end is the last octet in use */
      {
         uint64_t end = 0, off;
         size_t gi = 0;
         for (off = 0; off < sizeof(used); off++)
            if (used[off])
               end = off + 1;
         CHECK(fc.data_end == end);
         for (off = 0; off < end;)
         {
            uint64_t start;
            if (used[off])
            {
               off++;
               continue;
            }
            start = off;
            while (off < end && !used[off])
               off++;
            CHECK(gi < fc.gap_count && fc.gaps[gi].offset == start && fc.gaps[gi].size == off - start);
            gi++;
         }
         CHECK(gi == fc.gap_count);
         if (gi != fc.gap_count || fc.data_end != end)
            break;
      }
   }
   CHECK(allocate_space(&fc, 0) == fc.data_end);
   for (g = 0; g < fc.gap_count; g++)
      CHECK(fc.gaps[g].size > 0);
   free(fc.gaps);
}

/* --- through the file ------------------------------------------------------ */

typedef struct buf
{
   unsigned char data[70000];
   size_t size;
} buf_t;

static void *to_buf(void *ctx, size_t size)
{
   buf_t *b = (buf_t*)ctx;
   if (size > sizeof(b->data))
      return NULL;
   b->size = size;
   return b->data;
}

static void entry_data(unsigned char *out, size_t size, uint32_t seed)
{
   size_t i;
   for (i = 0; i < size; i++)
      out[i] = (unsigned char)((i * 31 + seed * 7) ^ (i >> 7));
}

static int entry_is(file_cache_t *fc, uint64_t n1, uint64_t n2, size_t size, uint32_t seed)
{
   static buf_t b;
   static unsigned char want[70000];
   if (!file_cache_get(fc, n1, n2, to_buf, &b))
      return 0;
   entry_data(want, size, seed);
   return b.size == size && memcmp(b.data, want, size) == 0;
}

static void add(file_cache_t *fc, uint64_t n1, uint64_t n2, size_t size, uint32_t seed)
{
   static unsigned char d[70000];
   entry_data(d, size, seed);
   file_cache_add(fc, n1, n2, d, size);
}

static size_t file_bytes(unsigned char *out, size_t cap)
{
   FILE *f = fopen(PATH, "rb");
   size_t n;
   if (!f)
      return 0;
   n = fread(out, 1, cap, f);
   fclose(f);
   return n;
}

static void file_patch(long at, const void *data, size_t size)
{
   FILE *f = fopen(PATH, "r+b");
   if (!f)
      return;
   fseek(f, at, SEEK_SET);
   fwrite(data, 1, size, f);
   fclose(f);
}

static void test_file(void)
{
   static unsigned char raw[1 << 20];
   file_cache_t *fc;
   bool corrupt = true;
   int32_t i;
   uint64_t header_table;
   remove(PATH);
   fc = file_cache_create(PATH, 5);
   CHECK(fc != NULL);
   if (!fc)
      return;
   CHECK(fc->table_count == 32 && fc->free_hint == 1);
   for (i = 1; i <= 10; i++)
      add(fc, 100, (uint64_t)i, 100 + i * 10, (uint32_t)i);
   file_cache_wait(fc);
   /* each write takes the lowest free entry */
   CHECK(fc->free_hint == 11);
   CHECK(fc->table[3].name2 == 3);
   CHECK(file_cache_delete(fc, 100, 3));
   CHECK(fc->free_hint == 3);
   add(fc, 100, 9999, 77, 50);
   file_cache_wait(fc);
   CHECK(fc->table[3].name2 == 9999 && fc->free_hint == 4);
   /* past 32 entries: the table grows by 64, then doubles */
   for (i = 11; i <= 40; i++)
      add(fc, 100, (uint64_t)i, 500, (uint32_t)i);
   file_cache_wait(fc);
   CHECK(fc->table_count == 96);
   for (i = 41; i <= 100; i++)
      add(fc, 100, (uint64_t)i, 300, (uint32_t)i);
   file_cache_wait(fc);
   CHECK(fc->table_count == 192);
   /* entry 0 in the file says where the table is, as the header does */
   header_table = fc->table_offset;
   file_cache_close(fc);
   CHECK(file_bytes(raw, sizeof(raw)) > 128);
   {
      uint64_t off;
      uint32_t size;
      memcpy(&off, raw + 128 + header_table + 16, 8);
      memcpy(&size, raw + 128 + header_table + 24, 4);
      CHECK(off == header_table && size == 192 * 32);
   }
   /* opened again: everything there, and new writes clear of the table
    * and of every entry */
   fc = file_cache_open(PATH, false, 5, &corrupt);
   CHECK(fc != NULL && !corrupt);
   if (!fc)
      return;
   CHECK(fc->table[0].offset == fc->table_offset && fc->table[0].size == fc->table_size);
   for (i = 101; i <= 120; i++)
      add(fc, 100, (uint64_t)i, 1000 + (size_t)i, (uint32_t)i);
   file_cache_wait(fc);
   file_cache_close(fc);
   fc = file_cache_open(PATH, false, 5, NULL);
   CHECK(fc != NULL);
   if (!fc)
      return;
   for (i = 1; i <= 120; i++)
   {
      size_t size = i <= 10 ? (size_t)(100 + i * 10) : i <= 40 ? 500 : i <= 100 ? 300 : (size_t)(1000 + i);
      if (i == 3)
         CHECK(!file_cache_has(fc, 100, 3));
      else
         CHECK(entry_is(fc, 100, (uint64_t)i, size, (uint32_t)i));
   }
   CHECK(entry_is(fc, 100, 9999, 77, 50));
   CHECK(file_cache_count(fc) == 120);
   file_cache_close(fc);

   /* another extra version: not opened, and not made anew unless asked */
   CHECK(file_cache_open(PATH, false, 6, &corrupt) == NULL && !corrupt);
   fc = file_cache_open_any(PATH, NULL);
   CHECK(fc != NULL && fc->extra_version == 5);
   file_cache_close(fc);
   remove(PATH);
}

static void test_compression(void)
{
   static unsigned char raw[1 << 20];
   file_cache_t *fc;
   int32_t entry;
   uint32_t size;
   uint64_t offset, data_offset;
   unsigned char wrong[4];
   remove(PATH);
   fc = file_cache_create(PATH, 1);
   if (!fc)
   {
      CHECK(fc != NULL);
      return;
   }
   file_cache_use_compression(fc, false);
   add(fc, 1, 1, 5000, 1);
   file_cache_wait(fc);
   file_cache_use_compression(fc, true);
   add(fc, 1, 2, 5000, 2);
   file_cache_wait(fc);
   entry = find_entry(fc, 1, 1);
   CHECK(entry > 0 && fc->table[entry].flags == 0 && fc->table[entry].size == 5000);
   entry = find_entry(fc, 1, 2);
   CHECK(entry > 0 && fc->table[entry].flags == FLAG_COMPRESSED && fc->table[entry].size < 5000);
   offset = fc->table[entry].offset;
   data_offset = fc->data_offset;
   size = fc->table[entry].size;
   CHECK(entry_is(fc, 1, 1, 5000, 1) && entry_is(fc, 1, 2, 5000, 2));
   file_cache_close(fc);
   CHECK(file_bytes(raw, sizeof(raw)) >= data_offset + offset + size);
   /* the size before the stream, said wrong: no entry, even with room
    * for what the stream holds */
   wrong[0] = 0;
   wrong[1] = 0;
   wrong[2] = 0x13;
   wrong[3] = 0x89; /* 5001 */
   file_patch((long)(data_offset + offset), wrong, 4);
   fc = file_cache_open(PATH, false, 1, NULL);
   CHECK(fc != NULL);
   if (fc)
   {
      static buf_t b;
      CHECK(!file_cache_get(fc, 1, 2, to_buf, &b));
      CHECK(entry_is(fc, 1, 1, 5000, 1));
      file_cache_close(fc);
   }
   remove(PATH);
}

/* a file made by hand: V2, its flags anything, two entries under one name */
static void test_v2(void)
{
   static unsigned char f[128 + 4 * 32 + 300];
   uint32_t u32;
   uint64_t u64;
   file_cache_t *fc;
   FILE *out;
   int e;
   memset(f, 0, sizeof(f));
   u32 = MAGIC_V2;
   memcpy(f, &u32, 4);
   u32 = 9;
   memcpy(f + 4, &u32, 4);
   u64 = 128;
   memcpy(f + 8, &u64, 8);
   u64 = 0;
   memcpy(f + 16, &u64, 8);
   u32 = 4 * 32;
   memcpy(f + 24, &u32, 4);
   for (e = 0; e < 4; e++)
   {
      unsigned char *t = f + 128 + e * 32;
      uint64_t n1 = e == 0 ? TABLE_NAME1 : 77, n2 = e == 0 ? TABLE_NAME2 : 1;
      uint64_t off = e == 0 ? 0 : 4 * 32 + (uint64_t)(e - 1) * 100;
      uint32_t size = e == 0 ? 4 * 32 : 100;
      memcpy(t, &n1, 8);
      memcpy(t + 8, &n2, 8);
      memcpy(t + 16, &off, 8);
      memcpy(t + 24, &size, 4);
      t[28] = 0xFF; /* V2 left these as anything */
      t[29] = t[30] = t[31] = 0xAA;
      if (e)
         memset(f + 128 + off, 'a' + e, 100);
   }
   /* the third entry another name */
   u64 = 2;
   memcpy(f + 128 + 3 * 32 + 8, &u64, 8);
   remove(PATH);
   out = fopen(PATH, "wb");
   if (!out)
   {
      CHECK(out != NULL);
      return;
   }
   fwrite(f, 1, sizeof(f), out);
   fclose(out);
   fc = file_cache_open(PATH, false, 9, NULL);
   CHECK(fc != NULL);
   if (fc)
   {
      static buf_t b;
      /* read as stored, not as compressed */
      CHECK(file_cache_get(fc, 77, 1, to_buf, &b) && b.size == 100 && b.data[0] == 'b');
      CHECK(file_cache_get(fc, 77, 2, to_buf, &b) && b.size == 100 && b.data[0] == 'd');
      CHECK(fc->table[1].flags == 0 && fc->table[2].reserved[0] == 0);
      CHECK(file_cache_count(fc) == 3);
      /* the first of the two is the one written over */
      add(fc, 77, 1, 50, 9);
      file_cache_wait(fc);
      CHECK(fc->table[1].size != 100 && fc->table[2].size == 100);
      CHECK(entry_is(fc, 77, 1, 50, 9));
      file_cache_close(fc);
   }
   /* cut short in its header or table: corrupt */
   {
      bool corrupt = false;
      out = fopen(PATH, "wb");
      fwrite(f, 1, 20, out);
      fclose(out);
      CHECK(file_cache_open(PATH, false, 9, &corrupt) == NULL && corrupt);
      out = fopen(PATH, "wb");
      fwrite(f, 1, 128 + 50, out);
      fclose(out);
      corrupt = false;
      CHECK(file_cache_open(PATH, false, 9, &corrupt) == NULL && corrupt);
      corrupt = true;
      CHECK(file_cache_open(PATH, false, 8, &corrupt) == NULL && !corrupt);
   }
   remove(PATH);
}

int main(void)
{
   if (!file_cache_global_init())
   {
      fprintf(stderr, "file_cache_test: no init\n");
      return 1;
   }
   test_index();
   test_space();
   test_file();
   test_compression();
   test_v2();
   file_cache_stop_writer();
   if (failures)
   {
      fprintf(stderr, "file_cache_test: %d failed\n", failures);
      return 1;
   }
   printf("file_cache_test: all passed\n");
   return 0;
}
