#ifndef UTIL_HELPERS_RCU_CELL_H
#define UTIL_HELPERS_RCU_CELL_H

/* A value read all the time and replaced now and then, without a lock on
 * the way in: a reader counts itself in, takes the current pointer, and
 * reads an object nothing will change or free while it is counted in. A
 * writer builds the new value, swaps it in, and gets the old one back to
 * free once every reader that may still hold it has left - readers counted
 * in before the swap, told apart from the ones after it by which of two
 * counters they used. Readers never wait; writers take turns with each
 * other and wait only for readers already inside, asleep on an eventcount.
 *
 * C, for the core's C; RcuCell.h is the C++ over it. */

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>
#include <rthreads/retro_eventcount.h>

RETRO_BEGIN_DECLS

typedef struct rcu_cell
{
   retro_atomic_ptr_t ptr;
   retro_atomic_int_t epoch;
   retro_atomic_int_t readers[2];
   retro_atomic_int_t writing;
   retro_eventcount_t wake;
} rcu_cell_t;

typedef struct rcu_reader
{
   rcu_cell_t *cell;
   unsigned slot;
   const void *ptr; /* what this reader reads, until rcu_read_end */
} rcu_reader_t;

/* initial: the first value, the cell's from now on */
bool rcu_cell_init(rcu_cell_t *cell, void *initial);
/* No reader or writer may be inside. The value in place, for the caller to
 * free. */
void *rcu_cell_deinit(rcu_cell_t *cell);

/* Never waits. The value is r->ptr until rcu_read_end. A reader must not
 * be held across a write on the same thread: the write would wait for it. */
const void *rcu_read_begin(rcu_cell_t *cell, rcu_reader_t *r);
void rcu_read_end(rcu_reader_t *r);

/* A writer's turn: one writer at a time. In it, the value in place may be
 * read (rcu_write_current) to build the next one from. */
void rcu_write_begin(rcu_cell_t *cell);
const void *rcu_write_current(rcu_cell_t *cell);
/* Puts fresh in place, waits for the readers that may hold the old value,
 * ends the turn, and gives the old value back to be freed */
void *rcu_write_publish(rcu_cell_t *cell, void *fresh);

RETRO_END_DECLS

#endif
