#ifndef UTIL_HELPERS_OBJ_POOL_H
#define UTIL_HELPERS_OBJ_POOL_H

/* Blocks of one size handed out and taken back by any thread, with no lock,
 * kept for reuse rather than freed: the renderer's objects made and dropped
 * while a game plays come from here, so that once a game has reached its
 * peak nothing more is allocated. C89.
 *
 * Blocks come in chunks of per_chunk, at most OBJ_POOL_MAX_CHUNKS of them,
 * malloc'd as the pool first needs each; past them a block is malloc'd on
 * its own and freed when put back. The free blocks are a bounded lock-free
 * queue of their indices (lf_ring.h), with room for every block there can
 * be, in storage its owner gives it. */

#include <stddef.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

#include "lf_ring.h"

RETRO_BEGIN_DECLS

#define OBJ_POOL_MAX_CHUNKS 64
/* the octets the free queue's storage takes */
#define OBJ_POOL_RING_SIZE(per_chunk) \
   ((size_t)(per_chunk) * OBJ_POOL_MAX_CHUNKS * LF_RING_STRIDE(4))

typedef struct obj_pool
{
   lf_ring_t free;
   retro_atomic_ptr_t chunks[OBJ_POOL_MAX_CHUNKS];
   retro_atomic_int_t claimed; /* chunks taken, those malloc'd or being */
   retro_atomic_int_t loose;   /* blocks malloc'd one by one, out now */
   retro_atomic_int_t out;     /* blocks handed out and not back */
   unsigned size;              /* of a block, as asked */
   unsigned stride;            /* of a block with its header */
   unsigned per_chunk;         /* a power of two */
} obj_pool_t;

/* ring: OBJ_POOL_RING_SIZE(per_chunk) octets, all zero, 8-aligned */
void obj_pool_init(obj_pool_t *p, void *ring, unsigned size, unsigned per_chunk);
/* chunks made now, so that the first blocks are not made while a game
 * plays; false for no memory */
bool obj_pool_reserve(obj_pool_t *p, unsigned chunks);
/* 16-aligned; NULL for no memory */
void *obj_pool_get(obj_pool_t *p);
void obj_pool_put(obj_pool_t *p, void *block);
/* blocks handed out and not back */
int obj_pool_out(obj_pool_t *p);
/* the chunks freed, when every block is back; false, and nothing freed,
 * while one is out */
bool obj_pool_free_all(obj_pool_t *p);

RETRO_END_DECLS

#endif
