#ifndef UTIL_HELPERS_ZLIB_STREAM_H
#define UTIL_HELPERS_ZLIB_STREAM_H

/* zlib streams (RFC 1950) through libretro-common's deflate, in place of
 * zlib's compress and uncompress: zlib reads what zlib_compress writes, and
 * zlib_uncompress reads what zlib writes. C89. */

#include <stddef.h>
#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum zlib_result
{
   ZLIB_OK = 0,
   ZLIB_BUFFER_TOO_SMALL, /* the output filled before the stream ended */
   ZLIB_DATA_ERROR,       /* malformed, or the input ended before it did */
   ZLIB_OUT_OF_MEMORY
};

/* zlib's compressBound(): what a game sizes its buffer by */
size_t zlib_compress_bound(size_t len);
/* room enough for zlib_compress never to run out, whatever the data */
size_t zlib_compress_room(size_t len);

/* A zlib stream of src into dst, *dst_len octets of room; *dst_len becomes
 * its length. 0 when it does not fit, or for no memory; level -1 is 6. */
int zlib_compress(uint8_t *dst, size_t *dst_len, const uint8_t *src,
      size_t src_len, int level);

/* The stream at src, *src_len octets, into dst, *dst_len octets of room;
 * *dst_len becomes what was decoded and *src_len what the decoder read,
 * which can run a few octets past the stream's end.
 * window_bits as zlib's: 8..15 a zlib stream, -8..-15 raw deflate, 16+
 * gzip, 32+ either. Inflaters are kept and lent from a small lock-free
 * pool, so only the first calls (or more at once than the pool holds)
 * allocate. */
enum zlib_result zlib_uncompress(uint8_t *dst, size_t *dst_len,
      const uint8_t *src, size_t *src_len, int window_bits);

uint32_t zlib_adler32(uint32_t adler, const uint8_t *data, size_t len);
uint32_t zlib_crc32(uint32_t crc, const uint8_t *data, size_t len);

#ifdef ZLIB_STREAM_TEST_HOOKS
/* the inflaters in the pool, not lent */
size_t zlib_stream_pooled(void);
#endif

RETRO_END_DECLS

#endif
