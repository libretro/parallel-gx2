/* A value read without a lock and replaced whole (rcu_cell.h) */

#include <stddef.h>

#include "rcu_cell.h"

#if !defined(RETRO_ATOMIC_HAS_CAS) || !defined(RETRO_ATOMIC_HAS_PTR)
#error "rcu_cell needs retro_atomic's exchange and pointer operations"
#endif

/* test/rcu_cell runs code of its own where a reader has taken its side
 * and not yet counted itself in: the moment a writer can move on under it */
#ifdef RCU_CELL_TEST_HOOKS
void rcu_cell_test_side_taken(void);
#define RCU_CELL_SIDE_TAKEN() rcu_cell_test_side_taken()
#else
#define RCU_CELL_SIDE_TAKEN() do { } while (0)
#endif

bool rcu_cell_init(rcu_cell_t *cell, void *initial)
{
   retro_atomic_ptr_init(&cell->ptr, initial);
   retro_atomic_int_init(&cell->epoch, 0);
   retro_atomic_int_init(&cell->readers[0], 0);
   retro_atomic_int_init(&cell->readers[1], 0);
   retro_atomic_int_init(&cell->writing, 0);
   return retro_eventcount_init(&cell->wake);
}

void *rcu_cell_deinit(rcu_cell_t *cell)
{
   retro_eventcount_free(&cell->wake);
   return retro_atomic_load_relaxed_ptr(&cell->ptr);
}

static void rcu_leave(rcu_cell_t *cell, unsigned slot)
{
   /* the last one out wakes a writer waiting for it */
   if (retro_atomic_fetch_sub_int(&cell->readers[slot], 1) == 1)
      retro_eventcount_notify(&cell->wake);
}

const void *rcu_read_begin(rcu_cell_t *cell, rcu_reader_t *r)
{
   r->cell = cell;
   for (;;)
   {
      r->slot = (unsigned)retro_atomic_load_seq_cst_int(&cell->epoch) & 1;
      RCU_CELL_SIDE_TAKEN();
      retro_atomic_fetch_add_seq_cst_int(&cell->readers[r->slot], 1);
      if (((unsigned)retro_atomic_load_seq_cst_int(&cell->epoch) & 1) == r->slot)
         break;
      /* a writer moved on in between: count in on the new side */
      rcu_leave(cell, r->slot);
   }
   /* Counted in where the writer will look before it frees what this
    * reads; or, if it moved on after this looked, reading the epoch it
    * moved to made its new pointer visible here */
   r->ptr = retro_atomic_load_acquire_ptr(&cell->ptr);
   return r->ptr;
}

void rcu_read_end(rcu_reader_t *r)
{
   rcu_leave(r->cell, r->slot);
}

static bool rcu_turn_free(rcu_cell_t *cell)
{
   return retro_atomic_load_acquire_int(&cell->writing) == 0;
}

void rcu_write_begin(rcu_cell_t *cell)
{
   for (;;)
   {
      int key;
      if (retro_atomic_exchange_int(&cell->writing, 1) == 0)
         return;
      key = retro_eventcount_prepare_wait(&cell->wake);
      if (rcu_turn_free(cell))
         retro_eventcount_cancel_wait(&cell->wake);
      else
         retro_eventcount_commit_wait(&cell->wake, key);
   }
}

const void *rcu_write_current(rcu_cell_t *cell)
{
   return retro_atomic_load_acquire_ptr(&cell->ptr);
}

void *rcu_write_publish(rcu_cell_t *cell, void *fresh)
{
   void *old = retro_atomic_exchange_ptr(&cell->ptr, fresh);
   /* readers that may hold old counted in on the current side: move new
    * readers to the other one, and wait for these to leave */
   const unsigned slot = (unsigned)retro_atomic_fetch_add_seq_cst_int(&cell->epoch, 1) & 1;
   for (;;)
   {
      int key;
      if (retro_atomic_load_seq_cst_int(&cell->readers[slot]) == 0)
         break;
      key = retro_eventcount_prepare_wait(&cell->wake);
      if (retro_atomic_load_seq_cst_int(&cell->readers[slot]) == 0)
      {
         retro_eventcount_cancel_wait(&cell->wake);
         break;
      }
      retro_eventcount_commit_wait(&cell->wake, key);
   }
   retro_atomic_store_release_int(&cell->writing, 0);
   retro_eventcount_notify(&cell->wake);
   return old;
}
