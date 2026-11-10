/* Wii U archives (zarchive.h) */

#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>

#include "Common/shared_file.h"
#include "util/helpers/rcu_cell.h"
#include "zarchive.h"

/* readers kept for lookups, listings and reads: one for each thread that
 * uses an archive at once, as many as the core has */
#define ZARCHIVE_READER_SLOTS 8
/* the footer: what an archive's last 144 octets say */
#define ZARCHIVE_FOOTER_SIZE 144

struct zarchive
{
   shared_file_t *file;
   const uint8_t *mapped; /* size octets, where mapped */
   uint64_t size;
   uint64_t index_offset;
   uint8_t *index; /* unmapped only: from index_offset to the end */
   char *path;
#ifdef RETRO_ATOMIC_HAS_PTR
   retro_atomic_ptr_t readers[ZARCHIVE_READER_SLOTS];
#endif
   retro_atomic_int_t readers_made;
   /* while shared: its holders; 0 once the last has given it back, after
    * which it is taken by none and leaves the table */
   retro_atomic_int_t holders;
};

/* --- what a reader asks for ------------------------------------------------ */

/* From the mapping, from the index in memory, or read */
static size_t zarchive_supply(const zarchive_t *ar, const zar_request_t *req)
{
   size_t length = req->length;
   int64_t got;
   if (req->offset >= ar->size)
      return 0;
   if ((uint64_t)length > ar->size - req->offset)
      length = (size_t)(ar->size - req->offset);
   if (ar->mapped)
   {
      /* one copy, from the pages to where the reader wants them */
      memcpy(req->dst, ar->mapped + req->offset, length);
      return length;
   }
   if (ar->index && req->offset >= ar->index_offset)
   {
      memcpy(req->dst, ar->index + (req->offset - ar->index_offset), length);
      return length;
   }
   /* a short supply is asked for again: the index part comes from memory */
   if (ar->index && (uint64_t)length > ar->index_offset - req->offset)
      length = (size_t)(ar->index_offset - req->offset);
   if (length > 0x40000000)
      length = 0x40000000;
   got = shared_file_read_at(ar->file, req->dst, (int64_t)length, req->offset);
   return got > 0 ? (size_t)got : 0;
}

/* --- readers --------------------------------------------------------------- */

static zar_t *zarchive_new_reader(const zarchive_t *ar)
{
   zar_request_t req;
   int err;
   zar_t *z = zar_new(ar->size);
   if (!z)
      return NULL;
   retro_atomic_fetch_add_int((retro_atomic_int_t*)&ar->readers_made, 1);
   while ((err = zar_open_step(z, &req)) == ZAR_PENDING)
      zar_feed(z, zarchive_supply(ar, &req));
   if (err != ZAR_OK)
   {
      zar_free(z);
      return NULL;
   }
   return z;
}

/* A free one from the pool, or a new one */
static zar_t *zarchive_acquire(const zarchive_t *ar)
{
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
   zarchive_t *a = (zarchive_t*)ar;
   for (i = 0; i < ZARCHIVE_READER_SLOTS; i++)
   {
      zar_t *z;
      if (!retro_atomic_load_relaxed_ptr(&a->readers[i]))
         continue;
      z = (zar_t*)retro_atomic_exchange_ptr(&a->readers[i], NULL);
      if (z)
         return z;
   }
#endif
   return zarchive_new_reader(ar);
}

/* Back to the pool, or freed when it is full */
static void zarchive_release(const zarchive_t *ar, zar_t *z)
{
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
   zarchive_t *a = (zarchive_t*)ar;
#endif
   if (!z)
      return;
#ifdef RETRO_ATOMIC_HAS_PTR
   for (i = 0; i < ZARCHIVE_READER_SLOTS; i++)
   {
      if (     !retro_atomic_load_relaxed_ptr(&a->readers[i])
            && retro_atomic_cas_ptr(&a->readers[i], NULL, z))
         return;
   }
#else
   (void)ar;
#endif
   zar_free(z);
}

/* --- opening ---------------------------------------------------------------- */

static void zarchive_append(char *out, size_t cap, const char *s)
{
   size_t n = strlen(out);
   while (*s && n + 1 < cap)
      out[n++] = *s++;
   out[n] = '\0';
}

