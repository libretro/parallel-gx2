/* The core's title detection (src/Cafe/TitleList/title_detect.c): files
 * told by their first octets (.wux, RPX, ELF, a .wud's two magics, short
 * and missing files), names by their ends, the files a scan looks into,
 * an MLC's title folders, and a .wua's title folders. C89, as the C89 lane
 * builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <streams/file_stream.h>

#include "Cafe/TitleList/title_detect.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const char *PATH = "title_detect_test.bin";
static unsigned char buf[0x10010];

static enum title_file_type type_of(const unsigned char *head, size_t head_len, size_t size)
{
   memset(buf, 0x11, sizeof(buf));
   memcpy(buf, head, head_len);
   filestream_write_file(PATH, buf, (int64_t)size);
   return title_file_type(PATH);
}

static void test_file_type(void)
{
   static const unsigned char wux[8] = { 0x57, 0x55, 0x58, 0x30, 0x2E, 0xD0, 0x99, 0x10 };
   static const unsigned char rpx[9] = { 0x7F, 0x45, 0x4C, 0x46, 0x01, 0x02, 0x01, 0xCA, 0xFE };
   static const unsigned char elf[9] = { 0x7F, 0x45, 0x4C, 0x46, 0x01, 0x02, 0x01, 0x00, 0x00 };
   static const unsigned char wup[4] = { 'W', 'U', 'P', '-' };
   static const unsigned char wud2[4] = { 0xCC, 0x54, 0x9E, 0xB9 };
   unsigned char head[9];
   CHECK(type_of(wux, 8, 32) == TITLE_FILE_WUX);
   CHECK(type_of(wux, 8, 31) == TITLE_FILE_UNKNOWN);
   CHECK(type_of(rpx, 9, 100) == TITLE_FILE_RPX);
   CHECK(type_of(elf, 9, 100) == TITLE_FILE_ELF);
   memcpy(head, elf, 9);
   head[8] = 1;
   CHECK(type_of(head, 9, 100) == TITLE_FILE_UNKNOWN);
   memcpy(head, wux, 8);
   head[7] = 0x11;
   CHECK(type_of(head, 8, 100) == TITLE_FILE_UNKNOWN);
   /* a .wud: WUP- first, its magic at 0x10000, 64 KiB at least */
   memset(buf, 0, sizeof(buf));
   memcpy(buf, wup, 4);
   memcpy(buf + 0x10000, wud2, 4);
   filestream_write_file(PATH, buf, 0x10004);
   CHECK(title_file_type(PATH) == TITLE_FILE_WUD);
   buf[0x10003] ^= 1;
   filestream_write_file(PATH, buf, 0x10004);
   CHECK(title_file_type(PATH) == TITLE_FILE_UNKNOWN);
   buf[0x10003] ^= 1;
   buf[3] = '_';
   filestream_write_file(PATH, buf, 0x10004);
   CHECK(title_file_type(PATH) == TITLE_FILE_UNKNOWN);
   /* WUP- in one under 64 KiB: not read further */
   buf[3] = '-';
   filestream_write_file(PATH, buf, 0xFFFF);
   CHECK(title_file_type(PATH) == TITLE_FILE_UNKNOWN);
   filestream_delete(PATH);
   CHECK(title_file_type(PATH) == TITLE_FILE_UNKNOWN);
   CHECK(title_file_type("no/such/dir/title_detect_test.bin") == TITLE_FILE_UNKNOWN);
}

