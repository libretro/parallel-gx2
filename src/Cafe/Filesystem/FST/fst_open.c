/* A Wii U disc image's game partition, as a volume (fst_open.h) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/crypto/aes128.h"
#include "fst_open.h"

#define SECTOR                0x8000
#define DISC_MAGIC            0xCC549EB9UL /* at 0x10000 */
#define TABLE_OFFSET          0x18000
#define TABLE_MAGIC           0xCCA6E67BUL
#define TABLE_ENTRIES         0x800
#define TABLE_ENTRY_SIZE      0x80
/* there is room for 240; more than this is taken for a table not read */
#define MAX_PARTITIONS        30
#define PARTITION_MAGIC       0xCC93A4F5UL
#define PARTITION_HEADER_SIZE 0x60
#define NONE                  0xFFFFFFFFUL

/* tickets */
#define TICKET_HEADER_SIZE    0x220
#define TICKET_TITLE_KEY      0x1BF
#define TICKET_TITLE_ID       0x1DC

static uint32_t be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
      | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool fst_ticket_title_key(const uint8_t *tik, size_t size, uint8_t *key)
{
   /* the Wii U's common key */
   static const uint8_t common[16] = {
      0xD7, 0xB0, 0x04, 0x02, 0x65, 0x9B, 0xA2, 0xAB,
      0xD2, 0xCB, 0x0D, 0xB2, 0x7F, 0xA2, 0xB6, 0x56};
   uint8_t iv[16];
   if (!tik || size < TICKET_HEADER_SIZE)
      return false;
   /* a Wii U title's id starts 0005 */
   if ((be32(tik + TICKET_TITLE_ID) >> 16) != 0x5)
      return false;
   /* the key is encrypted with the title id, then 8 zeroes, as the IV */
   memcpy(iv, tik + TICKET_TITLE_ID, 8);
   memset(iv + 8, 0, 8);
   AES128_CBC_decrypt(key, tik + TICKET_TITLE_KEY, 16, common, iv);
   return true;
}

typedef struct partition
{
   uint32_t address;  /* in sectors */
   uint32_t fst_sector; /* in sectors, from the partition's start */
   uint32_t fst_size;
} partition_t;

static bool read_partition(fst_source_t *src, partition_t *p)
{
   uint8_t h[PARTITION_HEADER_SIZE];
   if (fst_source_read(src, 0, 0, (uint64_t)p->address * SECTOR, h, sizeof(h)) != sizeof(h))
      return false;
   if (be32(h) != PARTITION_MAGIC)
      return false;
   p->fst_size = be32(h + 0x14);
   p->fst_sector = be32(h + 0x18);
   return true;
}

static fst_volume_t *open_partition(fst_source_t *src, bool take, const partition_t *p,
      const uint8_t *key)
{
   fst_source_set_base_offset(src, (uint64_t)p->address * SECTOR);
   return fst_volume_open(src, take, (uint64_t)p->fst_sector * SECTOR, p->fst_size,
         key, NULL, 0, NULL);
}

/* The partition table, decrypted: the SI partition and the first GM one.
 * FST_DISC_* */