static void zarchive_append_u64(char *out, size_t cap, uint64_t v)
{
   char digits[24];
   size_t i = sizeof(digits) - 1;
   digits[i] = '\0';
   do
   {
      digits[--i] = (char)('0' + (unsigned)(v % 10));
      v /= 10;
   } while (v);
   zarchive_append(out, cap, digits + i);
}

static void zarchive_append_hex32(char *out, size_t cap, uint32_t v)
{
   static const char hex[] = "0123456789abcdef";
   char digits[9];
   int i;
   for (i = 7; i >= 0; i--, v >>= 4)
      digits[i] = hex[v & 15];
   digits[8] = '\0';
   zarchive_append(out, cap, digits);
}

static uint32_t zarchive_be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* What the last 144 octets of a file say, against what an archive's footer
 * has there: its magic, its version, and the archive's size, which has to
 * be the file's */
static void zarchive_describe_footer(shared_file_t *file, uint64_t size, char *out, size_t cap)
{
   uint8_t f[ZARCHIVE_FOOTER_SIZE];
   out[0] = '\0';
   if (size < ZARCHIVE_FOOTER_SIZE)
   {
      zarchive_append(out, cap, "it is ");
      zarchive_append_u64(out, cap, size);
      zarchive_append(out, cap, " bytes long, too short for an archive");
      return;
   }
   if (shared_file_read_at(file, f, ZARCHIVE_FOOTER_SIZE, size - ZARCHIVE_FOOTER_SIZE) != ZARCHIVE_FOOTER_SIZE)
   {
      zarchive_append(out, cap, "its last 144 of ");
      zarchive_append_u64(out, cap, size);
      zarchive_append(out, cap, " bytes could not be read");
      return;
   }
   zarchive_append(out, cap, "its footer has magic ");
   zarchive_append_hex32(out, cap, zarchive_be32(f + 140));
   zarchive_append(out, cap, " (an archive's: 169f52d6), version ");
   zarchive_append_hex32(out, cap, zarchive_be32(f + 136));
   zarchive_append(out, cap, " (61bf3a01), size ");
   zarchive_append_u64(out, cap, ((uint64_t)zarchive_be32(f + 128) << 32) | zarchive_be32(f + 132));
   zarchive_append(out, cap, " (the file's: ");
   zarchive_append_u64(out, cap, size);
   zarchive_append(out, cap, ")");
}

static zarchive_t *zarchive_fail(zarchive_t *ar, char *why_not, size_t why_cap, const char *why)
{
   if (why_not && why_cap && why)
   {
      why_not[0] = '\0';
      zarchive_append(why_not, why_cap, why);
   }
   zarchive_close(ar);
   return NULL;
}

zarchive_t *zarchive_open(const char *path, unsigned flags, char *why_not, size_t why_cap)
{
   zar_request_t req;
   int err;
   size_t path_len;
   zar_t *z;
   zarchive_t *ar;
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
#endif
   if (why_not && why_cap)
      why_not[0] = '\0';
   if (!path)
      return zarchive_fail(NULL, why_not, why_cap, "it could not be opened");
   ar = (zarchive_t*)calloc(1, sizeof(*ar));
   if (!ar)
      return zarchive_fail(NULL, why_not, why_cap, "out of memory");
   retro_atomic_int_init(&ar->readers_made, 0);
   retro_atomic_int_init(&ar->holders, 0);
#ifdef RETRO_ATOMIC_HAS_PTR
   for (i = 0; i < ZARCHIVE_READER_SLOTS; i++)
      retro_atomic_ptr_init(&ar->readers[i], NULL);
#endif
   path_len = strlen(path);
   ar->path = (char*)malloc(path_len + 1);
   if (!ar->path)
      return zarchive_fail(ar, why_not, why_cap, "out of memory");
   memcpy(ar->path, path, path_len + 1);
   ar->file = shared_file_open(path, (flags & ZARCHIVE_OPEN_NO_MAP) ? SHARED_FILE_NO_MAP : 0);
   if (!ar->file)
      return zarchive_fail(ar, why_not, why_cap, "it could not be opened");
   ar->size = shared_file_size(ar->file);
   ar->mapped = shared_file_mapped(ar->file);

   /* the footer from the file, then the whole index after the blocks */
   z = zar_new(ar->size);
   if (!z)
      return zarchive_fail(ar, why_not, why_cap, "out of memory");
   while ((err = zar_open_step(z, &req)) == ZAR_PENDING)
      zar_feed(z, zarchive_supply(ar, &req));
   ar->index_offset = zar_index_offset(z);
   if (err != ZAR_OK || ar->index_offset >= ar->size)
   {
      zar_free(z);
      if (why_not && why_cap)
         zarchive_describe_footer(ar->file, ar->size, why_not, why_cap);
      return zarchive_fail(ar, NULL, 0, NULL);
   }
   if (!ar->mapped)
   {
      /* the index in memory, where everything but the blocks is read */
      const uint64_t index_size = ar->size - ar->index_offset;
      ar->index = index_size <= 0x7FFFFFFFUL ? (uint8_t*)malloc((size_t)index_size) : NULL;
      if (     !ar->index
            || shared_file_read_at(ar->file, ar->index, (int64_t)index_size, ar->index_offset)
               != (int64_t)index_size)
      {
         zar_free(z);
         free(ar->index);
         ar->index = NULL;
         if (why_not && why_cap)
         {
            why_not[0] = '\0';
            zarchive_append(why_not, why_cap, "its index, ");
            zarchive_append_u64(why_not, why_cap, index_size);
            zarchive_append(why_not, why_cap, " bytes from offset ");
            zarchive_append_u64(why_not, why_cap, ar->index_offset);
            zarchive_append(why_not, why_cap, ", could not be read");
         }
         return zarchive_fail(ar, NULL, 0, NULL);
      }
   }
   /* the reader that opened it is the pool's first */
   retro_atomic_fetch_add_int(&ar->readers_made, 1);
   zarchive_release(ar, z);
   return ar;
}

