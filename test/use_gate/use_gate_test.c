/* A resource used from any thread and now and then changed, with no lock
 * (src/util/helpers/use_gate.c - a USB device's handle): no use is ever
 * inside while a change has the resource, a change waits for the uses
 * already in, uses are turned away while it is closed or changing, changes
 * take turns, and a change that turns uses away just as the last one leaves
 * is still woken. C89. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/use_gate.h"

static int s_failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", \
   __FILE__, __LINE__, #c); s_failures++; } } while (0)

static use_gate_t s_gate;

static void test_single(void)
{
   CHECK(use_gate_init(&s_gate));
   CHECK(!use_gate_is_open(&s_gate));
   CHECK(!use_gate_enter(&s_gate)); /* closed */
   use_gate_change_begin(&s_gate);
   CHECK(!use_gate_enter(&s_gate)); /* changing */
   use_gate_change_end(&s_gate, true);
   CHECK(use_gate_is_open(&s_gate));
   CHECK(use_gate_enter(&s_gate));
   CHECK(use_gate_enter(&s_gate));
   use_gate_leave(&s_gate);
   use_gate_leave(&s_gate);
   CHECK(retro_atomic_load_acquire_int(&s_gate.state) == 0);
   use_gate_change_begin(&s_gate);
   use_gate_change_end(&s_gate, false);
   CHECK(!use_gate_is_open(&s_gate));
   CHECK(!use_gate_enter(&s_gate));
   CHECK(retro_atomic_load_acquire_int(&s_gate.state) == USE_GATE_CLOSED);
   use_gate_free(&s_gate);
}

/* a change does not have the resource while a use is still in: the change
 * started on another thread is still waiting a good while later, and goes
 * on once the use leaves */
static retro_atomic_int_t s_changed;
static void waiting_changer(void *arg)
{
   (void)arg;
   use_gate_change_begin(&s_gate);
   retro_atomic_store_release_int(&s_changed, 1);
   use_gate_change_end(&s_gate, true);
}

static void test_change_waits_for_use(void)
{
   sthread_t *t;
   long spin;
   CHECK(use_gate_init(&s_gate));
   use_gate_change_begin(&s_gate);
   use_gate_change_end(&s_gate, true);
   retro_atomic_int_init(&s_changed, 0);
   CHECK(use_gate_enter(&s_gate));
   t = sthread_create(waiting_changer, NULL);
   /* until the change has turned uses away, then a while longer */
   while (use_gate_is_open(&s_gate))
      ;
   for (spin = 0; spin < 20000000L; spin++)
      if (retro_atomic_load_acquire_int(&s_changed))
         break;
   CHECK(!retro_atomic_load_acquire_int(&s_changed));
   use_gate_leave(&s_gate);
   sthread_join(t);
   CHECK(retro_atomic_load_acquire_int(&s_changed));
   use_gate_free(&s_gate);
}

/* users and changers at once: the resource is a plain int a user reads and
 * a change writes; every use must see it whole and open */
#define USERS 3
#define CHANGERS 2
#define CHANGES 20000
static int s_resource;            /* plain: 1 open, 0 closed */
static int s_resource_twin;       /* plain: written with it */
static retro_atomic_int_t s_stop;
static retro_atomic_int_t s_inside_change;
static int s_bad_use;
static int s_bad_change;
static retro_atomic_int_t s_uses;

static void user(void *arg)
{
   (void)arg;
   while (!retro_atomic_load_acquire_int(&s_stop))
   {
      if (!use_gate_enter(&s_gate))
         continue;
      if (s_resource != 1 || s_resource_twin != 1)
         s_bad_use = 1;
      retro_atomic_fetch_add_int(&s_uses, 1);
      use_gate_leave(&s_gate);
   }
}

static void changer(void *arg)
{
   int i;
   (void)arg;
   for (i = 0; i < CHANGES; i++)
   {
      use_gate_change_begin(&s_gate);
      if (retro_atomic_exchange_int(&s_inside_change, 1) != 0)
         s_bad_change = 1;
      /* closed while it is changed: a use seeing this is the failure */
      s_resource = 0;
      s_resource_twin = 0;
      s_resource = 1;
      s_resource_twin = 1;
      retro_atomic_store_release_int(&s_inside_change, 0);
      use_gate_change_end(&s_gate, (i & 3) != 3);
   }
}

static void test_race(void)
{
   sthread_t *u[USERS], *c[CHANGERS];
   int i;
   CHECK(use_gate_init(&s_gate));
   retro_atomic_int_init(&s_stop, 0);
   retro_atomic_int_init(&s_inside_change, 0);
   retro_atomic_int_init(&s_uses, 0);
   s_resource = 1;
   s_resource_twin = 1;
   for (i = 0; i < USERS; i++)
      u[i] = sthread_create(user, NULL);
   for (i = 0; i < CHANGERS; i++)
      c[i] = sthread_create(changer, NULL);
   for (i = 0; i < CHANGERS; i++)
      sthread_join(c[i]);
   /* open again: the users get back in however few did during the race */
   {
      int before;
      use_gate_change_begin(&s_gate);
      use_gate_change_end(&s_gate, true);
      before = retro_atomic_load_acquire_int(&s_uses);
      while (retro_atomic_load_acquire_int(&s_uses) < before + USERS)
         ;
   }
   retro_atomic_store_release_int(&s_stop, 1);
   for (i = 0; i < USERS; i++)
      sthread_join(u[i]);
   CHECK(!s_bad_use);
   CHECK(!s_bad_change);
   CHECK(retro_atomic_load_acquire_int(&s_gate.state) == 0);
   use_gate_free(&s_gate);
}

