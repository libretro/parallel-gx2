/* The core's FST table (src/Cafe/Filesystem/FST/fst_table.c): tables made
 * here, parsed, looked up, listed and named; ones made wrong turned away;
 * and ones made wrong at random either turned away or walked to the end
 * (run_tests.sh). C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Cafe/Filesystem/FST/fst_table.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* ------------------------------------------------------------------ */
/* making a table */

#define IMAGE_MAX 4096

typedef struct image
{
   unsigned char b[IMAGE_MAX];
   unsigned clusters;
   unsigned entries;   /* made so far */
   unsigned names_at;  /* where the name table starts */
   unsigned names;     /* its size so far */
} image_t;

static void put32(unsigned char *p, unsigned long v)
{
   p[0] = (unsigned char)(v >> 24);
   p[1] = (unsigned char)(v >> 16);
   p[2] = (unsigned char)(v >> 8);
   p[3] = (unsigned char)v;
}

/* room for entries entries after clusters clusters */
static void image_begin(image_t *im, unsigned clusters, unsigned entries)
{
   unsigned i;
   memset(im, 0, sizeof(*im));
   put32(im->b, 0x46535400UL);
   put32(im->b + 4, 0x20);
   put32(im->b + 8, clusters);
   for (i = 0; i < clusters; i++)
   {
      put32(im->b + 0x20 + 0x20 * i, 100 + i);
      put32(im->b + 0x20 + 0x20 * i + 4, 7);
      im->b[0x20 + 0x20 * i + 0x14] = (unsigned char)(i % 3);
   }
   im->clusters = clusters;
   im->names_at = 0x20 + 0x20 * clusters + 0x10 * entries;
   /* the root's name, "" */
   im->b[im->names_at] = 0;
   im->names = 1;
}

static unsigned image_name(image_t *im, const char *name)
{
   unsigned at = im->names;
   size_t n = strlen(name) + 1;
   memcpy(im->b + im->names_at + at, name, n);
   im->names += (unsigned)n;
   return at;
}

/* type: 0 a file, 1 a directory; | 0x80 for a link */
static unsigned image_entry(image_t *im, unsigned type, unsigned name_at,
      unsigned long a, unsigned long b, unsigned cluster)
{
   unsigned char *e = im->b + 0x20 + 0x20 * im->clusters + 0x10 * im->entries;
   put32(e, ((unsigned long)type << 24) | name_at);
   put32(e + 4, a);
   put32(e + 8, b);
   e[14] = (unsigned char)(cluster >> 8);
   e[15] = (unsigned char)cluster;
   return im->entries++;
}

static unsigned image_size(const image_t *im)
{
   return im->names_at + im->names;
}

/* The tree the tests look at:
 *   0  /             (root, ends 13)
 *   1  Content/      (ends 9)
 *   2    a.bin       cluster 1
 *   3    Sub/        (ends 6)
 *   4      deep.txt  cluster 2
 *   5      a.bin     cluster 2   (the same name as 2, one deeper)
 *   6    b.BIN       cluster 1
 *   7    links/      (ends 9)
 *   8      linked.bin  cluster 1, a link
 *   9  code/         (ends 11)
 *  10    app.rpx     cluster 0
 *  11  top.txt       cluster 0
 *  12  pb            cluster 0   (its hash that of "p")
 * a.bin (2) says 6 where a directory would say its end: a lookup under it
 * would find Sub. */
static void make_tree(image_t *im)
{
   image_begin(im, 3, 13);
   image_entry(im, 1, 0, 0, 13, 0);
   image_entry(im, 1, image_name(im, "Content"), 0, 9, 0);
   image_entry(im, 0, image_name(im, "a.bin"), 6, 1000, 1);
   image_entry(im, 1, image_name(im, "Sub"), 1, 6, 0);
   image_entry(im, 0, image_name(im, "deep.txt"), 0x20, 2000, 2);
   image_entry(im, 0, image_name(im, "a.bin"), 0x30, 3000, 2);
   image_entry(im, 0, image_name(im, "b.BIN"), 0x40, 4000, 1);
   image_entry(im, 1, image_name(im, "links"), 1, 9, 0);
   image_entry(im, 0x80, image_name(im, "linked.bin"), 0x50, 5000, 1);
   image_entry(im, 1, image_name(im, "code"), 0, 11, 0);
   image_entry(im, 0, image_name(im, "app.rpx"), 0x60, 6000, 0);
   image_entry(im, 0, image_name(im, "top.txt"), 0x70, 7000, 0);
   image_entry(im, 0, image_name(im, "pb"), 0x80, 8000, 0);
}

