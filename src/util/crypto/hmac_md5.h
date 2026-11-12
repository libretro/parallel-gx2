#ifndef UTIL_CRYPTO_HMAC_MD5_H
#define UTIL_CRYPTO_HMAC_MD5_H

/* HMAC-MD5 (RFC 2104) over libretro-common's MD5, as NEX signs packets
 * and tickets with it. C89. */

#include <lrc_hash.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct
{
   MD5_CTX ctx;
   unsigned char k_ipad[64];
   unsigned char k_opad[64];
} HMACMD5Ctx;

/* A key over 64 octets is cut to its first 64, where RFC 2104 would hash
 * it: what NEX does */
void hmacMD5_init_limK_to_64(const unsigned char *key, int key_len, HMACMD5Ctx *ctx);
void hmacMD5_update(const unsigned char *text, int text_len, HMACMD5Ctx *ctx);
void hmacMD5_final(unsigned char *digest, HMACMD5Ctx *ctx);
/* the 16 octet MAC of text */
void hmacMD5(const unsigned char *key, int key_len, const unsigned char *text,
      int text_len, unsigned char *digest);

RETRO_END_DECLS

#endif