static void test_names(void)
{
   CHECK(title_name_kind("root.rpx") == TITLE_NAME_RPX);
   CHECK(title_name_kind("ROOT.RPX") == TITLE_NAME_RPX);
   CHECK(title_name_kind(".rpx") == TITLE_NAME_RPX);
   CHECK(title_name_kind("game.wud") == TITLE_NAME_DISC);
   CHECK(title_name_kind("game.WUX") == TITLE_NAME_DISC);
   CHECK(title_name_kind("game.iso") == TITLE_NAME_DISC);
   CHECK(title_name_kind("Title.TMD") == TITLE_NAME_TMD);
   CHECK(title_name_kind("other.tmd") == TITLE_NAME_OTHER);
   CHECK(title_name_kind("xtitle.tmd") == TITLE_NAME_OTHER);
   CHECK(title_name_kind("game.Wua") == TITLE_NAME_WUA);
   CHECK(title_name_kind("game.wuhb") == TITLE_NAME_WUHB);
   CHECK(title_name_kind("game.wuh") == TITLE_NAME_OTHER);
   CHECK(title_name_kind("rpx") == TITLE_NAME_OTHER);
   CHECK(title_name_kind("") == TITLE_NAME_OTHER);

   CHECK(title_scan_wanted("game.wud") && title_scan_wanted("GAME.WUX"));
   CHECK(title_scan_wanted("a.b.iso") && title_scan_wanted("x.wua") && title_scan_wanted("x.WUHB"));
   CHECK(title_scan_wanted("title.tmd") && title_scan_wanted("TITLE.TMD"));
   CHECK(!title_scan_wanted("other.tmd"));
   CHECK(!title_scan_wanted("root.rpx"));
   /* a name that only starts with its dot has no extension */
   CHECK(!title_scan_wanted(".wud") && title_scan_wanted("..wud") && title_scan_wanted(".a.wud"));
   CHECK(!title_scan_wanted("wud") && !title_scan_wanted(".") && !title_scan_wanted(".."));
   CHECK(!title_scan_wanted("game.wud.bak") && !title_scan_wanted("game.wudx"));

   CHECK(title_scan_mlc_folder("101c9400") && title_scan_mlc_folder("ABCDEF09"));
   CHECK(!title_scan_mlc_folder("101c940") && !title_scan_mlc_folder("101c94000"));
   CHECK(!title_scan_mlc_folder("101c940g") && !title_scan_mlc_folder(""));
}

static int wua_is(const char *name, uint64_t id, unsigned version)
{
   uint64_t got_id = 1;
   uint16_t got_version = 1;
   return title_wua_folder(name, strlen(name), &got_id, &got_version)
      && got_id == id && got_version == version;
}

static int wua_no(const char *name)
{
   uint64_t id = 7;
   uint16_t v = 7;
   return !title_wua_folder(name, strlen(name), &id, &v) && id == 7 && v == 7;
}

static void test_wua(void)
{
   const uint64_t botw = ((uint64_t)0x00050000UL << 32) | 0x101C9400UL;
   CHECK(wua_is("00050000101C9400_v0", botw, 0));
   CHECK(wua_is("00050000101c9400_v208", botw, 208));
   CHECK(wua_is("00050000101c9400_v65535", botw, 65535));
   CHECK(wua_no("00050000101c9400_v65536"));
   /* too big to hold, not wrapped round to a small one */
   CHECK(wua_no("00050000101c9400_v4294967296"));
   CHECK(wua_no("00050000101c9400_v42949672960"));
   CHECK(wua_no("00050000101c9400_v01") && wua_no("00050000101c9400_v"));
   CHECK(wua_no("00050000101c9400_V1") && wua_no("00050000101c9400-v1"));
   CHECK(wua_no("00050000101c9400_v1a") && wua_no("00050000101c94z0_v1"));
   CHECK(wua_no("0005000010_v1") && wua_no(""));
   /* the length, not a terminator, ends it */
   {
      uint64_t id;
      uint16_t v;
      CHECK(title_wua_folder("00050000101c9400_v12XYZ", 20, &id, &v) && v == 12);
      /* and nothing past it is read (an exact copy, for AddressSanitizer) */
      {
         char *exact = (char*)malloc(17);
         if (exact)
         {
            memcpy(exact, "00050000101c9400_v12", 17);
            CHECK(!title_wua_folder(exact, 17, &id, &v));
            CHECK(!title_wua_folder(exact, 16, &id, &v));
            free(exact);
         }
      }
   }
}

int main(void)
{
   test_file_type();
   test_names();
   test_wua();
   if (failures)
   {
      fprintf(stderr, "title_detect_test: %d failed\n", failures);
      return 1;
   }
   printf("title_detect_test: all passed\n");
   return 0;
}
