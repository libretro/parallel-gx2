/* A Wii U eTicket (src/Cemu/ncrypto/eticket.c), as nn_aoc reads a DLC's
 * title.tik for the contents it grants: the header's fields, rights from
 * every entry of every rights section, entries that overwrite one another
 * and ones out of range passed over, and tickets that are cut short or
 * point past their end refused - one with an offset near 4 GiB once wrapped
 * around to read outside the file, one whose rights entry lay past the end
 * once crashed. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include "Cemu/ncrypto/eticket.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint8_t s_tik[4096];
static eticket_t s_t;

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* a DLC ticket, version 1, with one rights section of count entries at
 * section_at (from 0x2A4) */
static size_t dlc_ticket(uint32_t count, uint32_t entries_at)
{
   memset(s_tik, 0, sizeof(s_tik));
   s_tik[0x1BC] = 1;
   put32(s_tik + 0x1D0, 0x00010203);
   put32(s_tik + 0x1D4, 0x04050607);
   put32(s_tik + 0x1DC, 0x0005000C);
   put32(s_tik + 0x1E0, 0x10101A00);
   put16(s_tik + 0x1E6, 0x0020);
   /* the extension header: a table of one section at +0x10 */
   put32(s_tik + 0x2A4 + 8, 0x10);
   put16(s_tik + 0x2A4 + 12, 1);
   put16(s_tik + 0x2A4 + 14, 0x14);
   put32(s_tik + 0x2A4 + 0x10, entries_at);
   put32(s_tik + 0x2A4 + 0x10 + 4, count);
   put32(s_tik + 0x2A4 + 0x10 + 8, 0x84);
   put16(s_tik + 0x2A4 + 0x10 + 16, 3);
   return 0x2A4 + entries_at + count * 0x84;
}

static uint8_t *entry(uint32_t entries_at, uint32_t i)
{
   return s_tik + 0x2A4 + entries_at + i * 0x84;
}

static void test_header(void)
{
   size_t n = dlc_ticket(0, 0x30);
   s_tik[0x1BC] = 0;
   put32(s_tik + 0x1D8, 0x11223344);
   CHECK(eticket_parse(s_tik, 0x220, &s_t) == ETICKET_OK);
   CHECK(s_t.title_id == 0x0005000C10101A00ULL);
   CHECK(s_t.ticket_id == 0x0001020304050607ULL);
   CHECK(s_t.title_version == 0x20 && s_t.format_version == 0);
   CHECK(s_t.device_id == 0x11223344);
   CHECK(s_t.rights_count == 0 && !eticket_right(&s_t, 0));
   (void)n;
   CHECK(eticket_parse(s_tik, 0x21F, &s_t) == ETICKET_TOO_SHORT);
   CHECK(eticket_parse(s_tik, 0, &s_t) == ETICKET_TOO_SHORT);
   put32(s_tik + 0x1DC, 0x00010000);
   CHECK(eticket_parse(s_tik, 0x220, &s_t) == ETICKET_NOT_WIIU);
}

