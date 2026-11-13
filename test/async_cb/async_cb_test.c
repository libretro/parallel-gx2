/* coreinit's async callback queue (src/Cafe/OS/libs/coreinit/async_cb.c):
 * empty before any reset, callbacks taken in the order they came, a full
 * queue refusing, a reset dropping what a title left - and, under TSan,
 * host threads queueing while the callback thread takes, with no lock.
 * C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/coreinit/async_cb.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static async_cb_t make(uint32_t func, uint32_t a)
{
   async_cb_t cb;
   unsigned i;
   cb.func = func;
   cb.nparams = 8;
   for (i = 0; i < 8; i++)
      cb.params[i] = a + i;
   return cb;
}

static void test_order(void)
{
   async_cb_t cb, out;
   unsigned i, lap;
   /* never reset: empty, and usable */
   CHECK(!async_cb_pop(&out));
   cb = make(0x02000000, 10);
   CHECK(async_cb_push(&cb));
   CHECK(async_cb_pop(&out) && memcmp(&cb, &out, sizeof(cb)) == 0);
   CHECK(!async_cb_pop(&out));
   /* full, and laps round it */
   for (lap = 0; lap < 3; lap++)
   {
      for (i = 0; i < ASYNC_CB_SLOTS; i++)
      {
         cb = make(0x02000000 + i, i * 8);
         CHECK(async_cb_push(&cb));
      }
      cb = make(0x03000000, 0);
      CHECK(!async_cb_push(&cb));
      for (i = 0; i < ASYNC_CB_SLOTS; i++)
      {
         CHECK(async_cb_pop(&out) && out.func == 0x02000000 + i && out.params[7] == i * 8 + 7);
         if (i == 0)
         {
            /* room again once one is taken */
            cb = make(0x04000000, 0);
            CHECK(async_cb_push(&cb));
         }
      }
      CHECK(async_cb_pop(&out) && out.func == 0x04000000);
      CHECK(!async_cb_pop(&out));
   }
   /* a reset drops what is there */
   cb = make(0x05000000, 0);
   CHECK(async_cb_push(&cb) && async_cb_push(&cb));
   async_cb_reset();
   CHECK(!async_cb_pop(&out));
   cb = make(0x06000000, 0);
   CHECK(async_cb_push(&cb));
   CHECK(async_cb_pop(&out) && out.func == 0x06000000);
   CHECK(!async_cb_pop(&out));
}

#define PRODUCERS 4
#define PER 50000
static retro_atomic_int_t s_done;

static void producer(void *ud)
{
   const uint32_t id = (uint32_t)(size_t)ud;
   uint32_t i;
   for (i = 0; i < PER; i++)
   {
      async_cb_t cb = make(id, i);
      while (!async_cb_push(&cb))
         ;
   }
   retro_atomic_fetch_add_int(&s_done, 1);
}

static void test_threads(void)
{
   sthread_t *t[PRODUCERS];
   uint32_t next[PRODUCERS + 1];
   unsigned got = 0, bad = 0;
   int i;
   async_cb_reset();
   retro_atomic_int_init(&s_done, 0);
   memset(next, 0, sizeof(next));
   for (i = 0; i < PRODUCERS; i++)
      t[i] = sthread_create(producer, (void*)(size_t)(i + 1));
   while (got < PRODUCERS * PER)
   {
      async_cb_t out;
      if (!async_cb_pop(&out))
         continue;
      /* each producer's in its order, whole */
      if (out.func < 1 || out.func > PRODUCERS || out.params[0] != next[out.func]
            || out.params[7] != next[out.func] + 7)
         bad++;
      else
         next[out.func]++;
      got++;
   }
   for (i = 0; i < PRODUCERS; i++)
      sthread_join(t[i]);
   CHECK(bad == 0);
   {
      async_cb_t out;
      CHECK(!async_cb_pop(&out));
   }
}

int main(void)
{
   test_order();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "async_cb_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("async_cb_test: all passed\n");
   return 0;
}
