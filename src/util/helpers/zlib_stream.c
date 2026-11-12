/* zlib streams (zlib_stream.h) */

#include <retro_atomic.h>
#include <encodings/crc32.h>
#include <encodings/deflate.h>

#include "zlib_stream.h"

/* inflaters not in use, each slot NULL or one; taken by exchange, put back
 * by compare-and-swap into an empty slot */
#define INFLATER_POOL 4
static retro_atomic_ptr_t s_inflaters[INFLATER_POOL];

size_t zlib_compress_bound(size_t len)
{
   return len + (len >> 12) + (len >> 14) + (len >> 25) + 13;
}

size_t zlib_compress_room(size_t len)
{
   /* stored blocks at worst: five octets every 16 KiB, header, trailer */
   return len + (len >> 11) + 64;
}

int zlib_compress(uint8_t *dst, size_t *dst_len, const uint8_t *src,
      size_t src_len, int level)
{
   void *z = rdeflate_new(level < 0 ? 6 : level, 15);
   size_t out = 0;
   int ok = 0;
   if (!z)
      return 0;
   rdeflate_set_in(z, src, src_len);
   rdeflate_set_out(z, dst, *dst_len);
   rdeflate_finish(z);
   for (;;)
   {
      size_t wrote = 0;
      int st = rdeflate_process(z, NULL, &wrote);
      out += wrote;
      if (st == RDEFLATE_PROCESS_END)
      {
         ok = 1;
         break;
      }
      if (st != RDEFLATE_PROCESS_NEXT || out == *dst_len)
         break;
      rdeflate_set_out(z, dst + out, *dst_len - out);
   }
   rdeflate_free(z);
   if (ok)
      *dst_len = out;
   return ok;
}

static void *inflater_take(int window_bits)
{
   int i;
   for (i = 0; i < INFLATER_POOL; i++)
   {
      void *z;
      if (!retro_atomic_load_acquire_ptr(&s_inflaters[i]))
         continue;
      if ((z = retro_atomic_exchange_ptr(&s_inflaters[i], NULL)))
      {
         rinflate_reset(z, window_bits);
         return z;
      }
   }
   return rinflate_new(window_bits);
}

static void inflater_give(void *z)
{
   int i;
   for (i = 0; i < INFLATER_POOL; i++)
      if (!retro_atomic_load_acquire_ptr(&s_inflaters[i])
            && retro_atomic_cas_ptr(&s_inflaters[i], NULL, z))
         return;
   rinflate_free(z);
}

enum zlib_result zlib_uncompress(uint8_t *dst, size_t *dst_len,
      const uint8_t *src, size_t *src_len, int window_bits)
{
   enum zlib_result result = ZLIB_DATA_ERROR;
   size_t in = 0, out = 0;
   void *z = inflater_take(window_bits);
   if (!z)
      return ZLIB_OUT_OF_MEMORY;
   rinflate_set_in(z, src, *src_len);
   rinflate_set_out(z, dst, *dst_len);
   for (;;)
   {
      size_t read = 0, wrote = 0;
      int st = rinflate_process(z, &read, &wrote);
      in += read;
      out += wrote;
      if (st == RDEFLATE_PROCESS_END)
      {
         result = ZLIB_OK;
         break;
      }
      if (st == RDEFLATE_PROCESS_ERROR)
         break;
      if (!read && !wrote)
      {
         /* as zlib's uncompress: out of room, or out of input */
         if (out == *dst_len)
            result = ZLIB_BUFFER_TOO_SMALL;
         break;
      }
   }
   inflater_give(z);
   *dst_len = out;
   *src_len = in;
   return result;
}

#ifdef ZLIB_STREAM_TEST_HOOKS
size_t zlib_stream_pooled(void)
{
   size_t n = 0;
   int i;
   for (i = 0; i < INFLATER_POOL; i++)
      n += retro_atomic_load_acquire_ptr(&s_inflaters[i]) != NULL;
   return n;
}
#endif

uint32_t zlib_adler32(uint32_t adler, const uint8_t *data, size_t len)
{
   uint32_t a = adler & 0xFFFF, b = adler >> 16;
   while (len)
   {
      /* 5552 octets is the most that cannot overflow 32 bits (zlib's NMAX) */
      size_t n = len < 5552 ? len : 5552;
      len -= n;
      while (n--)
      {
         a += *data++;
         b += a;
      }
      a %= 65521;
      b %= 65521;
   }
   return (b << 16) | a;
}

uint32_t zlib_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
   return len ? encoding_crc32(crc, data, len) : crc;
}
