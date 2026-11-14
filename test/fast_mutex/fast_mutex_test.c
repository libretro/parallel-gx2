/* The guest's OSFastMutex without a lock of the host's (src/Cafe/OS/libs/
 * coreinit/fast_mutex.c), with a stand-in for the guest scheduler: a lock,
 * a count of sleepers and wake tokens. Taken by one at a time (TSan sees
 * the counter it guards touched by one thread at a time), recursive for
 * its owner, given back only by it - and no waiter left asleep: one about
 * to sleep as the mutex is given back either takes it or is woken, or the
 * run never ends and its timeout fails it. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/coreinit/fast_mutex.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static retro_atomic_int_t s_owner;
static retro_atomic_int_t s_waiters;

static void test_basic(void)
{
   retro_atomic_int_init(&s_owner, 0);
   retro_atomic_int_init(&s_waiters, 0);
   CHECK(!fast_mutex_owned(&s_owner, 7));
   CHECK(fast_mutex_take(&s_owner, 7));
   CHECK(fast_mutex_owned(&s_owner, 7));
   CHECK(!fast_mutex_take(&s_owner, 7));        /* held: recursion is the caller's count */
   CHECK(!fast_mutex_take(&s_owner, 9));
   CHECK(!fast_mutex_owned(&s_owner, 9));
   /* about to wait while held: counted, not taken */
   CHECK(!fast_mutex_wait_begin(&s_owner, &s_waiters, 9));
   CHECK(retro_atomic_load_acquire_int(&s_waiters) == 1);
   /* given back with one waiting: wake it */
   CHECK(fast_mutex_give(&s_owner, &s_waiters, 7));
   CHECK(!fast_mutex_owned(&s_owner, 7));
   fast_mutex_wait_end(&s_waiters);
   CHECK(retro_atomic_load_acquire_int(&s_waiters) == 0);
   /* free when the waiter looks a last time: taken, not counted */
   CHECK(fast_mutex_wait_begin(&s_owner, &s_waiters, 9));
   CHECK(fast_mutex_owned(&s_owner, 9));
   CHECK(retro_atomic_load_acquire_int(&s_waiters) == 0);
   /* given back with none waiting: none woken */
   CHECK(!fast_mutex_give(&s_owner, &s_waiters, 9));
   /* given back by one not holding it: nothing changes */
   CHECK(fast_mutex_take(&s_owner, 3));
   fast_mutex_give(&s_owner, &s_waiters, 4);
   CHECK(fast_mutex_owned(&s_owner, 3));
   fast_mutex_give(&s_owner, &s_waiters, 3);
}

/* the scheduler's stand-in */
static slock_t *s_sched;
static scond_t *s_cond;
static int s_sleepers;  /* under s_sched */
static int s_tokens;    /* under s_sched */

static int s_guarded;   /* touched only while holding the fast mutex */
static int s_depth[8];

#define THREADS 4
#define ROUNDS 20000

static void lock(int me)
{
   for (;;)
   {
      if (fast_mutex_take(&s_owner, me))
         break;
      if (fast_mutex_owned(&s_owner, me))
      {
         s_depth[me]++;
         return;
      }
      slock_lock(s_sched);
      if (fast_mutex_wait_begin(&s_owner, &s_waiters, me))
      {
         slock_unlock(s_sched);
         break;
      }
      s_sleepers++;
      while (!s_tokens)
         scond_wait(s_cond, s_sched);
      s_tokens--;
      fast_mutex_wait_end(&s_waiters);
      slock_unlock(s_sched);
   }
   s_depth[me] = 1;
}

static void unlock(int me)
{
   if (--s_depth[me])
      return;
   if (fast_mutex_give(&s_owner, &s_waiters, me))
   {
      slock_lock(s_sched);
      if (s_sleepers)
      {
         s_sleepers--;
         s_tokens++;
         scond_signal(s_cond);
      }
      slock_unlock(s_sched);
   }
}

static void worker(void *ud)
{
   int me = (int)(size_t)ud, i;
   for (i = 0; i < ROUNDS; i++)
   {
      lock(me);
      if (i & 1)
         lock(me);      /* recursive */
      s_guarded++;
      if (i & 1)
         unlock(me);
      unlock(me);
      if (!(i & 63))
         sthread_yield();
   }
}

static void test_threads(void)
{
   sthread_t *t[THREADS];
   int i;
   retro_atomic_int_init(&s_owner, 0);
   retro_atomic_int_init(&s_waiters, 0);
   s_sched = slock_new();
   s_cond  = scond_new();
   s_guarded = 0;
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)(i + 1));
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   CHECK(s_guarded == THREADS * ROUNDS);
   CHECK(retro_atomic_load_acquire_int(&s_owner) == 0);
   CHECK(retro_atomic_load_acquire_int(&s_waiters) == 0);
   CHECK(s_sleepers == 0);
   scond_free(s_cond);
   slock_free(s_sched);
}

/* The race as tight as it goes: the holder gives the mutex back while the
 * other is about to wait. Where a store can be passed by the load after it
 * (x86 too), the waiter's last look missing the mutex given back and the
 * giver missing the waiter both at once is a waiter asleep for good. */
#define SB_ROUNDS 200000
static retro_atomic_int_t s_sb_go, s_sb_done, s_sb_took;

static void sb_wait(retro_atomic_int_t *v, int r)
{
   unsigned spins = 0;
   while (retro_atomic_load_acquire_int(v) != r)
      if (!(++spins & 1023))
         sthread_yield();
}

static void sb_waiter(void *ud)
{
   int r;
   (void)ud;
   for (r = 1; r <= SB_ROUNDS; r++)
   {
      sb_wait(&s_sb_go, r);
      retro_atomic_store_relaxed_int(&s_sb_took,
            fast_mutex_wait_begin(&s_owner, &s_waiters, 2) ? 1 : 0);
      retro_atomic_store_release_int(&s_sb_done, r);
   }
}

static void test_store_buffering(void)
{
   sthread_t *t;
   int r, lost = 0;
   retro_atomic_int_init(&s_owner, 0);
   retro_atomic_int_init(&s_waiters, 0);
   retro_atomic_int_init(&s_sb_go, 0);
   retro_atomic_int_init(&s_sb_done, 0);
   t = sthread_create(sb_waiter, NULL);
   for (r = 1; r <= SB_ROUNDS; r++)
   {
      bool wake;
      CHECK(fast_mutex_take(&s_owner, 1));
      retro_atomic_store_release_int(&s_sb_go, r);
      wake = fast_mutex_give(&s_owner, &s_waiters, 1);
      sb_wait(&s_sb_done, r);
      if (retro_atomic_load_relaxed_int(&s_sb_took))
         fast_mutex_give(&s_owner, &s_waiters, 2);
      else
      {
         if (!wake)
            lost++;
         fast_mutex_wait_end(&s_waiters);
      }
   }
   sthread_join(t);
   CHECK(lost == 0);
}

int main(void)
{
   test_basic();
   test_threads();
   test_store_buffering();
   if (failures)
   {
      fprintf(stderr, "fast_mutex_test: %d failed\n", failures);
      return 1;
   }
   printf("fast_mutex_test: all passed\n");
   return 0;
}
