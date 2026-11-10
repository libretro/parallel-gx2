/* The core's AES-128 (src/util/crypto/aes128.c) over libretro-common's
 * crypto, in C89 as it is:
 *  - one block each way, FIPS-197 C.1;
 *  - CBC each way, SP 800-38A F.2.1/F.2.2; a NULL IV is one of zeros;
 *    decrypted in place; a part block at the end left as it is;
 *  - a 64 KiB CBC decryption (the size of an FST hashed block, which the
 *    CPU's AES instructions take several blocks at a time) the same as
 *    each block decrypted on its own and chained by hand, at every
 *    alignment of source and destination;
 *  - the IV after AES128_CBC_decrypt_updateIV is the last ciphertext block,
 *    so two halves decrypt as the whole did;
 *  - CTR, SP 800-38A F.5.1; the counter carried across all 16 octets, not
 *    just the last four; a part block at the end; the counter left as the
 *    next. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/crypto/aes128.h"

static int failures = 0;
#define CHECK(cond) \
   do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void hex(const char *s, uint8_t *out)
{
   size_t i;
   for (i = 0; s[2 * i] && s[2 * i + 1]; i++)
   {
      unsigned v;
      sscanf(s + 2 * i, "%2x", &v);
      out[i] = (uint8_t)v;
   }
}

#define BIG 0x10000

int main(void)
{
   static uint8_t plain[BIG + 64], cipher[BIG + 64], out[BIG + 64], ref[BIG + 64];
   uint8_t key[16], iv[16], a[64], b[64], c[64];
   uint32_t state = 12345;
   size_t i;

   AES128_init();

   /* FIPS-197 C.1 */
   hex("000102030405060708090a0b0c0d0e0f", key);
   hex("00112233445566778899aabbccddeeff", a);
   hex("69c4e0d86a7b0430d8cdb78070b4c55a", c);
   AES128_ECB_encrypt(a, key, b);
   CHECK(memcmp(b, c, 16) == 0);
   AES128_ECB_decrypt(c, key, b);
   CHECK(memcmp(b, a, 16) == 0);

   /* SP 800-38A F.2.1 / F.2.2 */
   hex("2b7e151628aed2a6abf7158809cf4f3c", key);
   hex("000102030405060708090a0b0c0d0e0f", iv);
   hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
       "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", a);
   hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
       "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7", c);
   AES128_CBC_encrypt(b, a, 64, key, iv);
   CHECK(memcmp(b, c, 64) == 0);
   AES128_CBC_decrypt(b, c, 64, key, iv);
   CHECK(memcmp(b, a, 64) == 0);
   /* in place, and the input left alone by encryption */
   memcpy(b, c, 64);
   AES128_CBC_decrypt(b, b, 64, key, iv);
   CHECK(memcmp(b, a, 64) == 0);
   memcpy(b, a, 64);
   AES128_CBC_encrypt(out, b, 64, key, iv);
   CHECK(memcmp(b, a, 64) == 0 && memcmp(out, c, 64) == 0);
   /* a part block at the end is left as it is */
   memset(b, 0xEE, sizeof(b));
   AES128_CBC_decrypt(b, c, 40, key, iv);
   CHECK(memcmp(b, a, 32) == 0 && b[32] == 0xEE && b[39] == 0xEE);
   /* a NULL IV is zeros: the first block is then the ECB one */
   AES128_CBC_decrypt(b, c, 16, key, NULL);
   AES128_ECB_decrypt(c, key, out);
   CHECK(memcmp(b, out, 16) == 0);
   memset(iv, 0, 16);
   AES128_CBC_encrypt(b, a, 32, key, NULL);
   AES128_CBC_encrypt(out, a, 32, key, iv);
   CHECK(memcmp(b, out, 32) == 0);

   /* 64 KiB at once against block by block, at each alignment */
   for (i = 0; i < sizeof(plain); i++)
   {
      state = state * 1664525u + 1013904223u;
      plain[i] = (uint8_t)(state >> 24);
   }
   hex("0f1e2d3c4b5a69788796a5b4c3d2e1f0", key);
   hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", iv);
   AES128_CBC_encrypt(cipher, plain, BIG, key, iv);
   {
      const uint8_t *prev = iv;
      for (i = 0; i < BIG; i += 16)
      {
         size_t j;
         AES128_ECB_decrypt(cipher + i, key, ref + i);
         for (j = 0; j < 16; j++)
            ref[i + j] ^= prev[j];
         prev = cipher + i;
      }
      CHECK(memcmp(ref, plain, BIG) == 0);
   }
   {
      unsigned in_off, out_off;
      static uint8_t src[BIG + 64];
      for (in_off = 0; in_off < 16; in_off += 3)
         for (out_off = 0; out_off < 16; out_off += 5)
         {
            memcpy(src + in_off, cipher, BIG);
            memset(out, 0, sizeof(out));
            AES128_CBC_decrypt(out + out_off, src + in_off, BIG, key, iv);
            CHECK(memcmp(out + out_off, plain, BIG) == 0);
         }
   }

   /* two halves through updateIV decrypt as the whole did */
   memcpy(c, iv, 16);
   AES128_CBC_decrypt_updateIV(out, cipher, BIG / 2, key, c);
   CHECK(memcmp(c, cipher + BIG / 2 - 16, 16) == 0);
   AES128_CBC_decrypt_updateIV(out + BIG / 2, cipher + BIG / 2, BIG / 2, key, c);
   CHECK(memcmp(out, plain, BIG) == 0);

   /* SP 800-38A F.5.1 */
   hex("2b7e151628aed2a6abf7158809cf4f3c", key);
   hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
   hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
       "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", a);
   hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
       "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee", c);
   memcpy(b, a, 64);
   AES128CTR_transform(b, 64, key, iv);
   CHECK(memcmp(b, c, 64) == 0);
   hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdff03", a);
   CHECK(memcmp(iv, a, 16) == 0); /* the next counter */

   /* a carry through all 16 octets: the second block's counter is
    * 0x01 00..00, the first's 0x00 ff..ff */
   memset(iv, 0xff, 16);
   iv[0] = 0;
   memset(b, 0, 32);
   AES128CTR_transform(b, 32, key, iv);
   memset(a, 0xff, 16);
   a[0] = 0;
   AES128_ECB_encrypt(a, key, out);
   CHECK(memcmp(b, out, 16) == 0);
   memset(a, 0, 16);
   a[0] = 1;
   AES128_ECB_encrypt(a, key, out);
   CHECK(memcmp(b + 16, out, 16) == 0);
   memset(a, 0, 16);
   a[0] = 1;
   a[15] = 1;
   CHECK(memcmp(iv, a, 16) == 0);

   /* a part block at the end: what it needs of the key stream, no more */
   memset(iv, 0, 16);
   memset(b, 0, sizeof(b));
   b[20] = 0x5A;
   AES128CTR_transform(b, 20, key, iv);
   memset(a, 0, 16);
   a[15] = 1;
   AES128_ECB_encrypt(a, key, out);
   CHECK(memcmp(b + 16, out, 4) == 0 && b[20] == 0x5A);
   AES128CTR_transform(b, 0, key, iv);
   AES128CTR_transform(b, -16, key, iv);
   CHECK(b[20] == 0x5A);

   if (failures)
   {
      fprintf(stderr, "aes: %d check(s) failed\n", failures);
      return 1;
   }
   printf("aes: all checks passed\n");
   return 0;
}
