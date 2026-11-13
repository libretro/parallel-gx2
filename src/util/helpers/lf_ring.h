#ifndef UTIL_HELPERS_LF_RING_H
#define UTIL_HELPERS_LF_RING_H

/* A bounded queue of fixed-size items any thread pushes to and pops from
 * with a compare-and-swap (D. Vyukov's), in storage its owner gives it:
 * nothing allocated, no lock. Each cell's sequence is kept less the cell's
 * index, so storage all zero is an empty queue. C89. */

#include <stddef.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* the octets a cell takes for items of size octets: the item, 8-aligned,
 * then its sequence */
#define LF_RING_STRIDE(size) ((((size) + 7u) & ~(size_t)7u) + 8u)

typedef struct lf_ring
{
   retro_atomic_int_t enqueue;
   retro_atomic_int_t dequeue;
   unsigned char *cells;
   unsigned cap;   /* a power of two */
   unsigned size;  /* of an item */
   unsigned stride;
} lf_ring_t;

/* cells: cap * LF_RING_STRIDE(size) octets, all zero, 8-aligned */
void lf_ring_init(lf_ring_t *r, void *cells, unsigned cap, unsigned size);
/* false when full */
bool lf_ring_push(lf_ring_t *r, const void *item);
/* false when empty */
bool lf_ring_pop(lf_ring_t *r, void *item);
/* how many are queued, as current as the moment it is read */
unsigned lf_ring_count(lf_ring_t *r);

RETRO_END_DECLS

#endif
