#pragma once

struct wuxHeader_t
{
	unsigned int		magic0;
	unsigned int		magic1;
	unsigned int		sectorSize;
	unsigned long long	uncompressedSize;
	unsigned int		flags;
};

struct wud_t
{
	class FileStream* fs; // the mapping's, where mapped
	// where not mapped: a stream for each reader at once
	class FileStreamPool* streams;
	// the file's octets, where it is mapped, while fs lives
	const unsigned char* mapped;
	unsigned long long	fileSize;
	long long		uncompressedSize;
	bool			isCompressed;
	// data used when compressed
	unsigned int	sectorSize;
	unsigned int	indexTableEntryCount;
	const unsigned int*	indexTable; // in the mapping, or indexTableCopy
	unsigned int*	indexTableCopy;
	long long		offsetIndexTable;
	long long		offsetSectorArray;
};

#define WUX_MAGIC_0	'0XUW' // "WUX0"
#define WUX_MAGIC_1	0x1099d02e

// wud and wux functions
wud_t* wud_open(const fs::path& path); // transparently handles wud and wux files
void wud_close(wud_t* wud);

bool wud_isWUXCompressed(wud_t* wud);
unsigned int wud_readData(wud_t* wud, void* buffer, unsigned int length, long long offset);
// The image's length octets at offset, lent from the mapping and valid until
// wud_close: nullptr where they are not all there and in one piece (an
// unmapped file, a range past the end, or one that crosses a .wux sector)
const unsigned char* wud_lend(wud_t* wud, unsigned int length, long long offset);
long long wud_getWUDSize(wud_t* wud);