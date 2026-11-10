/* Homebrew bundles, .wuhb (wuhb.h) */

#include <stdlib.h>
#include <string.h>

#include "Common/shared_file.h"
#include "wuhb.h"

/* the header: "WUHB", its size, then where and how big each table is */
#define WUHB_HEADER_SIZE 0x50
/* an entry's fields before its name */
#define WUHB_DIR_FIXED 24
#define WUHB_FILE_FIXED 32
/* a hash chain longer than this is one that loops */
#define WUHB_CHAIN_MAX 0x100000

struct wuhb
{
   shared_file_t *file;
   const uint8_t *mapped; /* its octets, where mapped */
   uint64_t size;
   uint64_t dir_hash_ofs, dir_hash_size;
   uint64_t dir_table_ofs, dir_table_size;
   uint64_t file_hash_ofs, file_hash_size;
   uint64_t file_table_ofs, file_table_size;
   uint64_t partition_ofs;
};

static uint32_t wuhb_be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t wuhb_be64(const uint8_t *p)
{
   return ((uint64_t)wuhb_be32(p) << 32) | wuhb_be32(p + 4);
}

/* len octets at offset, all of them or false */
static bool wuhb_read_at(const wuhb_t *wuhb, uint64_t offset, void *dst, uint64_t len)
{
   if (offset > wuhb->size || wuhb->size - offset < len)
      return false;
   if (wuhb->mapped)
   {
      memcpy(dst, wuhb->mapped + offset, (size_t)len);
      return true;
   }
   return shared_file_read_at(wuhb->file, dst, (int64_t)len, offset) == (int64_t)len;
}

wuhb_t *wuhb_open(const char *path, unsigned flags)
{
   uint8_t h[WUHB_HEADER_SIZE];
   wuhb_t *wuhb;
   shared_file_t *file = shared_file_open(path,
         (flags & WUHB_OPEN_NO_MAP) ? SHARED_FILE_NO_MAP : 0);
   if (!file)
      return NULL;
   wuhb = (wuhb_t*)calloc(1, sizeof(*wuhb));
   if (!wuhb)
   {
      shared_file_close(file);
      return NULL;
   }
   wuhb->file = file;
   wuhb->mapped = shared_file_mapped(file);
   wuhb->size = shared_file_size(file);
   if (!wuhb_read_at(wuhb, 0, h, WUHB_HEADER_SIZE) || memcmp(h, "WUHB", 4) != 0)
   {
      wuhb_close(wuhb);
      return NULL;
   }
   wuhb->dir_hash_ofs = wuhb_be64(h + 0x08);
   wuhb->dir_hash_size = wuhb_be64(h + 0x10);
   wuhb->dir_table_ofs = wuhb_be64(h + 0x18);
   wuhb->dir_table_size = wuhb_be64(h + 0x20);
   wuhb->file_hash_ofs = wuhb_be64(h + 0x28);
   wuhb->file_hash_size = wuhb_be64(h + 0x30);
   wuhb->file_table_ofs = wuhb_be64(h + 0x38);
   wuhb->file_table_size = wuhb_be64(h + 0x40);
   wuhb->partition_ofs = wuhb_be64(h + 0x48);
   /* a hash table with no room for one entry is none */
   if (wuhb->dir_hash_size < 4 || wuhb->file_hash_size < 4)
   {
      wuhb_close(wuhb);
      return NULL;
   }
   return wuhb;
}

void wuhb_close(wuhb_t *wuhb)
{
   if (!wuhb)
      return;
   shared_file_close(wuhb->file);
   free(wuhb);
}

bool wuhb_is_mapped(const wuhb_t *wuhb)
{
   return wuhb->mapped != NULL;
}

/* An entry's fields, and where its name lies */
static bool wuhb_read_fixed(const wuhb_t *wuhb, uint32_t entry, bool is_file,
      uint8_t *fixed, uint64_t *name_at)
{
   const uint64_t table_ofs = is_file ? wuhb->file_table_ofs : wuhb->dir_table_ofs;
   const uint64_t table_size = is_file ? wuhb->file_table_size : wuhb->dir_table_size;
   const unsigned fixed_size = is_file ? WUHB_FILE_FIXED : WUHB_DIR_FIXED;
   uint64_t at;
   if (entry == WUHB_ENTRY_EMPTY || entry >= table_size)
      return false;
   at = table_ofs + entry;
   if (at < table_ofs || !wuhb_read_at(wuhb, at, fixed, fixed_size))
      return false;
   *name_at = at + fixed_size;
   return true;
}

/* The name into name, cut to name_cap - 1 and NUL-terminated */
static bool wuhb_read_name(const wuhb_t *wuhb, uint64_t name_at, uint32_t name_size,
      char *name, size_t name_cap)
{
   size_t n;
   if (!name || !name_cap)
      return true;
   n = name_size < name_cap - 1 ? name_size : name_cap - 1;
   name[0] = '\0';
   if (!wuhb_read_at(wuhb, name_at, name, n))
      return false;
   name[n] = '\0';
   return true;
}

bool wuhb_dir_entry(const wuhb_t *wuhb, uint32_t entry, wuhb_dir_t *out, char *name, size_t name_cap)
{
   uint8_t f[WUHB_DIR_FIXED];
   uint64_t name_at;
   if (!wuhb_read_fixed(wuhb, entry, false, f, &name_at))
      return false;
   out->parent = wuhb_be32(f);
   out->list_next = wuhb_be32(f + 4);
   out->dir_list_head = wuhb_be32(f + 8);
   out->file_list_head = wuhb_be32(f + 12);
   out->hash = wuhb_be32(f + 16);
   out->name_size = wuhb_be32(f + 20);
   return wuhb_read_name(wuhb, name_at, out->name_size, name, name_cap);
}

