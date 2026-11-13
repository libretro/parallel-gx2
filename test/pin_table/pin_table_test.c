/* Objects held by key and freed by the last to let go
 * (src/util/helpers/pin_table.c, the H.264 decoder sessions): found by key,
 * held and put back, released once and only when nothing holds them, a
 * slot not given out again before; a lookup that finds a slot as its object
 * goes and another takes it is not handed the other - and, under TSan and
 * ASan, threads adding and taking out keys while others hold them, with no
 * lock. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/pin_table.h"

extern void (*pin_table_test_mid_lookup)(void);
extern void (*pin_table_test_mid_add)(void);

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

typedef struct obj
{
   retro_atomic_int_t alive;
   retro_atomic_int_t released;
} obj_t;

static retro_atomic_int_t s_releases;

static void on_release(void *p)
{
   obj_t *o = (obj_t*)p;
   retro_atomic_fetch_add_int(&s_releases, 1);
   retro_atomic_fetch_add_int(&o->released, 1);
   retro_atomic_store_release_int(&o->alive, 0);
}

static void obj_init(obj_t *o)
{
   retro_atomic_int_init(&o->alive, 1);
   retro_atomic_int_init(&o->released, 0);
}

#define SLOTS 8
static pin_slot_t s_slots[SLOTS];
static pin_table_t s_t;

static void reset(void (*release)(void *))
{
   memset(s_slots, 0, sizeof(s_slots));
   pin_table_init(&s_t, s_slots, SLOTS, release);
   retro_atomic_int_init(&s_releases, 0);
}

static void test_basic(void)
{
   static obj_t o[12];
   int i, a, b;
   reset(on_release);
   for (i = 0; i < 12; i++)
      obj_init(&o[i]);
   CHECK(!pin_table_add(&s_t, 0, &o[0]));
   CHECK(pin_table_add(&s_t, 7, &o[0]));
   CHECK(pin_table_add(&s_t, 8, &o[1]));
   CHECK(pin_table_get(&s_t, 0) == -1 && pin_table_get(&s_t, 9) == -1);
   a = pin_table_get(&s_t, 7);
   b = pin_table_get(&s_t, 7);
   CHECK(a == 0 && b == 0 && pin_table_obj(&s_t, a) == &o[0]);
   /* taken out while held twice: gone from lookups, released at the last put */
   CHECK(pin_table_remove(&s_t, 7));
   CHECK(!pin_table_remove(&s_t, 7));
   CHECK(pin_table_get(&s_t, 7) == -1);
   pin_table_put(&s_t, a);
   CHECK(retro_atomic_load_acquire_int(&o[0].released) == 0);
   /* its slot not given out while held */
   CHECK(pin_table_add(&s_t, 9, &o[2]));
   CHECK(pin_table_get(&s_t, 9) == 2);
   pin_table_put(&s_t, 2);
   pin_table_put(&s_t, b);
   CHECK(retro_atomic_load_acquire_int(&o[0].released) == 1);
   /* now it is */
   CHECK(pin_table_add(&s_t, 10, &o[3]));
   CHECK(pin_table_get(&s_t, 10) == 0);
   pin_table_put(&s_t, 0);
   /* not held: released at once */
   CHECK(pin_table_remove(&s_t, 8));
   CHECK(retro_atomic_load_acquire_int(&o[1].released) == 1);
   /* full */
   for (i = 0; i < SLOTS - 2; i++)
      CHECK(pin_table_add(&s_t, 100 + i, &o[4 + i]));
   CHECK(!pin_table_add(&s_t, 999, &o[11]));
   /* all out, each released once */
   pin_table_remove_all(&s_t);
   for (i = 0; i < 12; i++)
      CHECK(retro_atomic_load_acquire_int(&o[i].released) == (i < 4 + SLOTS - 2 ? 1 : 0));
   CHECK(retro_atomic_load_acquire_int(&s_releases) == SLOTS + 2);
   CHECK(pin_table_get(&s_t, 9) == -1 && pin_table_get(&s_t, 10) == -1);
   pin_table_remove_all(&s_t);
   CHECK(retro_atomic_load_acquire_int(&s_releases) == SLOTS + 2);
}

/* a lookup that has found its slot when the object goes and another takes
 * the slot */
static obj_t s_first, s_second;

static void swap_in(void)
{
   pin_table_test_mid_lookup = NULL;
   CHECK(pin_table_remove(&s_t, 5));
   CHECK(pin_table_add(&s_t, 6, &s_second));
}

static void test_lookup_race(void)
{
   int which;
   for (which = 0; which < 2; which++)
   {
      reset(on_release);
      obj_init(&s_first);
      obj_init(&s_second);
      CHECK(pin_table_add(&s_t, 5, &s_first));
      pin_table_test_mid_lookup = swap_in;
      if (which == 0)
         CHECK(pin_table_get(&s_t, 5) == -1);
      else
         CHECK(!pin_table_remove(&s_t, 5));
      pin_table_test_mid_lookup = NULL;
      /* the other is still there, held by no one */
      CHECK(retro_atomic_load_acquire_int(&s_second.released) == 0);
      CHECK(pin_table_get(&s_t, 6) == 0);
      pin_table_put(&s_t, 0);
      CHECK(pin_table_remove(&s_t, 6));
      CHECK(retro_atomic_load_acquire_int(&s_second.released) == 1);
   }
}

