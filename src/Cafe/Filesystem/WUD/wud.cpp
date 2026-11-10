#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wud.h"
#include "Common/FileStream.h"

wud_t* wud_open(const fs::path& path)
{
	// mapped where it can be: then a read is one copy from the pages, and the
	// FST decrypts straight from them (wud_lend)
	FileStream* fs = FileStream::openFileMapped(path);
	if( !fs )
		return nullptr;
	// allocate wud struct
	wud_t* wud = (wud_t*)malloc(sizeof(wud_t));
	memset(wud, 0x00, sizeof(wud_t));
	wud->fs = fs;
	// get size of file
	long long inputFileSize = wud->fs->GetSize();
	wud->fileSize = (unsigned long long)inputFileSize;
	const std::span<const uint8> mapped = wud->fs->MappedSpan();
	if (!mapped.empty() && mapped.size() == wud->fileSize)
		wud->mapped = mapped.data();
	// determine whether the WUD is compressed or not
	wuxHeader_t wuxHeader = {0};
	if( wud->fs->readData(&wuxHeader, sizeof(wuxHeader_t)) != sizeof(wuxHeader_t))
	{
		// file is too short to be either
		wud_close(wud);
		return nullptr;
	}
	if( wuxHeader.magic0 == WUX_MAGIC_0 && wuxHeader.magic1 == WUX_MAGIC_1 )
	{
		// this is a WUX file
		wud->isCompressed = true;
		wud->sectorSize = wuxHeader.sectorSize;
		wud->uncompressedSize = wuxHeader.uncompressedSize;
		// validate header values
		if( wud->sectorSize < 0x100 || wud->sectorSize >= 0x10000000 )
		{
			wud_close(wud);
			return nullptr;
		}
		// calculate offsets and sizes
		wud->indexTableEntryCount = (unsigned int)((wud->uncompressedSize+(long long)(wud->sectorSize-1)) / (long long)wud->sectorSize);
		wud->offsetIndexTable = sizeof(wuxHeader_t);
		wud->offsetSectorArray = (wud->offsetIndexTable + (long long)wud->indexTableEntryCount*sizeof(unsigned int));
		// align to SECTOR_SIZE
		wud->offsetSectorArray = (wud->offsetSectorArray + (long long)(wud->sectorSize-1));
		wud->offsetSectorArray = wud->offsetSectorArray - (wud->offsetSectorArray%(long long)wud->sectorSize);
		// read index table
		unsigned int indexTableSize = sizeof(unsigned int) * wud->indexTableEntryCount;
		if (wud->offsetSectorArray > inputFileSize)
		{
			// too short for its index table
			wud_close(wud);
			return nullptr;
		}
		if (wud->mapped)
		{
			// read where it lies (the header is 32 octets: it lies aligned)
			wud->indexTable = (const unsigned int*)(wud->mapped + wud->offsetIndexTable);
		}
		else
		{
			wud->indexTableCopy = (unsigned int*)malloc(indexTableSize);
			wud->indexTable = wud->indexTableCopy;
			wud->fs->SetPosition(wud->offsetIndexTable);
			if( !wud->indexTableCopy || wud->fs->readData(wud->indexTableCopy, indexTableSize) != indexTableSize )
			{
				// could not read index table
				wud_close(wud);
				return nullptr;
			}
		}
	}
	else
	{
		// uncompressed file
		wud->uncompressedSize = inputFileSize;
	}
	if (!wud->mapped)
	{
		// read from any number of threads at once, each with a stream
		wud->streams = new FileStreamPool(path, wud->fs);
		wud->fs = nullptr;
	}
	return wud;
}

void wud_close(wud_t* wud)
{
	delete wud->fs;
	delete wud->streams;
	free(wud->indexTableCopy);
	free(wud);
}

bool wud_isWUXCompressed(wud_t* wud)
{
	return wud->isCompressed;
}

// Where the image's octet at offset lies in the file, and how many follow it
// there in one piece (to its sector's end in a .wux); false past the end
static bool wud_locate(wud_t* wud, long long offset, unsigned long long& fileOffset, unsigned long long& run)
{
	if (offset < 0 || offset >= wud->uncompressedSize)
		return false;
	if (!wud->isCompressed)
	{
		fileOffset = (unsigned long long)offset;
		run = (unsigned long long)(wud->uncompressedSize - offset);
		return true;
	}
	const unsigned int sectorOffset = (unsigned int)(offset % (long long)wud->sectorSize);
	const unsigned int sectorIndex = wud->indexTable[offset / (long long)wud->sectorSize];
	fileOffset = (unsigned long long)wud->offsetSectorArray + (unsigned long long)sectorIndex * wud->sectorSize + sectorOffset;
	run = std::min<unsigned long long>(wud->sectorSize - sectorOffset, (unsigned long long)(wud->uncompressedSize - offset));
	return true;
}

const unsigned char* wud_lend(wud_t* wud, unsigned int length, long long offset)
{
	unsigned long long at, run;
	if (!wud->mapped || !wud_locate(wud, offset, at, run) || run < length || at > wud->fileSize || wud->fileSize - at < length)
		return nullptr;
	return wud->mapped + at;
}

/*
 * Read data from WUD file
 * Can read up to 4GB at once
 */
unsigned int wud_readData(wud_t* wud, void* buffer, unsigned int length, long long offset)
{
	// make sure there is no out-of-bounds read
	long long fileBytesLeft = wud->uncompressedSize - offset;
	if( fileBytesLeft <= 0 )
		return 0;
	if( fileBytesLeft < (long long)length )
		length = (unsigned int)fileBytesLeft;
	// read data, a run in one piece at a time: the whole of it in a .wud, up
	// to a sector's end in a .wux; from the pages where the file is mapped
	unsigned int readBytes = 0;
	while (length > 0)
	{
		unsigned long long at, run;
		if (!wud_locate(wud, offset, at, run))
			break;
		const unsigned int n = (unsigned int)std::min<unsigned long long>(run, length);
		unsigned int got;
		if (wud->mapped)
		{
			got = at < wud->fileSize ? (unsigned int)std::min<unsigned long long>(n, wud->fileSize - at) : 0;
			memcpy(buffer, wud->mapped + at, got);
		}
		else
		{
			FileStream* fs = wud->streams->Acquire();
			got = 0;
			if (fs)
			{
				fs->SetPosition(at);
				got = fs->readData(buffer, n);
			}
			wud->streams->Release(fs);
		}
		readBytes += got;
		if (got != n)
			break;
		buffer = (void*)((char*)buffer + n);
		length -= n;
		offset += n;
	}
	return readBytes;
}

/*
 * Returns the size of the data
 * For .wud: Size of file
 * For .wux: Size of uncompressed data
 */
long long wud_getWUDSize(wud_t* wud)
{
	return wud->uncompressedSize;
}