#ifndef UTIL_HELPERS_U64_MAP_H
#define UTIL_HELPERS_U64_MAP_H

/* A map of 64-bit keys to pointers in storage its owner gives it, for one
 * thread: open addressing with linear probing, an entry taken out by
 * moving the ones after it back (no tombstones), nothing allocated. It
 * takes no more than three quarters of its slots; past that a put says
 * so. C89. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct u64_map_slot
{
   uint64_t key;
   void *value; /* NULL: free */
} u64_map_slot_t;

typedef struct u64_map
{
   u64_map_slot_t *slots;
   size_t cap;   /* a power of two */
   size_t count;
} u64_map_t;

/* slots: cap of them, all zero */
void u64_map_init(u64_map_t *m, u64_map_slot_t *slots, size_t cap);
/* NULL when key is not in it */
void *u64_map_get(const u64_map_t *m, uint64_t key);
/* key to value (not NULL), in place of what it was; false when full */
bool u64_map_put(u64_map_t *m, uint64_t key, void *value);
/* what key was, and taken out; NULL when it was not in it */
void *u64_map_take(u64_map_t *m, uint64_t key);
/* every entry, in no order: *cursor from 0; NULL when no more */
void *u64_map_next(const u64_map_t *m, size_t *cursor, uint64_t *key);
void u64_map_clear(u64_map_t *m);

RETRO_END_DECLS

#endif