/* threads at once */
#define KEYS 6
/* another add takes the slot this one saw free: each gets its own */
static obj_t s_a, s_b;
static void add_other(void)
{
   pin_table_test_mid_add = NULL;
   CHECK(pin_table_add(&s_t, 21, &s_b));
}

static void test_add_race(void)
{
   int a, b;
   reset(on_release);
   obj_init(&s_a);
   obj_init(&s_b);
   pin_table_test_mid_add = add_other;
   CHECK(pin_table_add(&s_t, 20, &s_a));
   pin_table_test_mid_add = NULL;
   a = pin_table_get(&s_t, 20);
   b = pin_table_get(&s_t, 21);
   CHECK(a >= 0 && b >= 0 && a != b);
   CHECK(a >= 0 && pin_table_obj(&s_t, a) == &s_a);
   CHECK(b >= 0 && pin_table_obj(&s_t, b) == &s_b);
   pin_table_put(&s_t, a);
   pin_table_put(&s_t, b);
   pin_table_remove_all(&s_t);
   CHECK(retro_atomic_load_acquire_int(&s_releases) == 2);
}

/* a slot is not given out again while its object is being released */
static obj_t s_old, s_new;
static int s_reentry_slot;
static void add_in_release(void *p)
{
   on_release(p);
   if (p != &s_old)
      return;
   CHECK(pin_table_add(&s_t, 31, &s_new));
   s_reentry_slot = pin_table_get(&s_t, 31);
   pin_table_put(&s_t, s_reentry_slot);
}

static void test_release_reentry(void)
{
   reset(add_in_release);
   obj_init(&s_old);
   obj_init(&s_new);
   s_reentry_slot = -1;
   CHECK(pin_table_add(&s_t, 30, &s_old));
   CHECK(pin_table_remove(&s_t, 30));
   CHECK(s_reentry_slot == 1);
   CHECK(pin_table_get(&s_t, 30) == -1);
   pin_table_remove_all(&s_t);
   CHECK(retro_atomic_load_acquire_int(&s_releases) == 2);
}

static retro_atomic_int_t s_added;
static retro_atomic_int_t s_dead;
static retro_atomic_int_t s_stop;

static void free_release(void *p)
{
   retro_atomic_fetch_add_int(&s_releases, 1);
   free(p);
}

static void adder(void *ud)
{
   const uint32_t base = (uint32_t)(size_t)ud * 100;
   int i;
   for (i = 0; i < 20000; i++)
   {
      const uint32_t key = base + 1 + (uint32_t)(i % KEYS);
      if (!pin_table_remove(&s_t, key))
      {
         obj_t *o = (obj_t*)malloc(sizeof(*o));
         obj_init(o);
         if (pin_table_add(&s_t, key, o))
            retro_atomic_fetch_add_int(&s_added, 1);
         else
            free(o);
      }
   }
}

static void user(void *ud)
{
   unsigned x = (unsigned)(size_t)ud;
   while (!retro_atomic_load_acquire_int(&s_stop))
   {
      int slot;
      x = x * 1103515245u + 12345u;
      slot = pin_table_get(&s_t, 100 + (x >> 8) % 2 * 100 + 1 + (x >> 12) % KEYS);
      if (slot < 0)
         continue;
      /* freed would be caught by ASan here */
      if (!retro_atomic_load_acquire_int(&((obj_t*)pin_table_obj(&s_t, slot))->alive))
         retro_atomic_fetch_add_int(&s_dead, 1);
      pin_table_put(&s_t, slot);
   }
}

static void test_threads(void)
{
   sthread_t *a[2], *u[2];
   int i;
   reset(free_release);
   retro_atomic_int_init(&s_added, 0);
   retro_atomic_int_init(&s_dead, 0);
   retro_atomic_int_init(&s_stop, 0);
   for (i = 0; i < 2; i++)
      u[i] = sthread_create(user, (void*)(size_t)(i + 7));
   for (i = 0; i < 2; i++)
      a[i] = sthread_create(adder, (void*)(size_t)(i + 1));
   for (i = 0; i < 2; i++)
      sthread_join(a[i]);
   retro_atomic_store_release_int(&s_stop, 1);
   for (i = 0; i < 2; i++)
      sthread_join(u[i]);
   pin_table_remove_all(&s_t);
   CHECK(retro_atomic_load_acquire_int(&s_dead) == 0);
   CHECK(retro_atomic_load_acquire_int(&s_releases) == retro_atomic_load_acquire_int(&s_added));
}

int main(void)
{
   test_basic();
   test_lookup_race();
   test_add_race();
   test_release_reentry();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "pin_table_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("pin_table_test: all passed\n");
   return 0;
}