/* A change waits for a use already in: the user is in, the change starts on
 * another thread, the user leaves - the change must then go on. Rounds of
 * it, each started together, so the last one out lands on every side of the
 * change's going to sleep; a change still asleep a few seconds after the
 * use left fails the test. */
#define LITMUS_ROUNDS 50000
static retro_atomic_int_t s_round;
static retro_atomic_int_t s_round_done;

static void litmus_changer(void *arg)
{
   int r;
   (void)arg;
   for (r = 1; r <= LITMUS_ROUNDS; r++)
   {
      while (retro_atomic_load_acquire_int(&s_round) < r)
         ;
      use_gate_change_begin(&s_gate);
      use_gate_change_end(&s_gate, true);
      retro_atomic_store_release_int(&s_round_done, r);
   }
}

static bool litmus(void)
{
   sthread_t *t;
   int r;
   CHECK(use_gate_init(&s_gate));
   use_gate_change_begin(&s_gate);
   use_gate_change_end(&s_gate, true);
   retro_atomic_int_init(&s_round, 0);
   retro_atomic_int_init(&s_round_done, 0);
   t = sthread_create(litmus_changer, NULL);
   for (r = 1; r <= LITMUS_ROUNDS; r++)
   {
      time_t start;
      CHECK(use_gate_enter(&s_gate));
      retro_atomic_store_release_int(&s_round, r);
      use_gate_leave(&s_gate);
      start = time(NULL);
      while (retro_atomic_load_acquire_int(&s_round_done) < r)
         if (time(NULL) - start > 2)
         {
            fprintf(stderr, "round %d: a change slept past the last use\n", r);
            return false;
         }
   }
   sthread_join(t);
   use_gate_free(&s_gate);
   return true;
}

/* A change waiting its turn behind another is woken when that one ends:
 * rounds of a second changer starting as the first ends */
static void turn_changer(void *arg)
{
   int r;
   (void)arg;
   for (r = 1; r <= LITMUS_ROUNDS; r++)
   {
      while (retro_atomic_load_acquire_int(&s_round) < r)
         ;
      use_gate_change_begin(&s_gate);
      use_gate_change_end(&s_gate, true);
      retro_atomic_store_release_int(&s_round_done, r);
   }
}

static bool litmus_turns(void)
{
   sthread_t *t;
   int r;
   CHECK(use_gate_init(&s_gate));
   retro_atomic_int_init(&s_round, 0);
   retro_atomic_int_init(&s_round_done, 0);
   t = sthread_create(turn_changer, NULL);
   for (r = 1; r <= LITMUS_ROUNDS; r++)
   {
      time_t start;
      use_gate_change_begin(&s_gate);
      retro_atomic_store_release_int(&s_round, r);
      use_gate_change_end(&s_gate, true);
      start = time(NULL);
      while (retro_atomic_load_acquire_int(&s_round_done) < r)
         if (time(NULL) - start > 2)
         {
            fprintf(stderr, "round %d: a change slept past its turn\n", r);
            return false;
         }
   }
   sthread_join(t);
   use_gate_free(&s_gate);
   return true;
}

/* a change that has turned uses away, waiting for the one still in, while
 * another change waits its turn - set up by the hook */
static int s_hook_ran;
static void hook_leave_one(use_gate_t *g)
{
   if (s_hook_ran++)
      return;
   /* the use that came in before the change leaves while it closes */
   CHECK(!use_gate_enter(g));
   use_gate_leave(g);
}

static void test_last_out_while_closing(void)
{
   CHECK(use_gate_init(&s_gate));
   use_gate_change_begin(&s_gate);
   use_gate_change_end(&s_gate, true);
   CHECK(use_gate_enter(&s_gate));
   s_hook_ran = 0;
   use_gate_test_closing = hook_leave_one;
   use_gate_change_begin(&s_gate);
   use_gate_test_closing = NULL;
   CHECK(s_hook_ran == 1);
   CHECK(retro_atomic_load_acquire_int(&s_gate.state) == USE_GATE_CHANGING);
   use_gate_change_end(&s_gate, true);
   CHECK(retro_atomic_load_acquire_int(&s_gate.state) == 0);
   use_gate_free(&s_gate);
}

int main(void)
{
   test_single();
   test_last_out_while_closing();
   test_change_waits_for_use();
   test_race();
   if (!litmus() || !litmus_turns())
   {
      fprintf(stderr, "use_gate: a change was not woken\n");
      return 1;
   }
   if (s_failures)
   {
      fprintf(stderr, "use_gate: %d failure(s)\n", s_failures);
      return 1;
   }
   printf("use_gate: ok\n");
   return 0;
}