static int read_table(fst_source_t *src, const uint8_t *disc_key, partition_t *si,
      partition_t *gm, uint32_t *gm_index, uint32_t *detail)
{
   uint8_t iv[16];
   uint8_t *table = (uint8_t*)malloc(SECTOR);
   uint32_t count, i;
   uint32_t si_index = NONE;
   int r = FST_DISC_OK;
   *gm_index = NONE;
   if (!table)
      return FST_DISC_NO_IMAGE;
   if (fst_source_read(src, 0, 0, TABLE_OFFSET, table, SECTOR) != SECTOR)
   {
      free(table);
      return FST_DISC_NO_IMAGE;
   }
   memset(iv, 0, sizeof(iv));
   AES128_CBC_decrypt(table, table, SECTOR, disc_key, iv);
   if (be32(table) != TABLE_MAGIC)
      r = FST_DISC_WRONG_KEY;
   else if (be32(table + 4) != SECTOR)
      r = FST_DISC_BAD_TABLE;
   else if ((count = be32(table + 0x1C)) > MAX_PARTITIONS)
   {
      *detail = count;
      r = FST_DISC_BAD_TABLE;
   }
   else
   {
      for (i = 0; i < count && r == FST_DISC_OK; i++)
      {
         const uint8_t *e = table + TABLE_ENTRIES + i * TABLE_ENTRY_SIZE;
         if (e[0x1F] != 1)
         {
            /* one address each is all that is read */
            *detail = e[0x1F];
            r = FST_DISC_BAD_TABLE;
         }
         else if (e[0] == 'S' && e[1] == 'I')
         {
            if (si_index != NONE)
               r = FST_DISC_NO_PARTITION;
            si_index = i;
            si->address = be32(e + 0x20);
         }
         else if (e[0] == 'G' && e[1] == 'M' && *gm_index == NONE)
         {
            /* the first: likely not always the one, but it is for nearly
             * every disc */
            *gm_index = i;
            gm->address = be32(e + 0x20);
         }
      }
      if (r == FST_DISC_OK && (si_index == NONE || *gm_index == NONE))
         r = FST_DISC_NO_PARTITION;
   }
   free(table);
   return r;
}

/* the GM partition's title key, from its ticket in the SI partition */
static int gm_title_key(fst_source_t *src, const partition_t *si, uint32_t gm_index,
      const uint8_t *disc_key, uint8_t *key)
{
   char path[16];
   fst_volume_t *vol = open_partition(src, false, si, disc_key);
   const fst_table_t *table;
   fst_table_file_t file;
   uint32_t entry;
   uint8_t *tik;
   bool ok;
   if (!vol)
      return FST_DISC_SI_FST;
   table = fst_volume_table(vol);
   sprintf(path, "%02x/title.tik", (unsigned)(gm_index & 0xFF));
   entry = fst_table_lookup(table, path, strlen(path), true);
   if (entry == FST_TABLE_NONE || !fst_table_file(table, entry, &file) || !file.size)
   {
      fst_volume_free(vol);
      return FST_DISC_BAD_TICKET;
   }
   tik = (uint8_t*)malloc(file.size);
   ok = tik && fst_volume_read(vol, entry, 0, file.size, tik, NULL) == file.size
      && fst_ticket_title_key(tik, file.size, key);
   free(tik);
   fst_volume_free(vol);
   return ok ? FST_DISC_OK : FST_DISC_BAD_TICKET;
}

fst_volume_t *fst_disc_open(const char *path, const uint8_t *disc_key, int *why, uint32_t *detail)
{
   fst_source_t *src;
   fst_volume_t *gm_vol;
   partition_t si, gm;
   uint32_t gm_index;
   uint8_t magic[4];
   uint8_t title_key[16];
   int dummy_why;
   uint32_t dummy_detail;
   if (!why)
      why = &dummy_why;
   if (!detail)
      detail = &dummy_detail;
   *detail = 0;
   *why = FST_DISC_NO_IMAGE;
   src = path && disc_key ? fst_source_open_disc(path, 0) : NULL;
   if (!src)
      return NULL;
   memset(&si, 0, sizeof(si));
   memset(&gm, 0, sizeof(gm));
   if (fst_source_read(src, 0, 0, 2 * SECTOR, magic, 4) != 4 || be32(magic) != DISC_MAGIC)
      goto fail;
   *why = read_table(src, disc_key, &si, &gm, &gm_index, detail);
   if (*why != FST_DISC_OK)
      goto fail;
   if (!read_partition(src, &si) || !read_partition(src, &gm))
   {
      *why = FST_DISC_BAD_PARTITION;
      goto fail;
   }
   *why = gm_title_key(src, &si, gm_index, disc_key, title_key);
   if (*why != FST_DISC_OK)
      goto fail;
   gm_vol = open_partition(src, true, &gm, title_key); /* takes src, also when it fails */
   *why = gm_vol ? FST_DISC_OK : FST_DISC_GM_FST;
   return gm_vol;

fail:
   fst_source_close(src);
   return NULL;
}

