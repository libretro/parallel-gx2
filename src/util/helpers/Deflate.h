#pragma once

#include <cstddef>
#include <cstdint>

#include "util/helpers/zlib_stream.h"

// zlib streams (RFC 1950) through libretro-common's deflate, in place of
// zlib's compress and uncompress, over the C (zlib_stream.h). The streams
// are zlib's: zlib reads what Compress writes, and Uncompress reads what
// zlib writes.
namespace Deflate
{
	enum class Result
	{
		Ok = ZLIB_OK,
		BufferTooSmall = ZLIB_BUFFER_TOO_SMALL, // the output filled before the stream ended
		DataError = ZLIB_DATA_ERROR,            // malformed, or the input ended before the stream
		OutOfMemory = ZLIB_OUT_OF_MEMORY,
	};

	// zlib's compressBound(): what a game sizes its buffer by
	inline size_t ZlibCompressBound(size_t len) { return zlib_compress_bound(len); }
	// room enough for Compress() to never run out, whatever the data
	inline size_t CompressBound(size_t len) { return zlib_compress_room(len); }

	// A zlib stream of src into dst, dstLen bytes of room; dstLen becomes
	// its length. False when it does not fit, or out of memory.
	inline bool Compress(uint8_t* dst, size_t& dstLen, const uint8_t* src, size_t srcLen, int level = 6)
	{
		return zlib_compress(dst, &dstLen, src, srcLen, level) != 0;
	}

	// The stream at src, srcLen bytes, into dst, dstLen bytes of room;
	// dstLen becomes what was decoded and srcLen what the decoder read,
	// which can run a few bytes past the stream's end.
	// windowBits as zlib's: 8..15 a zlib stream, -8..-15 raw deflate,
	// 16+ gzip, 32+ either. Inflaters are lent from a pool and reused.
	inline Result Uncompress(uint8_t* dst, size_t& dstLen, const uint8_t* src, size_t& srcLen, int windowBits = 15)
	{
		return (Result)zlib_uncompress(dst, &dstLen, src, &srcLen, windowBits);
	}

	inline uint32_t Adler32(uint32_t adler, const uint8_t* data, size_t len) { return zlib_adler32(adler, data, len); }
	inline uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len) { return zlib_crc32(crc, data, len); }
}
