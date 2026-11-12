/* HMAC-MD5 (hmac_md5.h) */

#include <string.h>

#include "hmac_md5.h"

void hmacMD5_init_limK_to_64(const unsigned char *key, int key_len, HMACMD5Ctx *ctx)
{
   int i;
   if (key_len > 64)
      key_len = 64;
   memset(ctx, 0, sizeof(*ctx));
   if (key_len > 0)
   {
      memcpy(ctx->k_ipad, key, (size_t)key_len);
      memcpy(ctx->k_opad, key, (size_t)key_len);
   }
   for (i = 0; i < 64; i++)
   {
      ctx->k_ipad[i] ^= 0x36;
      ctx->k_opad[i] ^= 0x5c;
   }
   MD5_Init(&ctx->ctx);
   MD5_Update(&ctx->ctx, ctx->k_ipad, 64);
}

void hmacMD5_update(const unsigned char *text, int text_len, HMACMD5Ctx *ctx)
{
   MD5_Update(&ctx->ctx, text, (unsigned long)text_len);
}

void hmacMD5_final(unsigned char *digest, HMACMD5Ctx *ctx)
{
   MD5_CTX outer;
   MD5_Final(digest, &ctx->ctx);
   MD5_Init(&outer);
   MD5_Update(&outer, ctx->k_opad, 64);
   MD5_Update(&outer, digest, 16);
   MD5_Final(digest, &outer);
}

void hmacMD5(const unsigned char *key, int key_len, const unsigned char *text,
      int text_len, unsigned char *digest)
{
   HMACMD5Ctx ctx;
   hmacMD5_init_limK_to_64(key, key_len, &ctx);
   hmacMD5_update(text, text_len, &ctx);
   hmacMD5_final(digest, &ctx);
}
