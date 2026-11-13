/* The bounded lock-free queue (src/util/helpers/lf_ring.c) the core's
 * queues of fixed items sit on - GX2's events every frame, IOS timers and
 * requests, thread deallocators, pipelines to cache: empty in zeroed
 * storage, items of any size whole and in order, full refusing, laps round
 * it, its count - and, under TSan, threads pushing and popping at once,
 * with no lock. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/lf_ring.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

typedef struct odd { unsigned char b[3]; } odd_t;
typedef struct big { uint64_t a; uint32_t b; uint64_t c; } big_t;

static void test_sizes(void)
{
   CHECK(LF_RING_STRIDE(1) == 16 && LF_RING_STRIDE(8) == 16 && LF_RING_STRIDE(9) == 24);
   CHECK(LF_RING_STRIDE(sizeof(big_t)) % 8 == 0);
}

static void test_odd(void)
{
   static unsigned char cells[8 * LF_RING_STRIDE(sizeof(odd_t))];
   lf_ring_t r;
   odd_t in, out;
   unsigned i, lap;
   memset(cells, 0, sizeof(cells));
   lf_ring_init(&r, cells, 8, sizeof(odd_t));
   CHECK(!lf_ring_pop(&r, &out) && lf_ring_count(&r) == 0);
   for (lap = 0; lap < 5; lap++)
   {
      for (i = 0; i < 8; i++)
      {
         in.b[0] = (unsigned char)i;
         in.b[1] = (unsigned char)(i * 7);
         in.b[2] = (unsigned char)lap;
         CHECK(lf_ring_push(&r, &in));
      }
      CHECK(lf_ring_count(&r) == 8);
      CHECK(!lf_ring_push(&r, &in));
      for (i = 0; i < 8; i++)
      {
         CHECK(lf_ring_pop(&r, &out) && out.b[0] == i && out.b[1] == (unsigned char)(i * 7) && out.b[2] == lap);
         if (i == 2)
            CHECK(lf_ring_count(&r) == 5);
      }
      CHECK(!lf_ring_pop(&r, &out));
   }
   /* room again one at a time */
   for (i = 0; i < 100; i++)
   {
      in.b[0] = (unsigned char)i;
      CHECK(lf_ring_push(&r, &in));
      CHECK(lf_ring_pop(&r, &out) && out.b[0] == (unsigned char)i);
   }
}

static void test_big(void)
{
   static unsigned char cells[4 * LF_RING_STRIDE(sizeof(big_t))];
   lf_ring_t r;
   big_t in, out;
   unsigned i;
   memset(cells, 0, sizeof(cells));
   lf_ring_init(&r, cells, 4, sizeof(big_t));
   for (i = 0; i < 4; i++)
   {
      in.a = 0x0102030405060708ull * (i + 1);
      in.b = i;
      in.c = ~in.a;
      CHECK(lf_ring_push(&r, &in));
   }
   CHECK(!lf_ring_push(&r, &in));
   for (i = 0; i < 4; i++)
      CHECK(lf_ring_pop(&r, &out) && out.a == 0x0102030405060708ull * (i + 1)
            && out.b == i && out.c == ~out.a);
}

/* producers and consumers at once: every item out once */
#define PRODUCERS 3
#define CONSUMERS 3
#define PER 60000
static unsigned char s_cells[256 * LF_RING_STRIDE(8)];
static lf_ring_t s_ring;
static retro_atomic_int_t s_taken;
static retro_atomic_int_t s_seen[PRODUCERS];
static retro_atomic_int_t s_order_bad;

static void producer(void *ud)
{
   const uint32_t id = (uint32_t)(size_t)ud;
   uint32_t i;
   for (i = 0; i < PER; i++)
   {
      uint32_t item[2];
      item[0] = id;
      item[1] = i;
      while (!lf_ring_push(&s_ring, item))
         ;
   }
}

static void consumer(void *unused)
{
   uint32_t last[PRODUCERS];
   (void)unused;
   memset(last, 0xff, sizeof(last));
   while (retro_atomic_load_acquire_int(&s_taken) < PRODUCERS * PER)
   {
      uint32_t item[2];
      if (!lf_ring_pop(&s_ring, item))
         continue;
      retro_atomic_fetch_add_int(&s_taken, 1);
      if (item[0] >= PRODUCERS)
      {
         retro_atomic_fetch_add_int(&s_order_bad, 1);
         continue;
      }
      /* one producer's, as one consumer sees them: in order */
      if (last[item[0]] != 0xffffffffu && item[1] <= last[item[0]])
         retro_atomic_fetch_add_int(&s_order_bad, 1);
      last[item[0]] = item[1];
      retro_atomic_fetch_add_int(&s_seen[item[0]], (int)(item[1] & 0xff) + 1);
   }
}

static void test_threads(void)
{
   sthread_t *p[PRODUCERS], *c[CONSUMERS];
   int i;
   long want = 0;
   uint32_t k;
   memset(s_cells, 0, sizeof(s_cells));
   lf_ring_init(&s_ring, s_cells, 256, 8);
   retro_atomic_int_init(&s_taken, 0);
   retro_atomic_int_init(&s_order_bad, 0);
   for (i = 0; i < PRODUCERS; i++)
      retro_atomic_int_init(&s_seen[i], 0);
   for (i = 0; i < CONSUMERS; i++)
      c[i] = sthread_create(consumer, NULL);
   for (i = 0; i < PRODUCERS; i++)
      p[i] = sthread_create(producer, (void*)(size_t)i);
   for (i = 0; i < PRODUCERS; i++)
      sthread_join(p[i]);
   for (i = 0; i < CONSUMERS; i++)
      sthread_join(c[i]);
   for (k = 0; k < PER; k++)
      want += (long)(k & 0xff) + 1;
   CHECK(retro_atomic_load_acquire_int(&s_order_bad) == 0);
   for (i = 0; i < PRODUCERS; i++)
      CHECK(retro_atomic_load_acquire_int(&s_seen[i]) == want);
   CHECK(lf_ring_count(&s_ring) == 0);
}

int main(void)
{
   test_sizes();
   test_odd();
   test_big();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "lf_ring_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("lf_ring_test: all passed\n");
   return 0;
}