static uint32_t look(const fst_table_t *t, const char *path, bool only_files)
{
   return fst_table_lookup(t, path, strlen(path), only_files);
}

/* ------------------------------------------------------------------ */

static void test_parse(void)
{
   image_t im;
   fst_table_t *t;
   const fst_table_cluster_t *c;
   make_tree(&im);
   im.b[12] = 1; /* hashes disabled */
   t = fst_table_parse(im.b, image_size(&im));
   CHECK(t != NULL);
   if (!t)
      return;
   CHECK(fst_table_offset_factor(t) == 0x20);
   CHECK(fst_table_hash_disabled(t));
   CHECK(fst_table_cluster_count(t) == 3);
   CHECK(fst_table_entry_count(t) == 13);
   CHECK(fst_table_file_count(t) == 8);
   c = fst_table_cluster(t, 2);
   CHECK(c && c->offset == 102 && c->size == 7 && c->hash_mode == 2);
   CHECK(fst_table_cluster(t, 3) == NULL);
   fst_table_free(t);
}

static void test_lookup(void)
{
   image_t im;
   fst_table_t *t;
   fst_table_file_t f;
   make_tree(&im);
   t = fst_table_parse(im.b, image_size(&im));
   CHECK(t != NULL);
   if (!t)
      return;
   CHECK(look(t, "content/a.bin", true) == 2);
   CHECK(look(t, "CONTENT/SUB/A.BIN", true) == 5);
   CHECK(look(t, "/content\\sub//deep.txt", true) == 4);
   CHECK(look(t, "content/./sub/./deep.txt/", true) == 4);
   CHECK(look(t, "content/b.bin", true) == 6);
   CHECK(look(t, "top.txt", true) == 11);
   CHECK(look(t, "code/app.rpx", false) == 10);
   /* a name in a directory under it is not in it */
   CHECK(look(t, "deep.txt", false) == FST_TABLE_NONE);
   CHECK(look(t, "content/deep.txt", false) == FST_TABLE_NONE);
   CHECK(look(t, "app.rpx", false) == FST_TABLE_NONE);
   /* directories, and only files */
   CHECK(look(t, "content/sub", false) == 3);
   CHECK(look(t, "content/sub/", true) == FST_TABLE_NONE);
   CHECK(look(t, "", false) == 0);
   CHECK(look(t, "/", false) == 0);
   CHECK(look(t, "", true) == FST_TABLE_NONE);
   /* a name under a file; a longer or shorter name */
   CHECK(look(t, "top.txt/x", false) == FST_TABLE_NONE);
   CHECK(look(t, "content/a.bin/sub", false) == FST_TABLE_NONE);
   CHECK(look(t, "pb", true) == 12);
   CHECK(look(t, "p", false) == FST_TABLE_NONE);
   CHECK(look(t, "PB/", false) == 12);
   CHECK(look(t, "content/a.bi", false) == FST_TABLE_NONE);
   CHECK(look(t, "content/a.binx", false) == FST_TABLE_NONE);
   CHECK(look(t, "conten", false) == FST_TABLE_NONE);
   /* only len octets of the path */
   CHECK(fst_table_lookup(t, "top.txtJUNK", 7, true) == 11);
   CHECK(fst_table_lookup(t, "content/a.bin", 7, false) == 1);
   /* what is said of an entry */
   CHECK(fst_table_is_dir(t, 3) && !fst_table_is_file(t, 3));
   CHECK(fst_table_is_file(t, 4) && !fst_table_is_dir(t, 4));
   CHECK(fst_table_is_link(t, 8) && !fst_table_is_link(t, 6));
   CHECK(!fst_table_is_dir(t, 13) && !fst_table_is_file(t, 13) && !fst_table_is_link(t, 13));
   CHECK(strcmp(fst_table_name(t, 6), "b.BIN") == 0);
   CHECK(strcmp(fst_table_name(t, 13), "") == 0);
   CHECK(fst_table_file(t, 5, &f) && f.offset == 0x30 && f.size == 3000 && f.cluster == 2);
   CHECK(!fst_table_file(t, 3, &f));
   CHECK(!fst_table_file(t, 99, &f));
   fst_table_free(t);
}