void zarchive_close(zarchive_t *ar)
{
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
#endif
   if (!ar)
      return;
#ifdef RETRO_ATOMIC_HAS_PTR
   for (i = 0; i < ZARCHIVE_READER_SLOTS; i++)
      zar_free((zar_t*)retro_atomic_exchange_ptr(&ar->readers[i], NULL));
#endif
   shared_file_close(ar->file);
   free(ar->index);
   free(ar->path);
   free(ar);
}

const char *zarchive_path(const zarchive_t *ar)
{
   return ar->path;
}

uint64_t zarchive_size(const zarchive_t *ar)
{
   return ar->size;
}

bool zarchive_is_mapped(const zarchive_t *ar)
{
   return ar->mapped != NULL;
}

unsigned zarchive_readers_made(const zarchive_t *ar)
{
   return (unsigned)retro_atomic_load_relaxed_int((retro_atomic_int_t*)&ar->readers_made);
}

unsigned zarchive_streams_opened(const zarchive_t *ar)
{
   return shared_file_streams_opened(ar->file);
}

/* --- lookups, listings and reads ------------------------------------------ */

/* path, NUL-terminated, into buf; false for one too long */
static bool zarchive_copy_path(char *buf, const char *path, size_t len)
{
   if (len >= ZARCHIVE_DIR_PATH_MAX)
      return false;
   if (len)
      memcpy(buf, path, len);
   buf[len] = '\0';
   return true;
}

bool zarchive_lookup(const zarchive_t *ar, const char *path, size_t len, zar_file_t *out)
{
   char p[ZARCHIVE_DIR_PATH_MAX];
   zar_request_t req;
   int err;
   zar_t *z;
   if (!zarchive_copy_path(p, path, len))
      return false;
   z = zarchive_acquire(ar);
   if (!z)
      return false;
   err = zar_lookup_begin(z, p);
   if (err == ZAR_OK)
   {
      while ((err = zar_lookup_step(z, &req, out)) == ZAR_PENDING)
         zar_feed(z, zarchive_supply(ar, &req));
   }
   zarchive_release(ar, z);
   return err == ZAR_OK;
}

uint64_t zarchive_read(const zarchive_t *ar, const zar_file_t *file, uint64_t offset,
      void *dst, uint64_t size)
{
   zar_request_t req;
   int err;
   uint64_t got;
   zar_t *z;
   if (file->dir || offset >= file->size)
      return 0;
   if (size > file->size - offset)
      size = file->size - offset;
   z = zarchive_acquire(ar);
   if (!z)
      return 0;
   err = zar_read_begin(z, file, offset, dst, (size_t)size);
   if (err == ZAR_OK)
   {
      while ((err = zar_read_step(z, &req)) == ZAR_PENDING)
         zar_feed(z, zarchive_supply(ar, &req));
   }
   got = zar_read_progress(z);
   zarchive_release(ar, z);
   return got;
}

