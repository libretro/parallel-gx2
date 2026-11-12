#ifndef UTIL_CRYPTO_BASE64_CODEC_H
#define UTIL_CRYPTO_BASE64_CODEC_H

/* Base64 (RFC 4648, '+' and '/', '=' padding), as the console's services
 * and Miiverse send it: encoded, and decoded the way Cemu always has - a
 * length of 3 or less, or not a multiple of 4, decodes to nothing,
 * '=' counts as 0 wherever it is, and a character that
 * is not base64 decodes to the bits it always did rather than an error.
 * C89. */

#include <stddef.h>
#include <stdint.h>

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* the room b64_encode needs for len octets (no NUL) */
#define B64_ENCODED_SIZE(len) ((((len) + 2) / 3) * 4)

/* in, len octets, as base64 into out (B64_ENCODED_SIZE(len) octets, no
 * NUL written); its length */
size_t b64_encode(char *out, const void *in, size_t len);

/* what b64_decode writes for in, len characters: 0 where len is 3 or less
 * or not a multiple of 4; else 3 for every 4, one less for each of the
 * last two characters that is '=' */
size_t b64_decoded_size(const char *in, size_t len);

/* in, len characters, into out (b64_decoded_size(in, len) octets); how
 * many were written */
size_t b64_decode(uint8_t *out, const char *in, size_t len);

RETRO_END_DECLS

#endif
