#ifndef CAFE_FILESYSTEM_FST_OPEN_H
#define CAFE_FILESYSTEM_FST_OPEN_H

/* Volumes (fst_volume.h) opened from what holds them:
 * - a Wii U disc image's game partition (GM): the partition table decrypted
 *   with the disc's key, the system partition (SI) opened for the GM
 *   partition's ticket, and the GM partition opened with the title key the
 *   ticket holds;
 * - a title's content files (.app), as its TMD lists them, with the title
 *   key of its ticket, each raw content checked against the TMD's hash.
 * C89. */

#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "fst_volume.h"

RETRO_BEGIN_DECLS

/* why a disc did not open */
#define FST_DISC_OK                0
#define FST_DISC_NO_IMAGE          1 /* not opened, or not a disc image */
#define FST_DISC_WRONG_KEY         2 /* the partition table does not decrypt */
#define FST_DISC_BAD_TABLE         3 /* its block size or partitions are not ones read */
#define FST_DISC_NO_PARTITION      4 /* no SI or no GM partition, or more than one SI */
#define FST_DISC_BAD_PARTITION     5 /* a partition's header is not one */
#define FST_DISC_SI_FST            6 /* the SI partition's FST would not open */
#define FST_DISC_BAD_TICKET        7 /* the GM partition's ticket is missing or not one */
#define FST_DISC_GM_FST            8 /* the GM partition's FST would not open */

/* the octets a disc key is tried on: 48 of the partition table, which
 * decrypt to zeroes with the right key */
#define FST_DISC_KEY_PROBE_OFFSET  0x18100
#define FST_DISC_KEY_PROBE_SIZE    48

/* The image at path (as the host has it, UTF-8), decrypted with disc_key
 * (16 octets). why, when not NULL, gets FST_DISC_*, and detail the
 * partition count or partition addresses of one refused for them. */
fst_volume_t *fst_disc_open(const char *path, const uint8_t *disc_key, int *why, uint32_t *detail);

/* The title key of a ticket (.tik) of size octets into key (16 octets);
 * false if it is not a Wii U ticket. */
bool fst_ticket_title_key(const uint8_t *tik, size_t size, uint8_t *key);

/* a title's metadata (.tmd): its contents, cluster by cluster */
#define FST_TMD_SHA1            0x2000 /* a content's hash: SHA-1, not SHA-256 */
#define FST_TMD_HASHED          0x0002 /* a content of hashed blocks */

typedef struct fst_tmd_content
{
   uint32_t id;       /* its file: <id>.app, in hex */
   uint16_t index;    /* its cluster */
   uint16_t type;     /* FST_TMD_* */
   uint64_t size;
   uint8_t hash[32];
} fst_tmd_content_t;

typedef struct fst_tmd
{
   uint64_t title_id;
   uint16_t title_version;
   uint16_t count;
   fst_tmd_content_t *contents; /* count of them */
} fst_tmd_t;

/* false if it is not a TMD (or for no memory); fst_tmd_free lets go */
bool fst_tmd_parse(const uint8_t *tmd, size_t size, fst_tmd_t *out);
void fst_tmd_free(fst_tmd_t *tmd);

/* why a title did not open */
#define FST_TITLE_OK            0
#define FST_TITLE_NO_FILES      1 /* a content's file did not open */
#define FST_TITLE_NO_FST        2 /* no content is cluster 0 */
#define FST_TITLE_BAD_FST       3

/* The title tmd lists, its contents at paths (one for each, in the TMD's
 * order, as the host has them), decrypted with title_key (16 octets). */
fst_volume_t *fst_title_open(const fst_tmd_t *tmd, const uint8_t *title_key,
      const char *const *paths, int *why);

RETRO_END_DECLS

#endif
