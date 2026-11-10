#ifndef UTIL_CRYPTO_AES128_H
#define UTIL_CRYPTO_AES128_H

/* AES-128 as the core uses it, over libretro-common's crypto: AES-NI or the
 * ARMv8 AES instructions where the CPU has them, several CBC blocks at once,
 * and software everywhere else. The key is given with each call. */

#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* nothing left to set up: kept for its callers */
void AES128_init(void);

/* one block */
void AES128_ECB_encrypt(const uint8_t *input, const uint8_t *key, uint8_t *output);
void AES128_ECB_decrypt(const uint8_t *input, const uint8_t *key, uint8_t *output);

/* length octets, whole blocks only (a part block at the end is left as it
 * is); iv NULL for one of zeros; output may be input */
void AES128_CBC_encrypt(uint8_t *output, const uint8_t *input, uint32_t length, const uint8_t *key, const uint8_t *iv);
void AES128_CBC_decrypt(uint8_t *output, const uint8_t *input, uint32_t length, const uint8_t *key, const uint8_t *iv);
/* the same, and iv left holding the last block of input, the next IV */
void AES128_CBC_decrypt_updateIV(uint8_t *output, const uint8_t *input, uint32_t length, const uint8_t *key, uint8_t *iv);

/* CTR in place, the whole 16 octets of nonceIv counted up big-endian per
 * block and left as the next; a part block at the end takes what it needs
 * of the last key stream block */
void AES128CTR_transform(uint8_t *data, int32_t length, const uint8_t *key, uint8_t *nonceIv);

RETRO_END_DECLS

#endif
