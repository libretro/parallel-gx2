/* The core's host folder files (src/Cafe/Filesystem/host_fs.c) under the
 * virtual file system's host device: what each open makes of a file that is
 * there or not, a path in the wrong case (a file's, not a directory's), a
 * directory not taken for a file, reads stopping at the end, writes growing
 * the file, the length set, a directory listed with sizes and names cut to
 * fit, and one that cannot be listed. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "Common/host_path.h"
#include "Cafe/Filesystem/host_fs.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define DIR_ "host_fs_test_dir"

static int64_t size_on_disk(const char *path)
{
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   int64_t s;
   if (!f)
      return -1;
   s = filestream_get_size(f);
   filestream_close(f);
   return s;
}

static void test_open(void)
{
   host_fs_node_t *n;
   bool failed = true;
   char buf[16];
   host_remove_all(DIR_);
   CHECK(path_mkdir(DIR_ "/Sub"));
   CHECK(filestream_write_file(DIR_ "/Data.bin", "0123456789", 10));

   /* there: read, and read only unless asked */
   n = host_fs_open(DIR_ "/Data.bin", HOST_FS_OPEN_FILE, &failed);
   CHECK(n && !failed && !host_fs_is_dir(n) && !host_fs_writable(n) && host_fs_size(n) == 10);
   CHECK(n && host_fs_write(n, "x", 1) == 0);
   host_fs_close(n);
#ifndef _WIN32
   /* a file's path in the wrong case is found; a directory's is not */
   n = host_fs_open(DIR_ "/data.BIN", HOST_FS_OPEN_FILE, NULL);
   CHECK(n && host_fs_size(n) == 10);
   host_fs_close(n);
   CHECK(host_fs_open(DIR_ "/sub", HOST_FS_OPEN_DIR, NULL) == NULL);
