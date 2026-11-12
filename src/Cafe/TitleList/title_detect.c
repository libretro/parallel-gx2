/* What a file or folder is (title_detect.h) */

#include <string.h>

#include <streams/file_stream.h>

#include "title_detect.h"

static char lower(char c)
{
   return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool same_ignoring_case(const char *a, const char *b, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
      if (lower(a[i]) != lower(b[i]))
         return false;
   return true;
}

static bool ends_with(const char *s, const char *suffix)
{
   size_t n = strlen(s), m = strlen(suffix);
   return n >= m && same_ignoring_case(s + n - m, suffix, m);
}

static bool equals(const char *s, const char *t)
{
   size_t n = strlen(s);
   return n == strlen(t) && same_ignoring_case(s, t, n);
}

static int hex_digit(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   c = lower(c);
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   return -1;
}

enum title_file_type title_file_type(const char *path)
{
   static const uint8_t wux[8] = { 0x57, 0x55, 0x58, 0x30, 0x2E, 0xD0, 0x99, 0x10 };
   static const uint8_t rpx[9] = { 0x7F, 0x45, 0x4C, 0x46, 0x01, 0x02, 0x01, 0xCA, 0xFE };
   static const uint8_t elf[9] = { 0x7F, 0x45, 0x4C, 0x46, 0x01, 0x02, 0x01, 0x00, 0x00 };
   static const uint8_t wud1[4] = { 0x57, 0x55, 0x50, 0x2D }; /* "WUP-" */
   static const uint8_t wud2[4] = { 0xCC, 0x54, 0x9E, 0xB9 };
   enum title_file_type type = TITLE_FILE_UNKNOWN;
   uint8_t head[32], at64k[4];
   int64_t size;
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!f)
      return TITLE_FILE_UNKNOWN;
   size = filestream_get_size(f);
   if (size >= 32 && filestream_read(f, head, sizeof(head)) == (int64_t)sizeof(head))
   {
      if (!memcmp(head, wux, sizeof(wux)))
         type = TITLE_FILE_WUX;
      else if (!memcmp(head, rpx, sizeof(rpx)))
         type = TITLE_FILE_RPX;
      else if (!memcmp(head, elf, sizeof(elf)))
         type = TITLE_FILE_ELF;
      else if (size >= 0x10000 && !memcmp(head, wud1, sizeof(wud1))
            && filestream_seek(f, 0x10000, RETRO_VFS_SEEK_POSITION_START) == 0
            && filestream_read(f, at64k, sizeof(at64k)) == (int64_t)sizeof(at64k)
            && !memcmp(at64k, wud2, sizeof(wud2)))
         type = TITLE_FILE_WUD;
   }
   filestream_close(f);
   return type;
}

enum title_name_kind title_name_kind(const char *filename)
{
   if (ends_with(filename, ".rpx"))
      return TITLE_NAME_RPX;
   if (ends_with(filename, ".wud") || ends_with(filename, ".wux") || ends_with(filename, ".iso"))
      return TITLE_NAME_DISC;
   if (equals(filename, "title.tmd"))
      return TITLE_NAME_TMD;
   if (ends_with(filename, ".wua"))
      return TITLE_NAME_WUA;
   if (ends_with(filename, ".wuhb"))
      return TITLE_NAME_WUHB;
   return TITLE_NAME_OTHER;
}

bool title_scan_wanted(const char *filename)
{
   /* the extension as std::filesystem has it: from the last dot, unless
    * that dot is the name's first character (".." has "." for one, which
    * is none of these) */
   const char *dot = strrchr(filename, '.');
   if (!dot || dot == filename)
      return false;
   if (equals(dot, ".tmd"))
      return equals(filename, "title.tmd");
   return equals(dot, ".wud") || equals(dot, ".wux") || equals(dot, ".iso")
      || equals(dot, ".wua") || equals(dot, ".wuhb");
}

bool title_scan_mlc_folder(const char *name)
{
   size_t i;
   if (strlen(name) != 8)
      return false;
   for (i = 0; i < 8; i++)
      if (hex_digit(name[i]) < 0)
         return false;
   return true;
}

bool title_wua_folder(const char *name, size_t len, uint64_t *title_id,
      uint16_t *version)
{
   uint64_t id = 0;
   uint32_t v = 0;
   size_t i;
   if (len < 16 + 2)
      return false;
   for (i = 0; i < 16; i++)
   {
      int d = hex_digit(name[i]);
      if (d < 0)
         return false;
      id = (id << 4) | (unsigned)d;
   }
   if (name[16] != '_' || name[17] != 'v')
      return false;
   name += 18;
   len -= 18;
   /* decimal, no leading zero */
   if (!len || (name[0] == '0' && len != 1))
      return false;
   for (i = 0; i < len; i++)
   {
      if (name[i] < '0' || name[i] > '9')
         return false;
      v = v * 10 + (uint32_t)(name[i] - '0');
      if (v > 0xFFFF)
         return false;
   }
   *title_id = id;
   *version = (uint16_t)v;
   return true;
}
