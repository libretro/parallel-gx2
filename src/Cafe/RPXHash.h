#pragma once

// The hash of an executable's octets that names it - the RPXHash in log.txt
// and graphic packs' rules - taken over the file in any number of pieces
constexpr uint32 RPXHASH_SEED = 0x3416DCBF;

inline uint32 RPXHash_Update(uint32 h, const uint8* data, size_t size)
{
	for (size_t i = 0; i < size; i++)
		h = ((h << 3) | (h >> 29)) + data[i];
	return h;
}
