/* A volume's file system table, parsed once (fst_table.h). */

#include <stdlib.h>
#include <string.h>

#include "fst_table.h"

#define FST_MAGIC            0x46535400UL /* "FST\0" */
#define FST_MAX_CLUSTERS     0x1000
#define FST_ENTRY_SIZE       0x10

#define ENTRY_FILE           0
#define ENTRY_DIR            1

typedef struct fst_entry
{
   uint32_t name_offset;
   uint32_t parent;      /* its directory's index; 0 for the root's entries */
   uint32_t a;           /* a file's offset; a directory's end index */
   uint32_t size;        /* a file's */
   uint16_t cluster;     /* a file's */
   uint16_t name_hash;
   uint8_t  type;
   uint8_t  link;
} fst_entry_t;

struct fst_table
{
   fst_table_cluster_t *clusters;
   fst_entry_t *entries;
   const char *names;
   uint32_t names_size;
   uint32_t offset_factor;
   uint32_t cluster_count;
   uint32_t entry_count;
   uint32_t file_count;
   bool hash_disabled;
};

static uint32_t be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
      | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
   return (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static unsigned char lower(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* names hashed to pass most of them over without comparing them */
static uint16_t name_hash(const char *name, size_t len)
{
   unsigned v = 0;
   size_t i;
   for (i = 0; i < len; i++)
   {
      v = (v + lower((unsigned char)name[i])) & 0xFFFFu;
      v = ((v >> 3) | (v << 13)) & 0xFFFFu;
   }
   return (uint16_t)v;
}

/* the entry's name is name (len octets), ignoring the case of A-Z */
static bool name_is(const fst_table_t *t, const fst_entry_t *e,
      const char *name, size_t len)
{
   const char *have = t->names + e->name_offset;
   size_t i;
   for (i = 0; i < len; i++)
   {
      if (have[i] == '\0' || lower((unsigned char)have[i]) != lower((unsigned char)name[i]))
         return false;
   }
   return have[len] == '\0';
}

/* The entries, checked as they go: each name in the name table, each file
 * in a cluster there is, each directory within its own and naming it as
 * its parent. The directories an entry is in are a stack kept in the
 * entries already made: the top is the innermost one, its end its own,
 * the one under it its parent. The root ends past the last entry (its end
 * is how many there are), so it is never let go. */
static bool parse_entries(fst_table_t *t, const uint8_t *in)
{
   uint32_t i;
   uint32_t top = 0;
   for (i = 0; i < t->entry_count; i++, in += FST_ENTRY_SIZE)
   {
      fst_entry_t *e = &t->entries[i];
      uint32_t type_name = be32(in);
      uint32_t at = type_name & 0xFFFFFFUL;
      uint32_t pos = at;
      uint8_t bits = (uint8_t)(type_name >> 24);
      while (i > 0 && i >= t->entries[top].a)
         top = t->entries[top].parent;
      while (pos < t->names_size && t->names[pos] != '\0')
         pos++;
      if (pos >= t->names_size)
         return false; /* past the name table, or not ended in it */
      e->name_offset = at;
      e->name_hash = name_hash(t->names + at, pos - at);
      e->parent = top;
      e->link = (bits & 0x80) != 0;
      e->type = (bits & 1) ? ENTRY_DIR : ENTRY_FILE;
      if (e->type == ENTRY_FILE)
      {
         e->a = be32(in + 4);
         e->size = be32(in + 8);
         e->cluster = be16(in + 14);
         if (e->cluster >= t->cluster_count)
            return false;
         t->file_count++;
      }
      else
      {
         uint32_t parent = be32(in + 4);
         uint32_t end = be32(in + 8);
         uint32_t top_end = i == 0 ? t->entry_count : t->entries[top].a;
         if (end <= i || end > top_end || parent != top)
            return false;
         e->a = end;
         e->size = 0;
         e->cluster = 0;
         top = i;
      }
   }
   return true;
}

fst_table_t *fst_table_parse(const void *fst, uint32_t size)
{
   const uint8_t *in = (const uint8_t*)fst;
   const uint8_t *files;
   uint32_t clusters, entries, names_at, i;
   size_t bytes;
   fst_table_t *t;
   char *names;

   if (!fst || size < FST_TABLE_HEADER_SIZE || be32(in) != FST_MAGIC)
      return NULL;
   clusters = be32(in + 8);
   if (clusters >= FST_MAX_CLUSTERS)
      return NULL;
   /* the root directory, after the clusters, says how many entries */
   files = in + FST_TABLE_HEADER_SIZE + (size_t)clusters * FST_TABLE_HEADER_SIZE;
   if ((size_t)(files - in) + FST_ENTRY_SIZE > size)
      return NULL;
   if (!(files[0] & 1))
      return NULL;
   entries = be32(files + 8);
   if (entries == 0 || entries > (size - (uint32_t)(files - in)) / FST_ENTRY_SIZE)
      return NULL;
   /* where there is no name table, the root's name is past it */
   names_at = (uint32_t)(files - in) + entries * FST_ENTRY_SIZE;

   bytes = sizeof(*t) + (size_t)clusters * sizeof(fst_table_cluster_t)
      + (size_t)entries * sizeof(fst_entry_t) + (size - names_at);
   t = (fst_table_t*)malloc(bytes);
   if (!t)
      return NULL;
   t->clusters = (fst_table_cluster_t*)(t + 1);
   t->entries = (fst_entry_t*)(t->clusters + clusters);
   names = (char*)(t->entries + entries);
   memcpy(names, in + names_at, size - names_at);
   t->names = names;
   t->names_size = size - names_at;
   t->offset_factor = be32(in + 4);
   t->hash_disabled = in[12] != 0;
   t->cluster_count = clusters;
   t->entry_count = entries;
   t->file_count = 0;
   for (i = 0; i < clusters; i++)
   {
      const uint8_t *c = in + FST_TABLE_HEADER_SIZE + (size_t)i * FST_TABLE_HEADER_SIZE;
      t->clusters[i].offset = be32(c);
      t->clusters[i].size = be32(c + 4);
      t->clusters[i].hash_mode = c[0x14];
   }
   if (!parse_entries(t, files))
   {
      free(t);
      return NULL;
   }
   return t;
}

void fst_table_free(fst_table_t *table)
{
   free(table);
}

uint32_t fst_table_offset_factor(const fst_table_t *table)
{
   return table->offset_factor;
}

bool fst_table_hash_disabled(const fst_table_t *table)
{
   return table->hash_disabled;
}

uint32_t fst_table_cluster_count(const fst_table_t *table)
{
   return table->cluster_count;
}

const fst_table_cluster_t *fst_table_cluster(const fst_table_t *table, uint32_t index)
{
   return index < table->cluster_count ? &table->clusters[index] : NULL;
}

uint32_t fst_table_entry_count(const fst_table_t *table)
{
   return table->entry_count;
}

uint32_t fst_table_file_count(const fst_table_t *table)
{
   return table->file_count;
}

static bool is_slash(char c)
{
   return c == '/' || c == '\\';
}

/* the entry named name (len octets) in [first, end), a directory's own
 * entries passed over */
static uint32_t find_in(const fst_table_t *t, uint32_t first, uint32_t end,
      const char *name, size_t len)
{
   uint16_t hash = name_hash(name, len);
   uint32_t i = first;
   while (i < end)
   {
      const fst_entry_t *e = &t->entries[i];
      if (e->name_hash == hash && name_is(t, e, name, len))
         return i;
      i = e->type == ENTRY_DIR ? e->a : i + 1;
   }
   return FST_TABLE_NONE;
}

uint32_t fst_table_lookup(const fst_table_t *table, const char *path, size_t len, bool only_files)
{
   const char *p = path;
   const char *end = path + len;
   uint32_t found = 0;
   while (p < end)
   {
      const char *name;
      const fst_entry_t *dir;
      if (is_slash(*p))
      {
         p++;
         continue;
      }
      name = p;
      while (p < end && !is_slash(*p))
         p++;
      if (p - name == 1 && *name == '.')
         continue;
      dir = &table->entries[found];
      if (dir->type != ENTRY_DIR)
         return FST_TABLE_NONE; /* a name under a file */
      found = find_in(table, found + 1, dir->a, name, (size_t)(p - name));
      if (found == FST_TABLE_NONE)
         return FST_TABLE_NONE;
   }
   if (only_files && table->entries[found].type != ENTRY_FILE)
      return FST_TABLE_NONE;
   return found;
}

bool fst_table_is_dir(const fst_table_t *table, uint32_t entry)
{
   return entry < table->entry_count && table->entries[entry].type == ENTRY_DIR;
}

bool fst_table_is_file(const fst_table_t *table, uint32_t entry)
{
   return entry < table->entry_count && table->entries[entry].type == ENTRY_FILE;
}

bool fst_table_is_link(const fst_table_t *table, uint32_t entry)
{
   return entry < table->entry_count && table->entries[entry].link;
}

const char *fst_table_name(const fst_table_t *table, uint32_t entry)
{
   if (entry >= table->entry_count)
      return "";
   return table->names + table->entries[entry].name_offset;
}

size_t fst_table_path(const fst_table_t *table, uint32_t entry, char *buf, size_t size)
{
   size_t len = 0;
   size_t at;
   uint32_t i;
   if (entry >= table->entry_count)
   {
      if (size)
         buf[0] = '\0';
      return 0;
   }
   /* a directory's index is below its entries', so this ends */
   for (i = entry;; i = table->entries[i].parent)
   {
      len += strlen(fst_table_name(table, i));
      if (table->entries[i].parent == 0)
         break;
      len++; /* the '/' before it */
   }
   if (len >= size)
   {
      if (size)
         buf[0] = '\0';
      return len;
   }
   buf[len] = '\0';
   at = len;
   for (i = entry;; i = table->entries[i].parent)
   {
      const char *name = fst_table_name(table, i);
      size_t n = strlen(name);
      at -= n;
      memcpy(buf + at, name, n);
      if (table->entries[i].parent == 0)
         break;
      buf[--at] = '/';
   }
   return len;
}

bool fst_table_file(const fst_table_t *table, uint32_t entry, fst_table_file_t *out)
{
   const fst_entry_t *e;
   if (!fst_table_is_file(table, entry))
      return false;
   e = &table->entries[entry];
   out->offset = e->a;
   out->size = e->size;
   out->cluster = e->cluster;
   return true;
}

bool fst_table_dir_open(const fst_table_t *table, uint32_t dir, fst_table_dir_t *it)
{
   if (!fst_table_is_dir(table, dir))
      return false;
   it->next = dir + 1;
   it->end = table->entries[dir].a;
   return true;
}

uint32_t fst_table_dir_next(const fst_table_t *table, fst_table_dir_t *it)
{
   uint32_t i = it->next;
   if (i >= it->end || i >= table->entry_count)
      return FST_TABLE_NONE;
   it->next = table->entries[i].type == ENTRY_DIR ? table->entries[i].a : i + 1;
   return i;
}
