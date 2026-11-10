/* The core's whole-file reader (src/util/helpers/mapped_file.c), under
 * MappedFile.h: files lent from their mapping, or read once where there is
 * none - the test is built both ways (run_tests.sh). C89, as the C89 lane
 * builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "util/helpers/mapped_file.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* whether a file is mapped: with HAVE_MMAP on POSIX hosts, never without
 * it; Windows' own file I/O maps either way */
#if defined(_WIN32)
#define MAPPED_AS(m) true
#elif defined(HAVE_MMAP)
#define MAPPED_AS(m) (m)
#else
#define MAPPED_AS(m) (!(m))
#endif

#define BIG (1024 * 1024 + 17)

static unsigned char octet(size_t i)
{
   return (unsigned char)((i * 2654435761UL) >> 13);
}

static bool write_file(const char *path, const void *data, size_t size)
{
   return filestream_write_file(path, data, (int64_t)size);
}

int main(void)
{
   static const char missing[] = "mapped_file_test_missing.bin";
   static const char empty[] = "mapped_file_test_empty.bin";
   static const char small[] = "mapped_file_test_small.bin";
   static const char big[] = "mapped_file_test_big.bin";
   static const char dir[] = "mapped_file_test_dir";
   mapped_file_t mf;
   unsigned char *want = (unsigned char*)malloc(BIG);
   size_t i;
   bool same;
   bool replaced;

   CHECK(want != NULL);
   if (!want)
      return 1;
   for (i = 0; i < BIG; i++)
      want[i] = octet(i);
   filestream_delete(missing);
   CHECK(write_file(empty, "", 0));
   CHECK(write_file(small, "hello", 5));
   CHECK(write_file(big, want, BIG));
   path_mkdir(dir);

   /* none there, a directory, no path: not opened, and empty */
   memset(&mf, 0x5A, sizeof(mf));
   CHECK(!mapped_file_open(&mf, missing) && !mf.opened && !mf.data && mf.size == 0 && !mf.stream);
   CHECK(!mapped_file_open(&mf, dir) && !mf.opened);
   CHECK(!mapped_file_open(&mf, "") && !mf.opened);
   CHECK(!mapped_file_open(&mf, NULL) && !mf.opened);
   mapped_file_close(&mf);

   /* an empty file is there: opened, with nothing in it */
   CHECK(mapped_file_open(&mf, empty) && mf.opened && mf.size == 0 && !mf.data);
   mapped_file_close(&mf);

   CHECK(mapped_file_open(&mf, small) && mf.opened && mf.size == 5
         && mf.data && memcmp(mf.data, "hello", 5) == 0 && MAPPED_AS(mf.mapped));
   /* read where it is not mapped: nothing of the stream kept */
   CHECK(mf.mapped ? (mf.stream && !mf.copy) : (!mf.stream && mf.data == mf.copy));
   mapped_file_close(&mf);
   CHECK(!mf.opened && !mf.data && !mf.stream && !mf.copy);
   mapped_file_close(&mf); /* again: nothing */

   CHECK(mapped_file_open(&mf, big) && mf.size == BIG && MAPPED_AS(mf.mapped));
   CHECK(mf.data && memcmp(mf.data, want, BIG) == 0);
   /* Another file put in its place under its name: what was opened stays
    * as it was, mapped or read. Windows does not let a file still open be
    * replaced: there the path stays as it was. */
   want[0] ^= 0xFF;
   replaced = filestream_write_file_atomic(big, want, BIG);
   want[0] ^= 0xFF;
#ifndef _WIN32
   CHECK(replaced);
#endif
   same = mf.data && memcmp(mf.data, want, BIG) == 0;
   CHECK(same);
   mapped_file_close(&mf);
   CHECK(mapped_file_open(&mf, big) && mf.size == BIG && mf.data
         && mf.data[0] == (unsigned char)(replaced ? want[0] ^ 0xFF : want[0]));
   mapped_file_close(&mf);

   filestream_delete(empty);
   filestream_delete(small);
   filestream_delete(big);
   path_rmdir(dir);
   free(want);
   if (failures)
   {
      printf("mapped_file_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("mapped_file_test: all passed\n");
   return 0;
}
