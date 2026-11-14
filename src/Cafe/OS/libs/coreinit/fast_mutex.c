/* The guest's OSFastMutex without a lock of the host's (fast_mutex.h) */

#include "fast_mutex.h"

bool fast_mutex_take(retro_atomic_int_t *owner, int me)
{
   return retro_atomic_load_acquire_int(owner) == 0
      && retro_atomic_cas_int(owner, 0, me);
}

bool fast_mutex_owned(retro_atomic_int_t *owner, int me)
{
   return retro_atomic_load_acquire_int(owner) == me;
}

bool fast_mutex_wait_begin(retro_atomic_int_t *owner, retro_atomic_int_t *waiters, int me)
{
   /* counted before the last look: a giver after it sees the count */
   retro_atomic_fetch_add_seq_cst_int(waiters, 1);
   if (retro_atomic_cas_int(owner, 0, me))
   {
      retro_atomic_fetch_add_seq_cst_int(waiters, -1);
      return true;
   }
   return false;
}

void fast_mutex_wait_end(retro_atomic_int_t *waiters)
{
   retro_atomic_fetch_add_seq_cst_int(waiters, -1);
}

bool fast_mutex_give(retro_atomic_int_t *owner, retro_atomic_int_t *waiters, int me)
{
   retro_atomic_cas_int(owner, me, 0);
   /* given back before the count is read (a read that writes: not answered
    * from before the owner changed) */
   return retro_atomic_fetch_add_seq_cst_int(waiters, 0) != 0;
}