bool zarchive_dir_open(const zarchive_t *ar, zarchive_dir_t *dir, const char *path, size_t len)
{
   dir->ar = ar;
   dir->z = NULL;
   dir->err = ZAR_ERROR_NOT_FOUND;
   if (!zarchive_copy_path(dir->path, path, len))
      return false;
   dir->z = zarchive_acquire(ar);
   if (!dir->z)
   {
      dir->err = ZAR_ERROR_MEM;
      return false;
   }
   /* zar keeps the path for the steps: it lives in dir */
   dir->err = zar_dir_begin(dir->z, dir->path);
   return dir->err == ZAR_OK;
}

bool zarchive_dir_next(zarchive_dir_t *dir, const char **name, size_t *len, zar_file_t *node)
{
   zar_request_t req;
   zar_dirent_t e;
   if (!dir->z || dir->err != ZAR_OK)
      return false;
   while ((dir->err = zar_dir_step(dir->z, &req, &e)) == ZAR_PENDING)
      zar_feed(dir->z, zarchive_supply(dir->ar, &req));
   if (dir->err != ZAR_OK)
   {
      /* over: the reader goes back now, not at close */
      zarchive_release(dir->ar, dir->z);
      dir->z = NULL;
      return false;
   }
   *name = e.name;
   *len = e.len;
   *node = e.file;
   return true;
}

bool zarchive_dir_failed(const zarchive_dir_t *dir)
{
   return dir->err != ZAR_OK && dir->err != ZAR_END;
}

void zarchive_dir_close(zarchive_dir_t *dir)
{
   if (dir->z)
      zarchive_release(dir->ar, dir->z);
   dir->z = NULL;
}

/* --- archives shared by path ----------------------------------------------- */

/* test/zarchive runs code of its own at the two moments the table can be
 * raced: an archive opened and not yet in the table, and one whose last
 * holder has let it go and not yet taken it out */
#ifdef ZARCHIVE_TEST_HOOKS
void zarchive_test_opened(const char *path);
void zarchive_test_let_go(const zarchive_t *ar);
#define ZARCHIVE_TEST_OPENED(path) zarchive_test_opened(path)
#define ZARCHIVE_TEST_LET_GO(ar) zarchive_test_let_go(ar)
#else
#define ZARCHIVE_TEST_OPENED(path) do { } while (0)
#define ZARCHIVE_TEST_LET_GO(ar) do { } while (0)
#endif

/* the shared archives, a table replaced whole when one comes or goes */
typedef struct zarchive_shared
{
   size_t count;
   zarchive_t *ar[1]; /* count of them */
} zarchive_shared_t;

static retro_atomic_ptr_t zarchive_shared_cell_ptr;

/* Made by the first to share an archive and kept for the process: two at
 * once both make one, and the one not put in place is let go - no turn is
 * waited for. */
static rcu_cell_t *zarchive_shared_cell(void)
{
   rcu_cell_t *cell = (rcu_cell_t*)retro_atomic_load_acquire_ptr(&zarchive_shared_cell_ptr);
   if (cell)
      return cell;
   cell = (rcu_cell_t*)calloc(1, sizeof(*cell));
   if (!cell)
      return NULL;
   if (!rcu_cell_init(cell, NULL))
   {
      rcu_cell_deinit(cell);
      free(cell);
      return NULL;
   }
   if (!retro_atomic_cas_ptr(&zarchive_shared_cell_ptr, NULL, cell))
   {
      rcu_cell_deinit(cell);
      free(cell);
   }
   return (rcu_cell_t*)retro_atomic_load_acquire_ptr(&zarchive_shared_cell_ptr);
}

/* one more holder, unless there are none left: then it is on its way out */
static bool zarchive_hold(zarchive_t *ar)
{
   int n = retro_atomic_load_acquire_int(&ar->holders);
   while (n > 0)
   {
      if (retro_atomic_cas_int(&ar->holders, n, n + 1))
         return true;
      n = retro_atomic_load_acquire_int(&ar->holders);
   }
   return false;
}