/* --- titles ------------------------------------------------------------------ */

#define TMD_HEADER_SIZE     (0x204 + 64 * 36)
#define TMD_TITLE_ID        0x18C
#define TMD_TITLE_VERSION   0x1DC
#define TMD_CONTENT_COUNT   0x1DE
#define TMD_CONTENT_SIZE    0x30

static uint16_t be16(const uint8_t *p)
{
   return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

bool fst_tmd_parse(const uint8_t *tmd, size_t size, fst_tmd_t *out)
{
   uint16_t count, i;
   memset(out, 0, sizeof(*out));
   if (!tmd || size < TMD_HEADER_SIZE)
      return false;
   count = be16(tmd + TMD_CONTENT_COUNT);
   if (size < TMD_HEADER_SIZE + (size_t)count * TMD_CONTENT_SIZE)
      return false;
   out->title_id = ((uint64_t)be32(tmd + TMD_TITLE_ID) << 32) | be32(tmd + TMD_TITLE_ID + 4);
   out->title_version = be16(tmd + TMD_TITLE_VERSION);
   out->contents = (fst_tmd_content_t*)calloc(count ? count : 1, sizeof(*out->contents));
   if (!out->contents)
      return false;
   out->count = count;
   for (i = 0; i < count; i++)
   {
      const uint8_t *e = tmd + TMD_HEADER_SIZE + (size_t)i * TMD_CONTENT_SIZE;
      fst_tmd_content_t *c = &out->contents[i];
      c->id = be32(e);
      c->index = be16(e + 4);
      c->type = be16(e + 6);
      c->size = ((uint64_t)be32(e + 8) << 32) | be32(e + 12);
      memcpy(c->hash, e + 16, 32);
   }
   return true;
}

void fst_tmd_free(fst_tmd_t *tmd)
{
   if (!tmd)
      return;
   free(tmd->contents);
   memset(tmd, 0, sizeof(*tmd));
}

fst_volume_t *fst_title_open(const fst_tmd_t *tmd, const uint8_t *title_key,
      const char *const *paths, int *why)
{
   fst_volume_content_t *contents;
   fst_volume_t *vol;
   fst_source_t *src;
   uint16_t *clusters;
   uint64_t fst_size = 0;
   bool has_fst = false;
   uint16_t i;
   int dummy;
   if (!why)
      why = &dummy;
   *why = FST_TITLE_NO_FILES;
   if (!tmd || !title_key || !paths)
      return NULL;
   clusters = (uint16_t*)calloc(tmd->count ? tmd->count : 1, sizeof(*clusters));
   contents = (fst_volume_content_t*)calloc(tmd->count ? tmd->count : 1, sizeof(*contents));
   if (!clusters || !contents)
   {
      free(clusters);
      free(contents);
      return NULL;
   }
   /* each content is a cluster: its file, and what a raw one is hashed to.
    * The FST is all of cluster 0. */
   for (i = 0; i < tmd->count; i++)
   {
      const fst_tmd_content_t *c = &tmd->contents[i];
      clusters[i] = c->index;
      contents[i].size = c->size;
      contents[i].sha1 = (c->type & FST_TMD_SHA1) != 0;
      memcpy(contents[i].hash, c->hash, sizeof(contents[i].hash));
      if (c->index == 0)
      {
         has_fst = true;
         fst_size = c->size;
      }
   }
   src = fst_source_open_app(paths, clusters, tmd->count, 0);
   free(clusters);
   if (!src)
   {
      free(contents);
      return NULL;
   }
   if (!has_fst || fst_size > 0xFFFFFFFFUL)
   {
      fst_source_close(src);
      free(contents);
      *why = has_fst ? FST_TITLE_BAD_FST : FST_TITLE_NO_FST;
      return NULL;
   }
   vol = fst_volume_open(src, true, 0, (uint32_t)fst_size, title_key, contents, tmd->count, NULL);
   free(contents);
   *why = vol ? FST_TITLE_OK : FST_TITLE_BAD_FST;
   return vol;
}
