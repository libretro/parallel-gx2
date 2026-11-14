#ifndef COREINIT_FAST_MUTEX_H
#define COREINIT_FAST_MUTEX_H

/* The guest's OSFastMutex without a lock of the host's: the owner taken and
 * given back with a compare-and-swap; only a thread that must wait goes to
 * the scheduler, counted in waiters (the console's contendedState) before
 * its last look at the owner, while one giving the mutex back reads the
 * count after - so either the waiter finds it free or the giver wakes it.
 * The owner is the holder's id, 0 when free. The scheduler's side - the
 * queue, the sleep, the wake - is the caller's, under its own lock. C89. */

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* taken if free */
bool fast_mutex_take(retro_atomic_int_t *owner, int me);
bool fast_mutex_owned(retro_atomic_int_t *owner, int me);
/* Under the scheduler's lock, about to wait: counted, and one last try;
 * true when taken (counted no more) */
bool fast_mutex_wait_begin(retro_atomic_int_t *owner, retro_atomic_int_t *waiters, int me);
/* Under the scheduler's lock, woken: counted no more */
void fast_mutex_wait_end(retro_atomic_int_t *waiters);
/* Given back by its owner: true when a waiter must be woken (under the
 * scheduler's lock, the caller's) */
bool fast_mutex_give(retro_atomic_int_t *owner, retro_atomic_int_t *waiters, int me);

RETRO_END_DECLS

#endif
