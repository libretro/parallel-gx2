#ifndef PPCREC_REC_STORE_H
#define PPCREC_REC_STORE_H

/* The recompiler's bookkeeping, with no lock.
 *
 * rec_store: the recompiled functions by the guest code they cover, in
 * buckets of lists. One thread changes the lists (the recompiler's: it
 * puts a function in, and takes out the ones that are gone); any thread
 * looks through them (an invalidation, for the functions it ends). A node
 * taken out is handed back for freeing only once no look that began
 * before can still be on it. The nodes are the caller's: one for each
 * bucket a range touches (rec_store_nodes).
 *
 * rec_inval: the code ranges invalidated lately, which any thread adds to
 * and the recompiler's reads, to tell whether what it translated changed
 * meanwhile. A ring of the last REC_INVAL_SLOTS: one older than that, or
 * being written, counts as touching everything. C89. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct rec_node
{
   retro_atomic_ptr_t next;
   void *obj;
   uint32_t start, end;       /* the range, end past it */
   size_t bucket;
   struct rec_node *retired;  /* the writer's: groups taken out */
   size_t group;              /* in a group's first node: how many */
} rec_node_t;

typedef struct rec_store
{
   retro_atomic_ptr_t *heads;
   size_t count;
   uint32_t granularity;
   retro_atomic_int_t readers;
   rec_node_t *retired;       /* the writer's */
} rec_store_t;

/* heads: count of them, all NULL */
void rec_store_init(rec_store_t *s, retro_atomic_ptr_t *heads, size_t count, uint32_t granularity);
/* the nodes a range [start, end) takes */
size_t rec_store_nodes(const rec_store_t *s, uint32_t start, uint32_t end);

/* The writer's: the range put in under obj, through nodes (as many as
 * rec_store_nodes says) */
void rec_store_insert(rec_store_t *s, rec_node_t *nodes, void *obj, uint32_t start, uint32_t end);
/* The writer's: n nodes (one group, its first nodes[0]) taken out of the
 * lists, to be handed back by rec_store_collect */
void rec_store_remove(rec_store_t *s, rec_node_t *nodes, size_t n);
/* The writer's: each group taken out that no look can still be on, handed
 * to done(first node) */
void rec_store_collect(rec_store_t *s, void (*done)(rec_node_t *first, void *ud), void *ud);

/* Any thread: f for every range in it that overlaps [start, end) - more
 * than once for a range across buckets */
void rec_store_find(rec_store_t *s, uint32_t start, uint32_t end,
      void (*f)(void *obj, uint32_t start, uint32_t end, void *ud), void *ud);

#define REC_INVAL_SLOTS 64

typedef struct rec_inval_slot
{
   retro_atomic_int_t seq;    /* its sequence + 1 once written */
   retro_atomic_int_t start;
   retro_atomic_int_t end;
} rec_inval_slot_t;

typedef struct rec_inval
{
   retro_atomic_int_t next;   /* the next sequence */
   rec_inval_slot_t slots[REC_INVAL_SLOTS];
} rec_inval_t;

void rec_inval_init(rec_inval_t *v);
/* Any thread: [start, end) invalidated */
void rec_inval_add(rec_inval_t *v, uint32_t start, uint32_t end);
/* The sequence now: what rec_inval_since reads from */
int rec_inval_now(rec_inval_t *v);
/* Whether any range added from sequence from on overlaps [start, end):
 * true too when that can no longer be told */
bool rec_inval_since(rec_inval_t *v, int from, uint32_t start, uint32_t end);

RETRO_END_DECLS

#endif