static void test_rights(void)
{
   uint32_t i;
   size_t n = dlc_ticket(1, 0x30);
   uint8_t *e = entry(0x30, 0);
   /* 0, 3, 9 and the last of 1024 */
   e[4] = 0x09;
   e[5] = 0x02;
   e[4 + 127] = 0x80;
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK);
   CHECK(s_t.rights_count == 1024 && s_t.warnings == 0);
   CHECK(eticket_right(&s_t, 0) && eticket_right(&s_t, 3) && eticket_right(&s_t, 9) && eticket_right(&s_t, 1023));
   CHECK(!eticket_right(&s_t, 1) && !eticket_right(&s_t, 8) && !eticket_right(&s_t, 1022));
   CHECK(!eticket_right(&s_t, 1024) && !eticket_right(&s_t, 0xFFFFFFFF));
   /* a rights section in a ticket that is not DLC's, or not version 1: none */
   s_tik[0x1BC] = 0;
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK && s_t.rights_count == 0);
   s_tik[0x1BC] = 2;
   put32(s_tik + 0x1DC, 0x00050000);
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK && s_t.rights_count == 0);

   /* three entries: from 0, from 0x400, from 0 again - which wins */
   n = dlc_ticket(3, 0x30);
   entry(0x30, 0)[4] = 0xFF;
   put32(entry(0x30, 1), 0x400);
   entry(0x30, 1)[4 + 1] = 0x01;
   entry(0x30, 2)[4] = 0x02;
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK);
   CHECK(s_t.rights_count == 0x800);
   CHECK(!eticket_right(&s_t, 0) && eticket_right(&s_t, 1) && !eticket_right(&s_t, 2));
   CHECK(eticket_right(&s_t, 0x408) && !eticket_right(&s_t, 0x400));
   for (i = 0x410; i < 0x800; i++)
      CHECK(!eticket_right(&s_t, i));

   /* the highest base there can be, and one past it: passed over */
   n = dlc_ticket(2, 0x30);
   put32(entry(0x30, 0), 0x1000);
   entry(0x30, 0)[4 + 127] = 0x80;
   put32(entry(0x30, 1), 0x1001);
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK);
   CHECK(s_t.rights_count == ETICKET_RIGHTS_MAX && eticket_right(&s_t, ETICKET_RIGHTS_MAX - 1));
   CHECK(!eticket_right(&s_t, ETICKET_RIGHTS_MAX));
   CHECK(s_t.warnings == ETICKET_SKIPPED_RIGHTS);

   /* a section of another type is not read */
   n = dlc_ticket(1, 0x30);
   put16(s_tik + 0x2A4 + 0x10 + 16, 4);
   put32(s_tik + 0x2A4 + 0x10 + 8, 7);
   entry(0x30, 0)[4] = 0xFF;
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK && s_t.rights_count == 0);
}

static void test_refused(void)
{
   size_t n = dlc_ticket(1, 0x30);
   /* the entry cut short, then gone */
   CHECK(eticket_parse(s_tik, n - 1, &s_t) == ETICKET_TRUNCATED);
   CHECK(eticket_parse(s_tik, 0x2A4 + 0x30, &s_t) == ETICKET_TRUNCATED);
   /* the extension header and the section table past the end */
   CHECK(eticket_parse(s_tik, 0x2A4 + 8, &s_t) == ETICKET_TRUNCATED);
   CHECK(eticket_parse(s_tik, 0x2A4 + 0x20, &s_t) == ETICKET_TRUNCATED);
   /* offsets that wrap a 32-bit sum back into the file */
   n = dlc_ticket(1, 0x30);
   put32(s_tik + 0x2A4 + 0x10, 0xFFFFFFF0);
   CHECK(eticket_parse(s_tik, sizeof(s_tik), &s_t) == ETICKET_TRUNCATED);
   n = dlc_ticket(1, 0x30);
   put32(s_tik + 0x2A4 + 8, 0xFFFFFF80);
   CHECK(eticket_parse(s_tik, sizeof(s_tik), &s_t) == ETICKET_TRUNCATED);
   /* a count far past what the file holds */
   n = dlc_ticket(1, 0x30);
   put32(s_tik + 0x2A4 + 0x10 + 4, 0x7FFFFFFF);
   CHECK(eticket_parse(s_tik, sizeof(s_tik), &s_t) == ETICKET_TRUNCATED);
   /* rights entries of another size */
   n = dlc_ticket(1, 0x30);
   put32(s_tik + 0x2A4 + 0x10 + 8, 0x80);
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_BAD_RIGHTS_SIZE);
   /* a stride of 0: the one section, read again for each in the table */
   n = dlc_ticket(1, 0x30);
   put16(s_tik + 0x2A4 + 12, 3);
   put16(s_tik + 0x2A4 + 14, 0);
   entry(0x30, 0)[4] = 0x04;
   CHECK(eticket_parse(s_tik, n, &s_t) == ETICKET_OK && eticket_right(&s_t, 2) && s_t.rights_count == 1024);
}

int main(void)
{
   test_header();
   test_rights();
   test_refused();
   if (failures)
   {
      fprintf(stderr, "eticket_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("eticket_test: all passed\n");
   return 0;
}
