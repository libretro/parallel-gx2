/* Wii U disc images, .wud and .wux (wud.h) */

#include <stdlib.h>
#include <string.h>

#include "Common/shared_file.h"
#include "wud.h"

/* "WUX0", then a number, at the start of a .wux */
#define WUX_MAGIC_0 0x30585557UL
#define WUX_MAGIC_1 0x1099d02eUL
/* the header: the magics, the sector size, the image's size, flags */
#define WUX_HEADER_SIZE 32

struct wud
{
   shared_file_t *file;
   const uint8_t *mapped; /* the file's octets, where mapped */
   uint64_t file_size;
   int64_t image_size;
   /* .wux: the sector each of the image's sectors is stored as */
   const uint32_t *index_table; /* in the mapping, or index_table_copy */
   uint32_t *index_table_copy;
   uint64_t offset_sector_array;
   uint32_t sector_size;
   uint32_t index_entries;
   bool compressed;
};

static uint32_t wud_get_le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

wud_t *wud_open(const char *path, unsigned flags)
{
   uint8_t header[WUX_HEADER_SIZE];
   wud_t *wud;
   /* mapped where it can be: then a read is one copy from the pages, and
    * the FST decrypts straight from them (wud_lend) */
   shared_file_t *file = shared_file_open(path,
         (flags & WUD_OPEN_NO_MAP) ? SHARED_FILE_NO_MAP : 0);
   if (!file)
      return NULL;
   wud = (wud_t*)calloc(1, sizeof(*wud));
   if (!wud)
   {
      shared_file_close(file);
      return NULL;
   }
   wud->file = file;
   wud->mapped = shared_file_mapped(file);
   wud->file_size = shared_file_size(file);
   if (shared_file_read_at(file, header, WUX_HEADER_SIZE, 0) != WUX_HEADER_SIZE)
      goto fail; /* too short to be either */

   if (     wud_get_le32(header) == WUX_MAGIC_0
         && wud_get_le32(header + 4) == WUX_MAGIC_1)
   {
      uint64_t image_size = (uint64_t)wud_get_le32(header + 16)
         | ((uint64_t)wud_get_le32(header + 20) << 32);
      uint64_t offset_sector_array;
      wud->compressed = true;
      wud->sector_size = wud_get_le32(header + 8);
      if (wud->sector_size < 0x100 || wud->sector_size >= 0x10000000)
         goto fail;
      /* more sectors than the table can count: the count would wrap, and
       * the table be read past its end */
      if (image_size == 0 || image_size / wud->sector_size >= 0xFFFFFFFFUL)
         goto fail;
      wud->image_size = (int64_t)image_size;
      wud->index_entries = (uint32_t)((image_size + wud->sector_size - 1) / wud->sector_size);
      /* the table after the header, the sectors from the next whole sector */
      offset_sector_array = WUX_HEADER_SIZE + (uint64_t)wud->index_entries * 4;
      offset_sector_array = (offset_sector_array + wud->sector_size - 1)
         / wud->sector_size * wud->sector_size;
      if (offset_sector_array > wud->file_size)
         goto fail; /* too short for its table */
      wud->offset_sector_array = offset_sector_array;
      if (wud->mapped)
         /* read where it lies: aligned, past a header of 32 octets. The
          * entries are little-endian, as every host the core runs on. */
         wud->index_table = (const uint32_t*)(wud->mapped + WUX_HEADER_SIZE);
      else
      {
         const int64_t table_size = (int64_t)wud->index_entries * 4;
         wud->index_table_copy = (uint32_t*)malloc((size_t)table_size);
         if (     !wud->index_table_copy
               || shared_file_read_at(file, wud->index_table_copy, table_size, WUX_HEADER_SIZE) != table_size)
            goto fail;
         wud->index_table = wud->index_table_copy;
      }
   }
   else
      wud->image_size = (int64_t)wud->file_size; /* the disc as it is */
   return wud;

fail:
   wud_close(wud);
   return NULL;
}

void wud_close(wud_t *wud)
{
   if (!wud)
      return;
   shared_file_close(wud->file);
   free(wud->index_table_copy);
   free(wud);
}

bool wud_isWUXCompressed(const wud_t *wud)
{
   return wud->compressed;
}

bool wud_isMapped(const wud_t *wud)
{
   return wud->mapped != NULL;
}

unsigned wud_streamsOpened(const wud_t *wud)
{
   return shared_file_streams_opened(wud->file);
}

/* Where the image's octet at offset lies in the file, and how many follow it
 * there in one piece (to its sector's end in a .wux); false past the end */
static bool wud_locate(const wud_t *wud, int64_t offset, uint64_t *file_offset, uint64_t *run)
{
   uint64_t left;
   uint32_t sector_offset;
   if (offset < 0 || offset >= wud->image_size)
      return false;
   left = (uint64_t)(wud->image_size - offset);
   if (!wud->compressed)
   {
      *file_offset = (uint64_t)offset;
      *run = left;
      return true;
   }
   sector_offset = (uint32_t)((uint64_t)offset % wud->sector_size);
   *file_offset = wud->offset_sector_array
      + (uint64_t)wud->index_table[(uint64_t)offset / wud->sector_size] * wud->sector_size
      + sector_offset;
   *run = wud->sector_size - sector_offset;
   if (*run > left)
      *run = left;
   return true;
}

const unsigned char *wud_lend(const wud_t *wud, unsigned length, int64_t offset)
{
   uint64_t at, run;
   if (     !wud->mapped
         || !wud_locate(wud, offset, &at, &run)
         || run < length
         || at > wud->file_size
         || wud->file_size - at < length)
      return NULL;
   return wud->mapped + at;
}

/* A run in one piece at a time: the whole of it in a .wud, up to a sector's
 * end in a .wux. Up to 4 GiB at once. */
unsigned wud_readData(wud_t *wud, void *buffer, unsigned length, int64_t offset)
{
   unsigned read_bytes = 0;
   uint8_t *out = (uint8_t*)buffer;
   while (length > 0)
   {
      uint64_t at, run;
      unsigned n;
      int64_t got;
      if (!wud_locate(wud, offset, &at, &run))
         break;
      n = run < length ? (unsigned)run : length;
      got = shared_file_read_at(wud->file, out, n, at);
      if (got > 0)
         read_bytes += (unsigned)got;
      if (got != (int64_t)n)
         break;
      out += n;
      length -= n;
      offset += n;
   }
   return read_bytes;
}

int64_t wud_getWUDSize(const wud_t *wud)
{
   return wud->image_size;
}