static void test_path(void)
{
   image_t im;
   fst_table_t *t;
   char buf[64];
   make_tree(&im);
   t = fst_table_parse(im.b, image_size(&im));
   CHECK(t != NULL);
   if (!t)
      return;
   CHECK(fst_table_path(t, 4, buf, sizeof(buf)) == 20 && strcmp(buf, "Content/Sub/deep.txt") == 0);
   CHECK(fst_table_path(t, 11, buf, sizeof(buf)) == 7 && strcmp(buf, "top.txt") == 0);
   CHECK(fst_table_path(t, 3, buf, sizeof(buf)) == 11 && strcmp(buf, "Content/Sub") == 0);
   CHECK(fst_table_path(t, 0, buf, sizeof(buf)) == 0 && buf[0] == '\0');
   /* one octet short of its terminator: "" and its length */
   memset(buf, 'x', sizeof(buf));
   CHECK(fst_table_path(t, 4, buf, 20) == 20 && buf[0] == '\0');
   CHECK(fst_table_path(t, 4, buf, 21) == 20 && strcmp(buf, "Content/Sub/deep.txt") == 0);
   CHECK(fst_table_path(t, 4, NULL, 0) == 20);
   CHECK(fst_table_path(t, 99, buf, sizeof(buf)) == 0 && buf[0] == '\0');
   /* each path looks up its own entry */
   {
      uint32_t i;
      bool all = true;
      for (i = 1; i < fst_table_entry_count(t); i++)
      {
         fst_table_path(t, i, buf, sizeof(buf));
         all = all && look(t, buf, false) == i;
      }
      CHECK(all);
   }
   fst_table_free(t);
}

static void test_list(void)
{
   image_t im;
   fst_table_t *t;
   fst_table_dir_t it;
   static const uint32_t root[] = {1, 9, 11, 12};
   static const uint32_t content[] = {2, 3, 6, 7};
   uint32_t got[16];
   unsigned n;
   make_tree(&im);
   t = fst_table_parse(im.b, image_size(&im));
   CHECK(t != NULL);
   if (!t)
      return;
   CHECK(fst_table_dir_open(t, 0, &it));
   for (n = 0; n < 16 && (got[n] = fst_table_dir_next(t, &it)) != FST_TABLE_NONE; n++);
   CHECK(n == 4 && memcmp(got, root, sizeof(root)) == 0);
   CHECK(fst_table_dir_next(t, &it) == FST_TABLE_NONE);
   CHECK(fst_table_dir_open(t, 1, &it));
   for (n = 0; n < 16 && (got[n] = fst_table_dir_next(t, &it)) != FST_TABLE_NONE; n++);
   CHECK(n == 4 && memcmp(got, content, sizeof(content)) == 0);
   CHECK(fst_table_dir_open(t, 7, &it));
   CHECK(fst_table_dir_next(t, &it) == 8);
   CHECK(fst_table_dir_next(t, &it) == FST_TABLE_NONE);
   CHECK(!fst_table_dir_open(t, 2, &it));
   CHECK(!fst_table_dir_open(t, 99, &it));
   fst_table_free(t);
}

