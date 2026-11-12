/* Miiverse's offline posts (olv_offline.h) */

#include <stdlib.h>
#include <string.h>

#include "util/helpers/zlib_stream.h"
#include "olv_offline.h"

bool olv_offline_name(char *out, size_t cap, const char *dir,
      const uint8_t *id, size_t id_cap, const char *ext)
{
   const size_t dir_len = strlen(dir);
   const size_t ext_len = strlen(ext);
   size_t id_len = 0;
   while (id_len < id_cap && id[id_len])
      id_len++;
   if (dir_len + id_len + ext_len >= cap)
      return false;
   memcpy(out, dir, dir_len);
   memcpy(out + dir_len, id, id_len);
   memcpy(out + dir_len + id_len, ext, ext_len + 1);
   return true;
}

static bool lookup(const zarchive_t *ar, const char *name, zar_file_t *file)
{
   return ar && zarchive_lookup(ar, name, strlen(name), file) && !file->dir;
}

bool olv_offline_size(const zarchive_t *ar, const char *name, uint32_t *size)
{
   zar_file_t file;
   if (!lookup(ar, name, &file) || file.size > 0xFFFFFFFFu)
      return false;
   if (size)
      *size = (uint32_t)file.size;
   return true;
}

enum olv_offline_read_result olv_offline_read(const zarchive_t *ar, const char *name,
      void *out, uint32_t room, uint32_t *size)
{
   zar_file_t file;
   *size = 0;
   if (!lookup(ar, name, &file))
      return OLV_OFFLINE_MISSING;
   if (file.size > room)
      return OLV_OFFLINE_TOO_LARGE;
   if (zarchive_read(ar, &file, 0, out, file.size) != file.size)
      return OLV_OFFLINE_FAILED;
   *size = (uint32_t)file.size;
   return OLV_OFFLINE_OK;
}

static const uint8_t s_tga_header[OLV_OFFLINE_TGA_HEADER_SIZE] =
{
   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x40, 0x01, 0x78, 0x00, 0x20, 0x08 /* 320x120, 32 bpp, 8 alpha bits */
};

bool olv_offline_memo(const zarchive_t *ar, const char *name,
      uint8_t *out, uint32_t room, uint32_t *size)
{
   enum { PIXELS = OLV_OFFLINE_MEMO_W * OLV_OFFLINE_MEMO_H };
   const size_t bits_at = OLV_OFFLINE_MEMO_TGA_SIZE - OLV_OFFLINE_MEMO_BITS_SIZE;
   zar_file_t file;
   uint8_t *tga;
   size_t i;
   size_t len = room;
   bool ok;
   if (!lookup(ar, name, &file) || file.size != OLV_OFFLINE_MEMO_BITS_SIZE)
      return false;
   tga = (uint8_t*)malloc(OLV_OFFLINE_MEMO_TGA_SIZE);
   if (!tga)
      return false;
   /* The bits read into the end of the image they become, and the pixels
    * written from the front: the pixel written never reaches a bit not yet
    * read (4 octets a pixel against 1/8 of one, the bits starting at the
    * last 4800 of 153618), so one buffer does for both. */
   if (zarchive_read(ar, &file, 0, tga + bits_at, OLV_OFFLINE_MEMO_BITS_SIZE)
         != OLV_OFFLINE_MEMO_BITS_SIZE)
   {
      free(tga);
      return false;
   }
   memcpy(tga, s_tga_header, sizeof(s_tga_header));
   for (i = 0; i < PIXELS; i++)
   {
      const uint8_t v = (tga[bits_at + i / 8] >> (i % 8)) & 1 ? 0xFF : 0x00;
      uint8_t *p = tga + OLV_OFFLINE_TGA_HEADER_SIZE + i * 4;
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 0xFF;
   }
   ok = zlib_compress(out, &len, tga, OLV_OFFLINE_MEMO_TGA_SIZE, 6) != 0;
   free(tga);
   if (ok)
      *size = (uint32_t)len;
   return ok;
}

uint32_t olv_offline_pick(uint32_t n, uint32_t max, uint32_t seed, uint32_t *pool,
      bool (*take)(void *ud, uint32_t index, uint32_t slot), void *ud)
{
   uint32_t x = seed ? seed : 0x9E3779B9u;
   uint32_t left = n;
   uint32_t taken = 0;
   uint32_t i;
   for (i = 0; i < n; i++)
      pool[i] = i;
   /* each post leaves the pool when it is tried, taken or not: one that
    * could not be taken was tried again and again, for good once only such
    * were left */
   while (left && taken < max)
   {
      uint32_t k, index;
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      k = x % left;
      index = pool[k];
      pool[k] = pool[--left];
      if (take(ud, index, taken))
         taken++;
   }
   return taken;
}
