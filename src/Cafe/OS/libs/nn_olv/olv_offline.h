#ifndef CAFE_OS_LIBS_NN_OLV_OLV_OFFLINE_H
#define CAFE_OS_LIBS_NN_OLV_OLV_OFFLINE_H

/* Miiverse's offline posts (OfflineDB.zar): a post's files - its memo and
 * its picture - looked up and read out of the archive. C89. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

#include "Cafe/Filesystem/zarchive.h"

RETRO_BEGIN_DECLS

/* a memo: 320x120, a bit a pixel, low bit first, set for white */
#define OLV_OFFLINE_MEMO_W 320
#define OLV_OFFLINE_MEMO_H 120
#define OLV_OFFLINE_MEMO_BITS_SIZE (OLV_OFFLINE_MEMO_W * OLV_OFFLINE_MEMO_H / 8)
/* and as the post carries it, before compression: a 32 bpp TGA */
#define OLV_OFFLINE_TGA_HEADER_SIZE 18
#define OLV_OFFLINE_MEMO_TGA_SIZE (OLV_OFFLINE_TGA_HEADER_SIZE + OLV_OFFLINE_MEMO_W * OLV_OFFLINE_MEMO_H * 4)

enum olv_offline_read_result
{
   OLV_OFFLINE_OK = 0,
   OLV_OFFLINE_MISSING,   /* no such file (or no archive) */
   OLV_OFFLINE_TOO_LARGE, /* more than there is room for: nothing written */
   OLV_OFFLINE_FAILED     /* the archive could not be read */
};

/* "<dir><id><ext>" into out (cap octets with its NUL), the post's id read
 * from at most id_cap octets - the post's field is not always terminated.
 * false where it does not fit. */
bool olv_offline_name(char *out, size_t cap, const char *dir,
      const uint8_t *id, size_t id_cap, const char *ext);

/* name's size; false where it is not a file in ar (or ar is NULL) */
bool olv_offline_size(const zarchive_t *ar, const char *name, uint32_t *size);

/* The whole of name into out, room octets of it; *size its size */
enum olv_offline_read_result olv_offline_read(const zarchive_t *ar, const char *name,
      void *out, uint32_t room, uint32_t *size);

/* The memo at name - OLV_OFFLINE_MEMO_BITS_SIZE octets of bits - as the
 * post carries it: the TGA, as a zlib stream, into out (room octets);
 * *size its size. false where there is no such memo, it is not that size,
 * or it does not fit. */
bool olv_offline_memo(const zarchive_t *ar, const char *name,
      uint8_t *out, uint32_t room, uint32_t *size);

/* Up to max of n posts in random order, each tried once: take(ud, index,
 * slot) fills slot (0, 1, ...) from post index and says whether it could;
 * one that could not is passed over, and the next tried in its slot. pool
 * is n entries of room. seed picks the order (xorshift, not rand(): no
 * state shared with anything else). How many were taken. */
uint32_t olv_offline_pick(uint32_t n, uint32_t max, uint32_t seed, uint32_t *pool,
      bool (*take)(void *ud, uint32_t index, uint32_t slot), void *ud);

RETRO_END_DECLS

#endif