bool wuhb_file_entry(const wuhb_t *wuhb, uint32_t entry, wuhb_file_t *out, char *name, size_t name_cap)
{
   uint8_t f[WUHB_FILE_FIXED];
   uint64_t name_at;
   if (!wuhb_read_fixed(wuhb, entry, true, f, &name_at))
      return false;
   out->parent = wuhb_be32(f);
   out->list_next = wuhb_be32(f + 4);
   out->offset = wuhb_be64(f + 8);
   out->size = wuhb_be64(f + 16);
   out->hash = wuhb_be32(f + 24);
   out->name_size = wuhb_be32(f + 28);
   return wuhb_read_name(wuhb, name_at, out->name_size, name, name_cap);
}

uint64_t wuhb_file_size(const wuhb_t *wuhb, uint32_t entry)
{
   wuhb_file_t f;
   return wuhb_file_entry(wuhb, entry, &f, NULL, 0) ? f.size : 0;
}

uint64_t wuhb_read_file(wuhb_t *wuhb, uint32_t entry, uint64_t file_offset, uint64_t len, void *dst)
{
   wuhb_file_t f;
   uint64_t at, n;
   int64_t got;
   if (!wuhb_file_entry(wuhb, entry, &f, NULL, 0) || file_offset >= f.size)
      return 0;
   n = f.size - file_offset < len ? f.size - file_offset : len;
   at = wuhb->partition_ofs + f.offset;
   if (at < wuhb->partition_ofs || at + file_offset < at)
      return 0;
   at += file_offset;
   /* short where the image is */
   if (at >= wuhb->size)
      return 0;
   if (n > wuhb->size - at)
      n = wuhb->size - at;
   if (wuhb->mapped)
   {
      memcpy(dst, wuhb->mapped + at, (size_t)n);
      return n;
   }
   got = shared_file_read_at(wuhb->file, dst, (int64_t)n, at);
   return got > 0 ? (uint64_t)got : 0;
}

/* RomFS's hash of a name under its parent: case does not count */
static uint32_t wuhb_hash(uint32_t parent, const char *name, size_t len)
{
   uint32_t hash = parent ^ 123456789UL;
   size_t i;
   for (i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)name[i];
      if (c >= 'a' && c <= 'z')
         c = (unsigned char)(c - 'a' + 'A');
      hash = (hash >> 5) | (hash << 27);
      hash ^= c;
   }
   return hash;
}

/* Whether the name at name_at is name, compared where it lies when mapped */
static bool wuhb_name_is(const wuhb_t *wuhb, uint64_t name_at, uint32_t name_size,
      const char *name, size_t len)
{
   char buf[256];
   size_t done;
   if (name_size != len)
      return false;
   if (wuhb->mapped)
      return name_at <= wuhb->size && wuhb->size - name_at >= len
         && memcmp(wuhb->mapped + name_at, name, len) == 0;
   for (done = 0; done < len;)
   {
      size_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
      if (!wuhb_read_at(wuhb, name_at + done, buf, n) || memcmp(buf, name + done, n) != 0)
         return false;
      done += n;
   }
   return true;
}

/* The entry called name under the entry at *at, into *at */
static bool wuhb_look(const wuhb_t *wuhb, uint32_t *at, const char *name, size_t len, bool is_file)
{
   const uint64_t table_ofs = is_file ? wuhb->file_hash_ofs : wuhb->dir_hash_ofs;
   const uint64_t table_size = is_file ? wuhb->file_hash_size : wuhb->dir_hash_size;
   const uint64_t slot = wuhb_hash(*at, name, len) % (table_size / 4);
   uint8_t b[4];
   uint8_t fixed[WUHB_FILE_FIXED];
   uint32_t entry, steps;
   if (table_ofs + slot * 4 < table_ofs || !wuhb_read_at(wuhb, table_ofs + slot * 4, b, 4))
      return false;
   entry = wuhb_be32(b);
   /* a chain broken by a bad offset, or one that loops, ends the search */
   for (steps = 0; steps < WUHB_CHAIN_MAX && entry != WUHB_ENTRY_EMPTY; steps++)
   {
      uint64_t name_at;
      const unsigned fields = is_file ? WUHB_FILE_FIXED : WUHB_DIR_FIXED;
      if (!wuhb_read_fixed(wuhb, entry, is_file, fixed, &name_at))
         return false;
      if (wuhb_name_is(wuhb, name_at, wuhb_be32(fixed + fields - 4), name, len))
      {
         *at = entry;
         return true;
      }
      entry = wuhb_be32(fixed + fields - 8);
   }
   return false;
}

uint32_t wuhb_lookup(const wuhb_t *wuhb, const char *path, size_t len, bool is_file)
{
   uint32_t at = 0;
   size_t i = 0;
   /* the root, a directory with no name */
   if (!wuhb_look(wuhb, &at, "", 0, false))
      return WUHB_ENTRY_EMPTY;
   for (;;)
   {
      size_t start, end;
      while (i < len && path[i] == '/')
         i++;
      if (i >= len)
         break;
      start = i;
      while (i < len && path[i] != '/')
         i++;
      end = i;
      /* a file is the last name, with nothing after it */
      if (!wuhb_look(wuhb, &at, path + start, end - start, is_file && end == len))
         return WUHB_ENTRY_EMPTY;
      if (is_file && end == len)
         return at;
   }
   /* a file path that ends in '/' names no file; nothing but '/' is the
    * root */
   if (is_file && len > 0 && path[len - 1] == '/')
   {
      for (i = 0; i < len && path[i] == '/'; i++)
         ;
      if (i < len)
         return WUHB_ENTRY_EMPTY;
   }
   return at;
}
