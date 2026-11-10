/* Wii U archives (zarchive.h) */

#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>

#include "Common/shared_file.h"
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
