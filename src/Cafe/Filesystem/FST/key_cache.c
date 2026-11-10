/* Disc keys (key_cache.h) */

#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>

#include "Common/shared_file.h"
#include "util/crypto/aes128.h"
#include "key_cache.h"

/* other .key files beside an image tried at most: plenty, in a folder of
 * many */
#define KEY_CACHE_BESIDE_MAX 64

static const uint8_t key_cache_example[KEY_CACHE_KEY_SIZE] = {
   0x54, 0x1b, 0x98, 0x89, 0x51, 0x9b, 0x27, 0xd3,
   0x63, 0xcd, 0x21, 0x60, 0x4b, 0x97, 0xc6, 0x7a};

static int key_cache_hex(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* 32 hex digits into a key */
static bool key_cache_parse_hex(const char *hex, uint8_t *key)
{
   unsigned i;
   for (i = 0; i < KEY_CACHE_KEY_SIZE; i++)
   {
      int hi = key_cache_hex(hex[2 * i]);
      int lo = key_cache_hex(hex[2 * i + 1]);
      if (hi < 0 || lo < 0)
         return false;
      key[i] = (uint8_t)(hi << 4 | lo);
   }
   return true;
}

/* --- keys.txt ---------------------------------------------------------- */

static bool key_set_add(key_set_t *set, const uint8_t *key, size_t *cap)
{
   if (set->count == *cap)
   {
      size_t grown = *cap ? *cap * 2 : 16;
      uint8_t (*keys)[KEY_CACHE_KEY_SIZE] = (uint8_t (*)[KEY_CACHE_KEY_SIZE])
         realloc(set->keys, grown * KEY_CACHE_KEY_SIZE);
      if (!keys)
         return false;
      set->keys = keys;
      *cap = grown;
   }
   memcpy(set->keys[set->count++], key, KEY_CACHE_KEY_SIZE);
   if (memcmp(key, key_cache_example, KEY_CACHE_KEY_SIZE) != 0)
      set->keys_txt++;
   return true;
}

bool key_set_parse(key_set_t *set, const char *text, size_t len,
      key_cache_bad_line_t bad, void *ud, unsigned *lines)
{
   size_t cap = 0;
   size_t at = 0;
   unsigned line = 0;
   set->keys = NULL;
   set->count = 0;
   set->keys_txt = 0;
   while (at < len)
   {
      /* a line's digits, what it says before a comment, gathered; a line
       * longer than a key and its formatting is no key either way */
      char digits[40];
      size_t n = 0;
      bool comment = false, hex = true;
      line++;
      for (; at < len && text[at] != '\n'; at++)
      {
         const char c = text[at];
         if (comment)
            continue;
         if (c == '#' || c == ';')
            comment = true;
         else if (c == ' ' || c == '\t' || c == '-' || c == '_' || c == '\r')
            continue;
         else
         {
            if (key_cache_hex(c) < 0)
               hex = false;
            if (n < sizeof(digits))
               digits[n] = c;
            n++;
         }
      }
      at++; /* past the '\n' */
      if (n == 0)
         continue;
      if (!hex)
      {
         if (bad)
            bad(ud, line, KEY_CACHE_LINE_NOT_HEX);
      }
      else if (n == 2 * KEY_CACHE_KEY_SIZE)
      {
         uint8_t key[KEY_CACHE_KEY_SIZE];
         key_cache_parse_hex(digits, key);
         if (!key_set_add(set, key, &cap))
         {
            key_set_free(set);
            return false;
         }
      }
      else if (bad)
         bad(ud, line, KEY_CACHE_LINE_NOT_A_KEY);
   }
   if (lines)
      *lines = line;
   return true;
}

void key_set_free(key_set_t *set)
{
   free(set->keys);
   set->keys = NULL;
   set->count = 0;
   set->keys_txt = 0;
}

bool key_set_load(key_set_t *set, const char *path,
      key_cache_bad_line_t bad, void *ud, unsigned *lines)
{
   bool ok;
   char *copy = NULL;
   const char *text;
   uint64_t size;
   shared_file_t *file = shared_file_open(path, 0);
   set->keys = NULL;
   set->count = 0;
   set->keys_txt = 0;
   if (lines)
      *lines = 0;
   if (!file)
      return false;
   /* read where it lies; copied only where it cannot be mapped */
   size = shared_file_size(file);
   text = (const char*)shared_file_mapped(file);
   if (!text && size)
   {
      copy = size < 0x7FFFFFFFUL ? (char*)malloc((size_t)size) : NULL;
      if (!copy || shared_file_read_at(file, copy, (int64_t)size, 0) != (int64_t)size)
      {
         free(copy);
         shared_file_close(file);
         return false;
      }
      text = copy;
   }
   ok = key_set_parse(set, text ? text : "", (size_t)size, bad, ud, lines);
   free(copy);
   shared_file_close(file);
   return ok;
}

bool key_cache_write_template(const char *path)
{
   static const char text[] =
      "# this file contains keys needed for decryption of disc file system data (WUD/WUX)\r\n"
      "# 1 key per line, any text after a '#' character is considered a comment\r\n"
      "# the emulator will automatically pick the right key\r\n"
      "541b9889519b27d363cd21604b97c67a # example key (can be deleted)\r\n";
   return filestream_write_file(path, text, (int64_t)(sizeof(text) - 1));
}

/* --- a key in a file of its own --------------------------------------- */

int key_cache_read_key_file(const char *path, uint8_t *key_out)
{
   char buf[512];
   char hex[2 * KEY_CACHE_KEY_SIZE];
   size_t n = 0;
   int64_t size, got;
   RFILE *file;
   if (!path || !path_is_valid(path) || path_is_directory(path))
      return KEY_CACHE_FILE_ABSENT;
   file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return KEY_CACHE_FILE_ABSENT;
   size = filestream_get_size(file);
   if (size == KEY_CACHE_KEY_SIZE)
   {
      /* the key's octets as they are */
      got = filestream_read(file, key_out, KEY_CACHE_KEY_SIZE);
      filestream_close(file);
      return got == KEY_CACHE_KEY_SIZE ? KEY_CACHE_FILE_KEY : KEY_CACHE_FILE_NOT_A_KEY;
   }
   /* 32 hex digits, with white space anywhere around and between */
   while ((got = filestream_read(file, buf, sizeof(buf))) > 0 && n <= sizeof(hex))
   {
      int64_t i;
      for (i = 0; i < got && n <= sizeof(hex); i++)
      {
         const char c = buf[i];
         if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f')
            continue;
         if (n < sizeof(hex))
            hex[n] = c;
         n++;
      }
   }
   filestream_close(file);
   if (n != sizeof(hex) || !key_cache_parse_hex(hex, key_out))
      return KEY_CACHE_FILE_NOT_A_KEY;
   return KEY_CACHE_FILE_KEY;
}

bool key_cache_fits(const uint8_t *key, const uint8_t *header48)
{
   uint8_t decrypted[32];
   unsigned i;
   AES128_CBC_decrypt(decrypted, header48 + 16, sizeof(decrypted), key, header48);
   for (i = 0; i < sizeof(decrypted); i++)
      if (decrypted[i])
         return false;
   return true;
}

/* --- keys beside an image --------------------------------------------- */

static bool key_cache_is_key_name(const char *name)
{
   const size_t len = strlen(name);
   const char *ext;
   if (len <= 4)
      return false;
   ext = name + len - 4;
   return ext[0] == '.'
      && (ext[1] == 'k' || ext[1] == 'K')
      && (ext[2] == 'e' || ext[2] == 'E')
      && (ext[3] == 'y' || ext[3] == 'Y');
}

static void key_cache_try(const char *path, const char *name,
      uint8_t (*keys)[KEY_CACHE_KEY_SIZE], size_t max, size_t *found,
      key_cache_tried_t tried, void *ud)
{
   uint8_t key[KEY_CACHE_KEY_SIZE];
   const int what = key_cache_read_key_file(path, key);
   if (what == KEY_CACHE_FILE_ABSENT)
      return;
   if (what == KEY_CACHE_FILE_KEY && *found < max)
      memcpy(keys[(*found)++], key, KEY_CACHE_KEY_SIZE);
   if (tried)
      tried(ud, name, what);
}

size_t key_cache_keys_beside(const char *image, uint8_t (*keys)[KEY_CACHE_KEY_SIZE], size_t max,
      key_cache_tried_t tried, void *ud)
{
   size_t found = 0;
   size_t image_len;
   char *own, *folder, *other;
   const char *own_name;
   struct RDIR *dir;
   if (!image)
      return 0;
   image_len = strlen(image);
   /* the paths made here: the image's own .key, its folder, and each other
    * key file in it - made once, at the load of a title */
   own = (char*)malloc(image_len + 5);
   folder = (char*)malloc(image_len + 3);
   other = (char*)malloc(image_len + 4096);
   if (!own || !folder || !other)
   {
      free(own);
      free(folder);
      free(other);
      return 0;
   }
   memcpy(own, image, image_len + 1);
   /* the image's name with .key in place of its extension */
   {
      char *slash = strrchr(own, '/');
      char *dot = strrchr(own, '.');
#ifdef _WIN32
      char *bslash = strrchr(own, '\\');
      if (bslash && (!slash || bslash > slash))
         slash = bslash;
#endif
      if (dot && (!slash || dot > slash + 1))
         *dot = '\0';
      strcat(own, ".key");
   }
   own_name = path_basename(own);
   key_cache_try(own, own_name, keys, max, &found, tried, ud);

   fill_pathname_basedir(folder, image, image_len + 3);
   dir = retro_opendir_include_hidden(folder, true);
   if (dir)
   {
      unsigned others = 0;
      while (others < KEY_CACHE_BESIDE_MAX && retro_readdir(dir))
      {
         const char *name = retro_dirent_get_name(dir);
         if (!name || !key_cache_is_key_name(name) || strcmp(name, own_name) == 0)
            continue;
         others++;
         if (strlen(folder) + strlen(name) + 2 > image_len + 4096)
            continue;
         fill_pathname_join(other, folder, name, image_len + 4096);
         key_cache_try(other, name, keys, max, &found, tried, ud);
      }
      retro_closedir(dir);
   }
   free(own);
   free(folder);
   free(other);
   return found;
}
