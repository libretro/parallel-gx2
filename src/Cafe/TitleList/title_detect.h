#ifndef CAFE_TITLELIST_TITLE_DETECT_H
#define CAFE_TITLELIST_TITLE_DETECT_H

/* What a file or folder is, by its name or its first octets, as the title
 * list scans and a title is opened. C89. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* in CafeTitleFileType's order */
enum title_file_type
{
   TITLE_FILE_UNKNOWN = 0,
   TITLE_FILE_WUD,
   TITLE_FILE_WUX,
   TITLE_FILE_RPX,
   TITLE_FILE_ELF
};

/* By its header (a .wux's magic, an RPX's or ELF's) or, for a file of 64 KiB
 * or more, a .wud's "WUP-" and the magic at 0x10000. Files under 32 octets,
 * and ones that cannot be opened, are unknown. path as the host has it. */
enum title_file_type title_file_type(const char *path);

/* What a file's name says it is, ignoring ASCII case */
enum title_name_kind
{
   TITLE_NAME_OTHER = 0,
   TITLE_NAME_RPX,   /* ends in .rpx */
   TITLE_NAME_DISC,  /* ends in .wud, .wux or .iso */
   TITLE_NAME_TMD,   /* is title.tmd */
   TITLE_NAME_WUA,   /* ends in .wua */
   TITLE_NAME_WUHB   /* ends in .wuhb */
};
enum title_name_kind title_name_kind(const char *filename);

/* Whether a scan looks into a file of that name: one whose extension is
 * .wud, .wux, .iso, .wua or .wuhb, or that is title.tmd (a name starting
 * with its only dot has no extension) */
bool title_scan_wanted(const char *filename);

/* Whether a folder of that name in an MLC's title folders is a title's:
 * eight hex digits */
bool title_scan_mlc_folder(const char *name);

/* A .wua's title folder: <16 hex digits of the title id>_v<version>, the
 * version decimal, without leading zeros, at most 65535 */
bool title_wua_folder(const char *name, size_t len, uint64_t *title_id,
      uint16_t *version);

RETRO_END_DECLS

#endif
