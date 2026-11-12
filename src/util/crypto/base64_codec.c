/* Base64 (base64_codec.h) */

#include "base64_codec.h"

static const char s_charset[] =
   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
   "abcdefghijklmnopqrstuvwxyz"
   "0123456789+/";

size_t b64_encode(char *out, const void *in, size_t len)
{
   const uint8_t *p = (const uint8_t*)in;
   size_t o = 0;
   while (len >= 3)
   {
      out[o++] = s_charset[p[0] >> 2];
      out[o++] = s_charset[((p[0] & 0x03) << 4) | (p[1] >> 4)];
      out[o++] = s_charset[((p[1] & 0x0F) << 2) | (p[2] >> 6)];
      out[o++] = s_charset[p[2] & 0x3F];
      p += 3;
      len -= 3;
   }
   if (len)
   {
      const uint8_t b1 = len > 1 ? p[1] : 0;
      out[o++] = s_charset[p[0] >> 2];
      out[o++] = s_charset[((p[0] & 0x03) << 4) | (b1 >> 4)];
      out[o++] = len > 1 ? s_charset[(b1 & 0x0F) << 2] : '=';
      out[o++] = '=';
   }
   return o;
}

/* 64 for what is not base64: shifted into the triple as it is, as it
 * always was */
static unsigned value(char c)
{
   if (c >= 'A' && c <= 'Z')
      return (unsigned)(c - 'A');
   if (c >= 'a' && c <= 'z')
      return (unsigned)(c - 'a') + 26;
   if (c >= '0' && c <= '9')
      return (unsigned)(c - '0') + 52;
   if (c == '+')
      return 62;
   if (c == '/')
      return 63;
   if (c == '=')
      return 0;
   return 64;
}

size_t b64_decoded_size(const char *in, size_t len)
{
   size_t n;
   if (len <= 3 || (len & 3) != 0)
      return 0;
   n = len / 4 * 3;
   if (in[len - 1] == '=')
      n--;
   if (in[len - 2] == '=')
      n--;
   return n;
}

size_t b64_decode(uint8_t *out, const char *in, size_t len)
{
   const size_t n = b64_decoded_size(in, len);
   size_t i, j = 0;
   for (i = 0; i < len && n; i += 4)
   {
      const uint32_t triple = ((uint32_t)value(in[i]) << 18) + ((uint32_t)value(in[i + 1]) << 12)
         + ((uint32_t)value(in[i + 2]) << 6) + (uint32_t)value(in[i + 3]);
      if (j < n)
         out[j++] = (uint8_t)(triple >> 16);
      if (j < n)
         out[j++] = (uint8_t)(triple >> 8);
      if (j < n)
         out[j++] = (uint8_t)triple;
   }
   return j;
}
