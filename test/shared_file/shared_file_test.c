/* The core's file read from many threads at once (src/Common/shared_file.c),
 * in C89 as it is:
 *  - mapped, the file's octets are lent whole, a read copies from them, and
 *    no stream is opened for it, however many reads;
 *  - not mapped (asked not to be), a read goes through the stream the file
 *    was opened with, given back after: one thread reading a hundred times
 *    opens no other;
 *  - reads at the end are short, past it nothing, a negative length an
 *    error; an empty file opens, maps nothing and reads nothing; a missing
 *    file and a directory do not open;
 *  - eight threads reading at random places at once read right, mapped and
 *    not; not mapped, no more streams are opened than threads, and the
 *    file keeps a few of them for the next reads rather than closing them
 *    (run under ThreadSanitizer and AddressSanitizer by CI). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rthreads/rthreads.h>
#include <streams/file_stream.h>

#include "Common/shared_file.h"

static int failures = 0;
#define CHECK(cond) \
   do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define FILE_SIZE (3 * 65536 + 123)
#define THREADS 8
#define READS 400

static unsigned char content[FILE_SIZE];

/* a small generator of its own, the same everywhere */
static uint32_t next_random(uint32_t *state)
{
   *state = *state * 1664525u + 1013904223u;
   return *state >> 8;
}

static void write_file(const char *path, const void *data, size_t len)
{
   FILE *f = fopen(path, "wb");
   CHECK(f != NULL);
   if (!f)
      return;
   CHECK(fwrite(data, 1, len, f) == len);
   fclose(f);
}

struct reader
{
   shared_file_t *file;
   uint32_t seed;
   int bad;
};

static void read_at_random(void *data)
{
   struct reader *r = (struct reader*)data;
   unsigned char *got = (unsigned char*)malloc(20000);
   int i;
   if (!got)
   {
      r->bad++;
      return;
   }
   for (i = 0; i < READS; i++)
   {
      uint32_t at = next_random(&r->seed) % FILE_SIZE;
      int64_t len = 1 + next_random(&r->seed) % 20000;
      int64_t want = len < FILE_SIZE - (int64_t)at ? len : FILE_SIZE - (int64_t)at;
      if (     shared_file_read_at(r->file, got, len, at) != want
            || memcmp(got, content + at, (size_t)want) != 0)
         r->bad++;
   }
   free(got);
}

static void test_threads(const char *path, unsigned flags)
{
   struct reader readers[THREADS];
   sthread_t *threads[THREADS];
   shared_file_t *file = shared_file_open(path, flags);
   int i, bad = 0;
   CHECK(file != NULL);
   if (!file)
      return;
   for (i = 0; i < THREADS; i++)
   {
      readers[i].file = file;
      readers[i].seed = 7 + (uint32_t)i;
      readers[i].bad = 0;
      threads[i] = sthread_create(read_at_random, &readers[i]);
      CHECK(threads[i] != NULL);
   }
   for (i = 0; i < THREADS; i++)
   {
      if (threads[i])
         sthread_join(threads[i]);
      bad += readers[i].bad;
   }
   CHECK(bad == 0);
   if (flags & SHARED_FILE_NO_MAP)
   {
      unsigned opened = shared_file_streams_opened(file);
      unsigned char c;
      int j;
      CHECK(opened >= 1 && opened <= THREADS);
      /* the ones kept serve what comes next */
      for (j = 0; j < 50; j++)
         CHECK(shared_file_read_at(file, &c, 1, (uint64_t)j * 1000) == 1 && c == content[j * 1000]);
      CHECK(shared_file_streams_opened(file) == opened);
   }
   else
      CHECK(shared_file_streams_opened(file) == 0);
   shared_file_close(file);
}

int main(void)
{
   char path[1024];
   char empty_path[1024];
   const char *tmp = getenv("TMPDIR");
   uint32_t state = 99;
   unsigned char got[300];
   unsigned mode;
   size_t i;

   if (!tmp || !*tmp)
      tmp = getenv("TEMP");
   if (!tmp || !*tmp)
      tmp = "/tmp";
   sprintf(path, "%s/parallel_gx2_shared_file_test.bin", tmp);
   sprintf(empty_path, "%s/parallel_gx2_shared_file_empty.bin", tmp);
   for (i = 0; i < FILE_SIZE; i++)
      content[i] = (unsigned char)next_random(&state);
   write_file(path, content, FILE_SIZE);
   write_file(empty_path, content, 0);

   for (mode = 0; mode < 2; mode++)
   {
      const unsigned flags = mode ? SHARED_FILE_NO_MAP : 0;
      shared_file_t *file = shared_file_open(path, flags);
      CHECK(file != NULL);
      if (file)
      {
         const uint8_t *mapped = shared_file_mapped(file);
         int j;
         CHECK(shared_file_size(file) == FILE_SIZE);
         if (mode)
            CHECK(mapped == NULL);
         else
            CHECK(mapped != NULL && memcmp(mapped, content, FILE_SIZE) == 0);
         for (j = 0; j < 100; j++)
         {
            uint32_t at = next_random(&state) % (FILE_SIZE - 300);
            CHECK(shared_file_read_at(file, got, 300, at) == 300 && memcmp(got, content + at, 300) == 0);
         }
         /* one reader: the stream it was opened with, again and again */
         CHECK(shared_file_streams_opened(file) == (mode ? 1u : 0u));
         /* short at the end, nothing past it, an error for no length */
         CHECK(shared_file_read_at(file, got, 300, FILE_SIZE - 10) == 10);
         CHECK(memcmp(got, content + FILE_SIZE - 10, 10) == 0);
         CHECK(shared_file_read_at(file, got, 1, FILE_SIZE) == 0);
         CHECK(shared_file_read_at(file, got, 1, (uint64_t)1 << 40) == 0);
         CHECK(shared_file_read_at(file, got, 0, 0) == 0);
         CHECK(shared_file_read_at(file, got, -1, 0) == -1);
         shared_file_close(file);
      }

      file = shared_file_open(empty_path, flags);
      CHECK(file != NULL);
      if (file)
      {
         CHECK(shared_file_size(file) == 0 && shared_file_mapped(file) == NULL);
         CHECK(shared_file_read_at(file, got, 1, 0) == 0);
         CHECK(shared_file_streams_opened(file) == 0);
         shared_file_close(file);
      }
      CHECK(shared_file_open(tmp, flags) == NULL);
      CHECK(shared_file_open(NULL, flags) == NULL);
      {
         char missing[1100];
         sprintf(missing, "%s/parallel_gx2_shared_file_none", tmp);
         CHECK(shared_file_open(missing, flags) == NULL);
      }

      test_threads(path, flags);
   }
   shared_file_close(NULL);

   remove(path);
   remove(empty_path);
   if (failures)
   {
      fprintf(stderr, "shared_file: %d check(s) failed\n", failures);
      return 1;
   }
   printf("shared_file: all checks passed\n");
   return 0;
}