#endif
   /* not there: nothing, unless made */
   CHECK(host_fs_open(DIR_ "/none.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE, &failed) == NULL && !failed);
   n = host_fs_open(DIR_ "/new.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE | HOST_FS_ALLOW_CREATE, &failed);
   CHECK(n && !failed && host_fs_size(n) == 0 && host_fs_writable(n));
   host_fs_close(n);
   CHECK(path_is_valid(DIR_ "/new.bin"));
   /* allowed to make one, it keeps one that is there */
   n = host_fs_open(DIR_ "/Data.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE | HOST_FS_ALLOW_CREATE, NULL);
   CHECK(n && host_fs_size(n) == 10);
   host_fs_close(n);
   /* made anew: emptied */
   n = host_fs_open(DIR_ "/Data.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE | HOST_FS_ALWAYS_CREATE, NULL);
   CHECK(n && host_fs_size(n) == 0);
   host_fs_close(n);
   CHECK(size_on_disk(DIR_ "/Data.bin") == 0);
   /* where none can be made */
   CHECK(host_fs_open(DIR_ "/no/such/dir/x.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE | HOST_FS_ALLOW_CREATE, &failed) == NULL && failed);
   CHECK(host_fs_open(DIR_ "/no/such/dir/x.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE | HOST_FS_ALWAYS_CREATE, &failed) == NULL && failed);
   /* a directory is not a file: only opened as one when asked */
   CHECK(host_fs_open(DIR_ "/Sub", HOST_FS_OPEN_FILE, NULL) == NULL);
   n = host_fs_open(DIR_ "/Sub", HOST_FS_OPEN_FILE | HOST_FS_OPEN_DIR, NULL);
   CHECK(n && host_fs_is_dir(n) && host_fs_size(n) == 0 && host_fs_tell(n) == 0);
   CHECK(n && host_fs_read(n, buf, sizeof(buf)) == 0 && host_fs_write(n, "x", 1) == 0);
   CHECK(n && !host_fs_set_length(n, 0));
   host_fs_close(n);
   CHECK(host_fs_open(DIR_ "/new.bin", HOST_FS_OPEN_DIR, NULL) == NULL);
   host_fs_close(NULL);
}

static void test_read_write(void)
{
   host_fs_node_t *n;
   char buf[32];
   CHECK(filestream_write_file(DIR_ "/rw.bin", "abcdefghij", 10));
   n = host_fs_open(DIR_ "/rw.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE, NULL);
   CHECK(n != NULL);
   if (!n)
      return;
   /* no further than the end */
   CHECK(host_fs_read(n, buf, 4) == 4 && !memcmp(buf, "abcd", 4) && host_fs_tell(n) == 4);
   CHECK(host_fs_read(n, buf, sizeof(buf)) == 6 && !memcmp(buf, "efghij", 6) && host_fs_tell(n) == 10);
   CHECK(host_fs_read(n, buf, sizeof(buf)) == 0);
   host_fs_seek(n, 0);
   CHECK(host_fs_read(n, buf, 0x80000000UL) == 0 && host_fs_tell(n) == 0);
   host_fs_seek(n, 10);
   /* written past the end: grown */
   host_fs_seek(n, 8);
   CHECK(host_fs_write(n, "XYZW", 4) == 4 && host_fs_tell(n) == 12 && host_fs_size(n) == 12);
   CHECK(host_fs_write(n, buf, 0x80000000UL) == 0);
   host_fs_flush(n);
   CHECK(size_on_disk(DIR_ "/rw.bin") == 12);
   host_fs_seek(n, 0);
   CHECK(host_fs_read(n, buf, sizeof(buf)) == 12 && !memcmp(buf, "abcdefghXYZW", 12));
   /* cut short: the position no further, and the size where it is */
   CHECK(host_fs_set_length(n, 5));
   CHECK(host_fs_tell(n) == 5 && host_fs_size(n) == 5);
   CHECK(host_fs_read(n, buf, sizeof(buf)) == 0);
   host_fs_seek(n, 2);
   CHECK(host_fs_set_length(n, 9));
   CHECK(host_fs_tell(n) == 2 && host_fs_size(n) == 2);
   CHECK(host_fs_read(n, buf, sizeof(buf)) == 0);
   host_fs_close(n);
   CHECK(size_on_disk(DIR_ "/rw.bin") == 9);
   /* the next write goes where the position was left, not past it */
   n = host_fs_open(DIR_ "/rw.bin", HOST_FS_OPEN_FILE | HOST_FS_WRITE, NULL);
   CHECK(n != NULL);
   if (!n)
      return;
   host_fs_seek(n, 9);
   CHECK(host_fs_set_length(n, 4) && host_fs_tell(n) == 4);
   CHECK(host_fs_write(n, "Q", 1) == 1);
   host_fs_close(n);
   CHECK(size_on_disk(DIR_ "/rw.bin") == 5);
}

static void test_dir(void)
{
   host_fs_node_t *n;
   char name[8];
   bool is_dir, failed;
   uint64_t size;
   int files = 0, dirs = 0, seen_long = 0, seen_dot = 0;
   host_remove_all(DIR_);
   CHECK(path_mkdir(DIR_ "/D/Inner"));
   CHECK(filestream_write_file(DIR_ "/D/a.bin", "12345", 5));
   CHECK(filestream_write_file(DIR_ "/D/.hidden", "1", 1));
   CHECK(filestream_write_file(DIR_ "/D/a_rather_long_name.bin", "", 0));
   n = host_fs_open(DIR_ "/D", HOST_FS_OPEN_DIR, NULL);
   CHECK(n != NULL);
   if (!n)
      return;
   while (host_fs_dir_next(n, name, sizeof(name), &is_dir, &size, &failed))
   {
      CHECK(!failed && strlen(name) < sizeof(name));
      CHECK(strcmp(name, ".") && strcmp(name, ".."));
      if (is_dir)
      {
         dirs++;
         CHECK(!strcmp(name, "Inner") && size == 0);
      }
      else
      {
         files++;
         if (!strcmp(name, "a.bin"))
            CHECK(size == 5);
         if (!strcmp(name, "a_rathe"))
            seen_long = 1;
         if (!strcmp(name, ".hidden"))
            seen_dot = 1;
      }
   }
   CHECK(!failed && dirs == 1 && files == 3 && seen_long && seen_dot);
   CHECK(!host_fs_dir_next(n, name, sizeof(name), &is_dir, &size, &failed));
   host_fs_close(n);

   /* gone before it is listed: none, and said so */
   n = host_fs_open(DIR_ "/D/Inner", HOST_FS_OPEN_DIR, NULL);
   CHECK(n != NULL);
   host_remove_all(DIR_ "/D/Inner");
   if (n)
   {
      CHECK(!host_fs_dir_next(n, name, sizeof(name), &is_dir, &size, &failed) && failed);
      host_fs_close(n);
   }
   /* a file has no entries */
   n = host_fs_open(DIR_ "/D/a.bin", HOST_FS_OPEN_FILE, NULL);
   CHECK(n && !host_fs_dir_next(n, name, sizeof(name), &is_dir, &size, &failed) && !failed);
   host_fs_close(n);
   host_remove_all(DIR_);
}

int main(void)
{
   test_open();
   test_read_write();
   test_dir();
   if (failures)
   {
      fprintf(stderr, "host_fs_test: %d failed\n", failures);
      return 1;
   }
   printf("host_fs_test: all passed\n");
   return 0;
}
