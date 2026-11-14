/* A resource used from any thread and now and then changed, with no lock
 * (use_gate.h) */

#include "use_gate.h"

#ifdef USE_GATE_TEST_HOOKS
void (*use_gate_test_closing)(use_gate_t *g);
#endif

bool use_gate_init(use_gate_t *g)
{
   retro_atomic_int_init(&g->state, USE_GATE_CLOSED);
   return retro_eventcount_init(&g->wake);
}

void use_gate_free(use_gate_t *g)
{
   retro_eventcount_free(&g->wake);
}

bool use_gate_enter(use_gate_t *g)
{
   const int v = retro_atomic_fetch_add_int(&g->state, 1);
   if (!(v & (USE_GATE_CLOSED | USE_GATE_CHANGING)))
      return true;
   use_gate_leave(g);
   return false;
}

void use_gate_leave(use_gate_t *g)
{
   const int v = retro_atomic_fetch_sub_int(&g->state, 1) - 1;
   /* the last one out of a resource being changed */
   if (!(v & USE_GATE_USERS) && (v & USE_GATE_CHANGING))
      retro_eventcount_notify(&g->wake);
}

/* sleeps until done(g) holds, rechecked after registering to sleep so a
 * notify in between is not missed */
static void use_gate_wait(use_gate_t *g, bool (*done)(int state))
{
   for (;;)
   {
      int key;
      if (done(retro_atomic_load_acquire_int(&g->state)))
         return;
      key = retro_eventcount_prepare_wait(&g->wake);
      if (done(retro_atomic_load_acquire_int(&g->state)))
      {
         retro_eventcount_cancel_wait(&g->wake);
         return;
      }
      retro_eventcount_commit_wait(&g->wake, key);
   }
}

static bool use_gate_no_change(int state)
{
   return !(state & USE_GATE_CHANGING);
}

static bool use_gate_no_users(int state)
{
   return !(state & USE_GATE_USERS);
}

void use_gate_change_begin(use_gate_t *g)
{
   for (;;)
   {
      int v = retro_atomic_load_acquire_int(&g->state);
      if (v & USE_GATE_CHANGING)
      {
         use_gate_wait(g, use_gate_no_change);
         continue;
      }
      if (retro_atomic_cas_int(&g->state, v, v | USE_GATE_CHANGING))
         break;
   }
#ifdef USE_GATE_TEST_HOOKS
   if (use_gate_test_closing)
      use_gate_test_closing(g);
#endif
   use_gate_wait(g, use_gate_no_users);
}

void use_gate_change_end(use_gate_t *g, bool open)
{
   for (;;)
   {
      const int v = retro_atomic_load_acquire_int(&g->state);
      int next    = v & ~USE_GATE_CHANGING;
      if (open)
         next &= ~USE_GATE_CLOSED;
      else
         next |= USE_GATE_CLOSED;
      /* uses turned away may still be leaving: only the flags change */
      if (retro_atomic_cas_int(&g->state, v, next))
         break;
   }
   retro_eventcount_notify(&g->wake); /* a change waiting its turn */
}

bool use_gate_is_open(use_gate_t *g)
{
   return !(retro_atomic_load_acquire_int(&g->state)
         & (USE_GATE_CLOSED | USE_GATE_CHANGING));
}
