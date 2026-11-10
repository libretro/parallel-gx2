/* AES-128 over libretro-common's crypto (aes128.h) */

#include <string.h>

#include <crypto/crypto.h>

#include "aes128.h"

void AES128_init(void)
{
}

void AES128_ECB_encrypt(const uint8_t *input, const uint8_t *key, uint8_t *output)
{
   struct aes_ctx ctx;
   aes_init(&ctx, key, 16);
   aes_encrypt_block(&ctx, input, output);
}

void AES128_ECB_decrypt(const uint8_t *input, const uint8_t *key, uint8_t *output)
{
   struct aes_ctx ctx;
   aes_init(&ctx, key, 16);
   aes_decrypt_block(&ctx, input, output);
}

void AES128_CBC_encrypt(uint8_t *output, const uint8_t *input, uint32_t length, const uint8_t *key, const uint8_t *iv)
{
   struct aes_ctx ctx;
   uint8_t chain[16];
   if (iv)
      memcpy(chain, iv, 16);
   else
      memset(chain, 0, 16);
   aes_init(&ctx, key, 16);
   aes_cbc_encrypt(&ctx, chain, input, output, length & ~(uint32_t)15);
}

void AES128_CBC_decrypt(uint8_t *output, const uint8_t *input, uint32_t length, const uint8_t *key, const uint8_t *iv)
{
   struct aes_ctx ctx;
   uint8_t chain[16];
   if (iv)
      memcpy(chain, iv, 16);
   else
      memset(chain, 0, 16);
   aes_init(&ctx, key, 16);
   aes_cbc_decrypt(&ctx, chain, input, output, length & ~(uint32_t)15);
}

void AES128_CBC_decrypt_updateIV(uint8_t *output, const uint8_t *input, uint32_t length, const uint8_t *key, uint8_t *iv)
{
   struct aes_ctx ctx;
   length &= ~(uint32_t)15;
   if (length == 0)
      return;
   aes_init(&ctx, key, 16);
   /* aes_cbc_decrypt leaves iv holding the last ciphertext block */
   aes_cbc_decrypt(&ctx, iv, input, output, length);
}

void AES128CTR_transform(uint8_t *data, int32_t length, const uint8_t *key, uint8_t *nonceIv)
{
   struct aes_ctx ctx;
   int32_t done;
   if (length <= 0)
      return;
   aes_init(&ctx, key, 16);
   for (done = 0; done < length; done += 16)
   {
      uint8_t stream[16];
      int32_t n = length - done < 16 ? length - done : 16;
      int32_t i;
      aes_encrypt_block(&ctx, nonceIv, stream);
      for (i = 0; i < n; i++)
         data[done + i] ^= stream[i];
      /* the next counter: all 16 octets, big-endian */
      for (i = 15; i >= 0; i--)
         if (++nonceIv[i] != 0)
            break;
   }
}
