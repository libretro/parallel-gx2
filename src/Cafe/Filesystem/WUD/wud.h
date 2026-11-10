#ifndef CAFE_FILESYSTEM_WUD_H
#define CAFE_FILESYSTEM_WUD_H

/* A Wii U disc image, .wud (the disc as it is) or .wux (its sectors stored
 * once each, behind a table of where each one is), read over
 * libretro-common's filestream. Mapped where it can be: then a read is one
 * copy from the pages, and a range in one piece is lent (wud_lend). Where
 * it cannot be (a file only the frontend's VFS reaches), each reader at
 * once reads through a stream of its own, lent from a few the image keeps.
 * Any number of threads read at once, with no lock. */

#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct wud wud_t;

/* wud_open flags */
#define WUD_OPEN_NO_MAP 1 /* read through streams even where it could be mapped */

/* path as the host file system has it (UTF-8); NULL if it is neither */
wud_t *wud_open(const char *path, unsigned flags);
void wud_close(wud_t *wud);

bool wud_isWUXCompressed(const wud_t *wud);
/* Up to length octets of the image at offset into buffer; how many */
unsigned wud_readData(wud_t *wud, void *buffer, unsigned length, int64_t offset);
/* The image's length octets at offset, lent from the mapping and valid until
 * wud_close: NULL where they are not all there and in one piece (an
 * unmapped file, a range past the end, or one that crosses a .wux sector) */
const unsigned char *wud_lend(const wud_t *wud, unsigned length, int64_t offset);
/* .wud: the file's size; .wux: the size of the image it holds */
int64_t wud_getWUDSize(const wud_t *wud);

bool wud_isMapped(const wud_t *wud);
/* streams opened for readers, where it is not mapped */
unsigned wud_streamsOpened(const wud_t *wud);

RETRO_END_DECLS

#endif
