/* The core's host paths (src/Common/host_path.c): a path resolved without
 * regard to case where it does not exist as given - its last component, the
 * ones above, ones that match nothing, a relative one and one at the root -
 * and a tree removed with what a link in it points at left alone. C89, as
 * the C89 lane builds it. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "Common/host_path.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int resolves_to(const char *path, const char *want)
{
   char *got = host_path_resolve(path);
   int ok = got && strcmp(got, want) == 0;
   if (!ok)
      fprintf(stderr, "  %s -> %s, not %s\n", path, got ? got : "(null)", want);
   free(got);
   return ok;
}

#define BASE "host_path_test_dir"

static void make_tree(void)
{
   host_remove_all(BASE);
   CHECK(path_mkdir(BASE "/Dir/Sub"));
   CHECK(path_mkdir(BASE "/Dir/Other/Deep"));
   CHECK(filestream_write_file(BASE "/Dir/Sub/File.txt", "abc", 3));
   CHECK(filestream_write_file(BASE "/Dir/.Hidden", "h", 1));
   CHECK(filestream_write_file(BASE "/Dir/Other/Deep/x.bin", "x", 1));
   CHECK(filestream_write_file(BASE "/Top.BIN", "t", 1));
}

static void test_resolve(void)
{
   make_tree();
   /* there as given: as given */
   CHECK(resolves_to(BASE "/Dir/Sub/File.txt", BASE "/Dir/Sub/File.txt"));
   CHECK(resolves_to(BASE "/Dir", BASE "/Dir"));
#ifdef _WIN32
   /* the file system does not care: as given */
   CHECK(resolves_to(BASE "/dir/sub/FILE.TXT", BASE "/dir/sub/FILE.TXT"));
#else
   CHECK(resolves_to(BASE "/Dir/Sub/file.TXT", BASE "/Dir/Sub/File.txt"));
   CHECK(resolves_to(BASE "/dir/SUB/FILE.txt", BASE "/Dir/Sub/File.txt"));
   CHECK(resolves_to(BASE "/DIR/other/deep/X.BIN", BASE "/Dir/Other/Deep/x.bin"));
   CHECK(resolves_to(BASE "/dir/.hidden", BASE "/Dir/.Hidden"));
   /* what matches nothing is kept as given, and so is what is below it */
   CHECK(resolves_to(BASE "/dir/Nope/file.txt", BASE "/Dir/Nope/file.txt"));
   CHECK(resolves_to(BASE "/dir/sub/missing.txt", BASE "/Dir/Sub/missing.txt"));
   /* relative to here, with no directory: found here */
   CHECK(filestream_write_file("Host_Path_Here.TXT", "1", 1));
   CHECK(resolves_to("host_path_here.txt", "./Host_Path_Here.TXT"));
   CHECK(resolves_to("host_path_nowhere.txt", "./host_path_nowhere.txt"));
   filestream_delete("Host_Path_Here.TXT");
   /* at the root */
   CHECK(resolves_to("/host_path_test_no_such_thing", "/host_path_test_no_such_thing"));
   /* two entries a case apart: each there as given is itself */
   CHECK(filestream_write_file(BASE "/Dir/Twin", "1", 1));
   CHECK(filestream_write_file(BASE "/Dir/TWIN", "2", 1));
   CHECK(resolves_to(BASE "/Dir/Twin", BASE "/Dir/Twin"));
   CHECK(resolves_to(BASE "/Dir/TWIN", BASE "/Dir/TWIN"));
   /* a name that is a prefix of an entry's is not that entry */
   CHECK(resolves_to(BASE "/top", BASE "/top"));
   CHECK(resolves_to(BASE "/top.bi", BASE "/top.bi"));
   CHECK(resolves_to(BASE "/top.bin", BASE "/Top.BIN"));
#endif
}

static void test_remove_all(void)
{
#ifndef _WIN32
   const char *outside = "host_path_test_outside";
   host_remove_all(outside);
   CHECK(path_mkdir(outside));
   CHECK(filestream_write_file("host_path_test_outside/keep.txt", "k", 1));
#endif
   make_tree();
#ifndef _WIN32
   CHECK(symlink("../../host_path_test_outside", BASE "/Dir/link") == 0);
#endif
   CHECK(host_remove_all(BASE));
   CHECK(!path_is_valid(BASE));
   /* nothing there: nothing left */
   CHECK(host_remove_all(BASE));
   /* a file alone */
   CHECK(filestream_write_file("host_path_test_file", "f", 1));
   CHECK(host_remove_all("host_path_test_file") && !path_is_valid("host_path_test_file"));
#ifndef _WIN32
   CHECK(path_is_valid("host_path_test_outside/keep.txt"));
   host_remove_all(outside);
   CHECK(!path_is_valid(outside));
#endif
}

int main(void)
{
   test_resolve();
   test_remove_all();
   if (failures)
   {
      fprintf(stderr, "host_path_test: %d failed\n", failures);
      return 1;
   }
   printf("host_path_test: all passed\n");
   return 0;
}