/* tables made wrong are turned away */
static void test_invalid(void)
{
   image_t im;
   unsigned size;
   fst_table_t *t;

   make_tree(&im);
   size = image_size(&im);
   CHECK(fst_table_parse(NULL, size) == NULL);
   CHECK(fst_table_parse(im.b, 0x1F) == NULL);
   /* cut short: in the clusters, the entries, at the name table */
   CHECK(fst_table_parse(im.b, 0x40) == NULL);
   CHECK(fst_table_parse(im.b, 0x20 + 0x60 + 0x50) == NULL);
   CHECK(fst_table_parse(im.b, im.names_at) == NULL);
   /* a name cut short: no terminator */
   CHECK(fst_table_parse(im.b, size - 1) == NULL);

   make_tree(&im);
   im.b[0] = 'X';
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);

   /* as many clusters as there may be, and one more */
   {
      unsigned long clusters;
      for (clusters = 0xFFF; clusters <= 0x1000; clusters++)
      {
         size_t big = 0x20 + 0x20 * clusters + 0x10 + 1;
         unsigned char *b = (unsigned char*)calloc(1, big);
         CHECK(b != NULL);
         if (!b)
            break;
         put32(b, 0x46535400UL);
         put32(b + 8, clusters);
         b[0x20 + 0x20 * clusters] = 1; /* the root, its name "" */
         put32(b + 0x20 + 0x20 * clusters + 8, 1);
         t = fst_table_parse(b, (uint32_t)big);
         CHECK((t != NULL) == (clusters < 0x1000));
         CHECK(!t || fst_table_cluster_count(t) == clusters);
         fst_table_free(t);
         free(b);
      }
   }

   /* the root: a file; naming another parent; no entries; more than fit */
   make_tree(&im);
   im.b[0x80] = 0;
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);
   make_tree(&im);
   put32(im.b + 0x80 + 8, 0x10000);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);
   make_tree(&im);
   put32(im.b + 0x80 + 4, 1);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);
   make_tree(&im);
   put32(im.b + 0x80 + 8, 0);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);

   /* a name past the name table */
   make_tree(&im);
   put32(im.b + 0x80 + 0x10 * 2, im.names);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);

   /* a file in a cluster there is not */
   make_tree(&im);
   im.b[0x80 + 0x10 * 2 + 15] = 3;
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);

   /* Sub (3): ending before itself, at itself (a listing of it would go
    * round for ever), past Content; naming the root as its parent */
   make_tree(&im);
   put32(im.b + 0x80 + 0x10 * 3 + 8, 2);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);
   make_tree(&im);
   put32(im.b + 0x80 + 0x10 * 3 + 8, 3);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);
   make_tree(&im);
   put32(im.b + 0x80 + 0x10 * 3 + 8, 10);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);
   make_tree(&im);
   put32(im.b + 0x80 + 0x10 * 3 + 4, 0);
   CHECK(fst_table_parse(im.b, image_size(&im)) == NULL);

   /* and one made right parses still */
   make_tree(&im);
   t = fst_table_parse(im.b, image_size(&im));
   CHECK(t != NULL);
   fst_table_free(t);
}

/* Made wrong at random: turned away, or every directory listed to its end,
 * every entry named and looked up by its path - in steps that are counted,
 * so a listing or lookup that goes round for ever fails rather than hangs. */
static unsigned long rng_state = 12345;
static unsigned long rng(void)
{
   rng_state = (rng_state * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
   return rng_state >> 8;
}

static void test_random(void)
{
   image_t base, im;
   unsigned size, round;
   unsigned parsed = 0;
   bool all = true;
   char buf[256];
   make_tree(&base);
   size = image_size(&base);
   for (round = 0; round < 20000; round++)
   {
      fst_table_t *t;
      unsigned flips = 1 + (unsigned)(rng() % 4), k;
      uint32_t i, n;
      im = base;
      for (k = 0; k < flips; k++)
      {
         /* most in the entries, where the checks are */
         unsigned at = rng() % 2 ? 0x80 + (unsigned)(rng() % (13 * 0x10)) : (unsigned)(rng() % size);
         im.b[at] ^= (unsigned char)(1u << (rng() % 8));
      }
      t = fst_table_parse(im.b, size - (unsigned)(rng() % 3 == 0 ? rng() % 8 : 0));
      if (!t)
         continue;
      parsed++;
      n = fst_table_entry_count(t);
      for (i = 0; i < n; i++)
      {
         fst_table_dir_t it;
         fst_table_file_t f;
         uint32_t steps = 0;
         if (fst_table_dir_open(t, i, &it))
         {
            while (fst_table_dir_next(t, &it) != FST_TABLE_NONE)
            {
               if (++steps > n)
               {
                  all = false;
                  break;
               }
            }
         }
         if (fst_table_file(t, i, &f))
            all = all && f.cluster < fst_table_cluster_count(t);
         if (fst_table_path(t, i, buf, sizeof(buf)) < sizeof(buf))
         {
            /* a name may hold a '/', so its path need not look it up */
            uint32_t found = look(t, buf, false);
            all = all && (found == FST_TABLE_NONE || found < n);
         }
      }
      fst_table_free(t);
   }
   CHECK(all);
   /* the flips leave some tables whole */
   CHECK(parsed > 100);
}

int main(void)
{
   test_parse();
   test_lookup();
   test_path();
   test_list();
   test_invalid();
   test_random();
   if (failures)
   {
      printf("fst_table_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("fst_table_test: all passed\n");
   return 0;
}