/* an archive in the table for path that takes one more holder */
static zarchive_t *zarchive_shared_find(const zarchive_shared_t *t, const char *path)
{
   size_t i;
   if (!t)
      return NULL;
   for (i = 0; i < t->count; i++)
   {
      if (strcmp(t->ar[i]->path, path) == 0 && zarchive_hold(t->ar[i]))
         return t->ar[i];
   }
   return NULL;
}

/* t with add put in, or drop taken out; NULL for an empty table, and for
 * one that could not be made (failed then says so) */
static zarchive_shared_t *zarchive_shared_with(const zarchive_shared_t *t,
      zarchive_t *add, const zarchive_t *drop, bool *failed)
{
   size_t have = t ? t->count : 0;
   size_t i, n = 0;
   zarchive_shared_t *next;
   *failed = false;
   for (i = 0; i < have; i++)
      n += t->ar[i] != drop;
   if (n + (add != NULL) == 0)
      return NULL;
   next = (zarchive_shared_t*)malloc(sizeof(*next) + n * sizeof(next->ar[0]));
   if (!next)
   {
      *failed = true;
      return NULL;
   }
   n = 0;
   for (i = 0; i < have; i++)
   {
      if (t->ar[i] != drop)
         next->ar[n++] = t->ar[i];
   }
   if (add)
      next->ar[n++] = add;
   next->count = n;
   return next;
}

zarchive_t *zarchive_acquire_shared(const char *path)
{
   rcu_cell_t *cell = zarchive_shared_cell();
   rcu_reader_t r;
   zarchive_t *found;
   zarchive_t *opened;
   const zarchive_shared_t *t;
   zarchive_shared_t *next;
   bool failed;
   if (!cell || !path)
      return NULL;
   /* held already: one more holder, and no turn taken */
   found = zarchive_shared_find((const zarchive_shared_t*)rcu_read_begin(cell, &r), path);
   rcu_read_end(&r);
   if (found)
      return found;
   /* opening one can take a while: done outside a writer's turn */
   opened = zarchive_open(path, 0, NULL, 0);
   if (!opened)
      return NULL;
   retro_atomic_store_release_int(&opened->holders, 1);
   ZARCHIVE_TEST_OPENED(path);
   rcu_write_begin(cell);
   t = (const zarchive_shared_t*)rcu_write_current(cell);
   /* opened by another meanwhile: that one is shared */
   found = zarchive_shared_find(t, path);
   next = found ? NULL : zarchive_shared_with(t, opened, NULL, &failed);
   if (found || failed)
   {
      /* the table as it was: nothing for the readers to move off */
      rcu_write_publish(cell, (void*)t);
      zarchive_close(opened);
      return found;
   }
   free(rcu_write_publish(cell, next));
   return opened;
}

void zarchive_release_shared(zarchive_t *ar)
{
   rcu_cell_t *cell;
   zarchive_shared_t *next;
   bool failed;
   if (!ar)
      return;
   if (retro_atomic_fetch_sub_int(&ar->holders, 1) != 1)
      return;
   /* The last holder: none takes it now. Out of the table, and closed once
    * every reader that may have seen it in the table has left. */
   ZARCHIVE_TEST_LET_GO(ar);
   cell = (rcu_cell_t*)retro_atomic_load_acquire_ptr(&zarchive_shared_cell_ptr);
   rcu_write_begin(cell);
   next = zarchive_shared_with((const zarchive_shared_t*)rcu_write_current(cell), NULL, ar, &failed);
   if (failed)
   {
      /* no memory for a smaller table: left in it, held by none and taken
       * by none - kept open rather than closed under a reader */
      rcu_write_publish(cell, (void*)rcu_write_current(cell));
      return;
   }
   free(rcu_write_publish(cell, next));
   zarchive_close(ar);
}

size_t zarchive_shared_count(void)
{
   rcu_cell_t *cell = (rcu_cell_t*)retro_atomic_load_acquire_ptr(&zarchive_shared_cell_ptr);
   rcu_reader_t r;
   const zarchive_shared_t *t;
   size_t i, n = 0;
   if (!cell)
      return 0;
   t = (const zarchive_shared_t*)rcu_read_begin(cell, &r);
   for (i = 0; t && i < t->count; i++)
   {
      if (retro_atomic_load_acquire_int(&t->ar[i]->holders) > 0)
         n++;
   }
   rcu_read_end(&r);
   return n;
}
