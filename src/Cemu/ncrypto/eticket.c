/* A Wii U eTicket (eticket.h) */

#include <string.h>

#include "eticket.h"

#define HEADER_SIZE 0x220
#define EXT_AT 0x2A4
#define EXT_SIZE 0x10
#define SECTION_SIZE 0x14
#define SECTION_CONTENT_RIGHTS 3
#define RIGHTS_ENTRY_SIZE 0x84

static uint32_t be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
   return (uint16_t)((p[0] << 8) | p[1]);
}

/* the octets at offset, len of them, where the file has them all: in 64
 * bits, so an offset near 4 GiB does not wrap back into the file */
static const uint8_t *at(const uint8_t *data, size_t size, uint64_t offset, uint64_t len)
{
   if (offset > size || len > (uint64_t)size - offset)
      return NULL;
   return data + (size_t)offset;
}

static void set_right(eticket_t *t, uint32_t index, bool on)
{
   if (on)
      t->rights[index / 8] |= (uint8_t)(1u << (index % 8));
   else
      t->rights[index / 8] &= (uint8_t)~(1u << (index % 8));
}

static enum eticket_error read_rights(const uint8_t *data, size_t size, eticket_t *t)
{
   const uint8_t *ext = at(data, size, EXT_AT, EXT_SIZE);
   uint32_t table, i;
   uint16_t sections, stride;
   if (!ext)
      return ETICKET_TRUNCATED;
   table = be32(ext + 8);
   sections = be16(ext + 12);
   stride = be16(ext + 14);
   for (i = 0; i < sections; i++)
   {
      const uint8_t *s = at(data, size, (uint64_t)EXT_AT + table + (uint64_t)i * stride, SECTION_SIZE);
      uint32_t offset, count, entry_size, r;
      if (!s)
         return ETICKET_TRUNCATED;
      if (be16(s + 16) != SECTION_CONTENT_RIGHTS)
         continue;
      offset = be32(s);
      count = be32(s + 4);
      entry_size = be32(s + 8);
      if (entry_size != RIGHTS_ENTRY_SIZE)
         return ETICKET_BAD_RIGHTS_SIZE;
      for (r = 0; r < count; r++)
      {
         const uint8_t *e = at(data, size, (uint64_t)EXT_AT + offset + (uint64_t)r * RIGHTS_ENTRY_SIZE,
               RIGHTS_ENTRY_SIZE);
         uint32_t base, x;
         if (!e)
            return ETICKET_TRUNCATED;
         base = be32(e);
         if (base > ETICKET_RIGHTS_BASE_MAX)
         {
            t->warnings |= ETICKET_SKIPPED_RIGHTS;
            continue;
         }
         if (base + ETICKET_RIGHTS_PER_ENTRY > t->rights_count)
            t->rights_count = base + ETICKET_RIGHTS_PER_ENTRY;
         for (x = 0; x < ETICKET_RIGHTS_PER_ENTRY; x++)
            set_right(t, base + x, ((e[4 + x / 8] >> (x % 8)) & 1) != 0);
      }
   }
   return ETICKET_OK;
}

enum eticket_error eticket_parse(const uint8_t *data, size_t size, eticket_t *out)
{
   uint32_t high;
   memset(out, 0, sizeof(*out));
   if (size < HEADER_SIZE)
      return ETICKET_TOO_SHORT;
   high = be32(data + 0x1DC);
   out->title_id = ((uint64_t)high << 32) | be32(data + 0x1E0);
   out->ticket_id = ((uint64_t)be32(data + 0x1D0) << 32) | be32(data + 0x1D4);
   out->format_version = data[0x1BC];
   out->title_version = be16(data + 0x1E6);
   out->device_id = be32(data + 0x1D8);
   if ((high >> 16) != 0x5)
      return ETICKET_NOT_WIIU;
   /* rights only in DLC's version 1 tickets */
   if (out->format_version >= 1 && high == 0x0005000C)
      return read_rights(data, size, out);
   return ETICKET_OK;
}

bool eticket_right(const eticket_t *t, uint32_t index)
{
   if (index >= t->rights_count)
      return false;
   return ((t->rights[index / 8] >> (index % 8)) & 1) != 0;
}
