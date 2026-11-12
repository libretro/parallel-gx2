/* The core's HMAC-MD5 (src/util/crypto/hmac_md5.c) over libretro-common's
 * MD5, as NEX signs packets and tickets with it: RFC 1321's MD5 and RFC
 * 2202's HMAC-MD5 test vectors, a MAC taken in pieces, and a key over 64
 * octets cut, not hashed. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include "util/crypto/hmac_md5.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int hex_is(const unsigned char *d, const char *hex)
{
   char got[33];
   int i;
   for (i = 0; i < 16; i++)
      sprintf(got + i * 2, "%02x", d[i]);
   return strcmp(got, hex) == 0;
}

static void md5_of(const char *s, unsigned char *d)
{
   MD5_CTX ctx;
   MD5_Init(&ctx);
   MD5_Update(&ctx, s, (unsigned long)strlen(s));
   MD5_Final(d, &ctx);
}

static int hmac_is(const unsigned char *key, int key_len, const unsigned char *data,
      int len, const char *hex)
{
   unsigned char d[16];
   hmacMD5(key, key_len, data, len, d);
   return hex_is(d, hex);
}

int main(void)
{
   unsigned char d[16], d2[16], key[80], data[80];
   HMACMD5Ctx ctx;
   int i;

   md5_of("", d);
   CHECK(hex_is(d, "d41d8cd98f00b204e9800998ecf8427e"));
   md5_of("abc", d);
   CHECK(hex_is(d, "900150983cd24fb0d6963f7d28e17f72"));
   md5_of("message digest", d);
   CHECK(hex_is(d, "f96b697d7cb7938d525a2f31aaf161d0"));

   memset(key, 0x0b, 16);
   CHECK(hmac_is(key, 16, (const unsigned char*)"Hi There", 8, "9294727a3638bb1c13f48ef8158bfc9d"));
   CHECK(hmac_is((const unsigned char*)"Jefe", 4,
         (const unsigned char*)"what do ya want for nothing?", 28, "750c783e6ab0b503eaa86e310a5db738"));
   memset(key, 0xaa, 16);
   memset(data, 0xdd, 50);
   CHECK(hmac_is(key, 16, data, 50, "56be34521d144c88dbb8c733f0e8b3f6"));
   for (i = 0; i < 25; i++)
      key[i] = (unsigned char)(i + 1);
   memset(data, 0xcd, 50);
   CHECK(hmac_is(key, 25, data, 50, "697eaf0aca3a3aea3a75164746ffaa79"));
   memset(key, 0x0c, 16);
   CHECK(hmac_is(key, 16, (const unsigned char*)"Test With Truncation", 20, "56461ef2342edc00f9bab995690efd4c"));

   /* in pieces, as prudp signs a packet, into a context not cleared */
   memset(key, 0xaa, 16);
   memset(data, 0xdd, 50);
   memset(&ctx, 0xFF, sizeof(ctx));
   hmacMD5_init_limK_to_64(key, 16, &ctx);
   hmacMD5_update(data, 7, &ctx);
   hmacMD5_update(data + 7, 0, &ctx);
   hmacMD5_update(data + 7, 43, &ctx);
   hmacMD5_final(d, &ctx);
   CHECK(hex_is(d, "56be34521d144c88dbb8c733f0e8b3f6"));

   /* a one octet key counts */
   hmacMD5((const unsigned char*)"a", 1, (const unsigned char*)"x", 1, d);
   hmacMD5((const unsigned char*)"b", 1, (const unsigned char*)"x", 1, d2);
   CHECK(memcmp(d, d2, 16) != 0);

   /* an 80 octet key is its first 64 (RFC 2202's would hash it) */
   for (i = 0; i < 80; i++)
      key[i] = (unsigned char)(0xaa ^ i);
   hmacMD5(key, 80, (const unsigned char*)"x", 1, d);
   hmacMD5(key, 64, (const unsigned char*)"x", 1, d2);
   CHECK(memcmp(d, d2, 16) == 0);
   hmacMD5(key, 63, (const unsigned char*)"x", 1, d2);
   CHECK(memcmp(d, d2, 16) != 0);
   memset(key, 0xaa, 80);
   CHECK(!hmac_is(key, 80, (const unsigned char*)"Test Using Larger Than Block-Size Key - Hash Key First", 54,
         "6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd"));

   if (failures)
   {
      fprintf(stderr, "hmac_md5_test: %d failed\n", failures);
      return 1;
   }
   printf("hmac_md5_test: all passed\n");
   return 0;
}
