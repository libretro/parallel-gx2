/* nlibcurl's transfer threads (src/Cafe/OS/libs/nlibcurl/curl_jobs.c):
 * slots claimed and let go of by caller and worker, an order run on a
 * worker with its callbacks answered by the caller, a retired title's jobs
 * not begun dropped and its callbacks answered 0, stopping unblocking what
 * the workers are in - and, under TSan, titles' threads ordering while the
 * workers run, with no lock. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/nlibcurl/curl_jobs.h"

extern void (*curl_jobs_test_taken)(void);

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* what an order is, per slot, as nlibcurl keeps it: the callbacks it asks
 * the caller for, the answers it gets, its result */
typedef struct order
{
   retro_atomic_int_t asks;     /* callbacks to ask for */
   retro_atomic_int_t asking;   /* 1: the worker waits on the caller */
   retro_atomic_int_t sum;      /* of the answers */
   retro_atomic_int_t done;
   retro_atomic_int_t ran;
} order_t;

static order_t s_orders[CURL_JOBS_SLOTS];
static retro_atomic_int_t s_runs;

static void run(int slot)
{
   order_t *o = &s_orders[slot];
   int i, sum = 0;
   const int asks = retro_atomic_load_acquire_int(&o->asks);
   retro_atomic_fetch_add_int(&s_runs, 1);
   retro_atomic_store_release_int(&o->ran, 1);
   for (i = 0; i < asks; i++)
   {
      retro_atomic_store_release_int(&o->asking, 1);
      sum += (int)curl_jobs_await(slot);
   }
   retro_atomic_store_release_int(&o->sum, sum);
   retro_atomic_store_release_int(&o->done, 1);
}

static void order_init(int slot, int asks)
{
   order_t *o = &s_orders[slot];
   retro_atomic_int_init(&o->asks, asks);
   retro_atomic_int_init(&o->asking, 0);
   retro_atomic_int_init(&o->sum, -1);
   retro_atomic_int_init(&o->done, 0);
   retro_atomic_int_init(&o->ran, 0);
}

/* the caller's side: answer each callback asked for, wait for the end */
static void serve(int slot, int base)
{
   order_t *o = &s_orders[slot];
   int n = 0;
   while (!retro_atomic_load_acquire_int(&o->done))
   {
      if (!retro_atomic_load_acquire_int(&o->asking))
         continue;
      retro_atomic_store_release_int(&o->asking, 0);
      curl_jobs_answer(slot, (uint32_t)(base + n++));
   }
}

static retro_atomic_int_t s_unblocked;

static void unblock(void)
{
   retro_atomic_fetch_add_int(&s_unblocked, 1);
}

static void test_slots(void)
{
   int i, s[CURL_JOBS_SLOTS];
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
      CHECK((s[i] = curl_jobs_claim()) == i);
   CHECK(curl_jobs_claim() == -1);
   /* no workers: the caller's alone, free once it lets go */
   CHECK(!curl_jobs_queue(s[3]));
   CHECK(curl_jobs_claim() == -1);
   curl_jobs_release(s[3]);
   CHECK(curl_jobs_claim() == 3);
   /* free only when both have let go */
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
      curl_jobs_release(i);
   CHECK(curl_jobs_claim() == -1);
   curl_jobs_retire(); /* the workers' holds stay: nothing was queued */
   CHECK(curl_jobs_claim() == -1);
}

static void test_orders(void)
{
   int slot, i;
   /* the slots test left every slot with a worker's hold and no queue */
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      CHECK(!curl_jobs_queue(i)); /* lets go of the worker's hold */
   }
   CHECK(curl_jobs_claim() == 0);
   CHECK(!curl_jobs_queue(0));
   curl_jobs_release(0);

   retro_atomic_int_init(&s_runs, 0);
   CHECK(!curl_jobs_start(NULL));
   CHECK(curl_jobs_start(run));
   CHECK(!curl_jobs_start(run));

   /* an order with three callbacks */
   slot = curl_jobs_claim();
   CHECK(slot == 0);
   order_init(slot, 3);
   CHECK(curl_jobs_live(slot));
   CHECK(curl_jobs_queue(slot));
   serve(slot, 10);
   CHECK(retro_atomic_load_acquire_int(&s_orders[slot].sum) == 10 + 11 + 12);
   curl_jobs_release(slot);
   /* another, with none */
   while ((slot = curl_jobs_claim()) < 0)
      ;
   order_init(slot, 0);
   CHECK(curl_jobs_queue(slot));
   serve(slot, 0);
   CHECK(retro_atomic_load_acquire_int(&s_orders[slot].sum) == 0);
   curl_jobs_release(slot);
}

static void test_retire(void)
{
   int busy[CURL_JOBS_WORKERS], later[4];
   int i, runs;
   /* every worker waiting on its caller, and four more queued */
   for (i = 0; i < CURL_JOBS_WORKERS; i++)
   {
      busy[i] = curl_jobs_claim();
      CHECK(busy[i] >= 0);
      order_init(busy[i], 2);
      CHECK(curl_jobs_queue(busy[i]));
   }
   for (i = 0; i < CURL_JOBS_WORKERS; i++)
      while (!retro_atomic_load_acquire_int(&s_orders[busy[i]].asking))
         ;
   for (i = 0; i < 4; i++)
   {
      later[i] = curl_jobs_claim();
      CHECK(later[i] >= 0);
      order_init(later[i], 1);
      CHECK(curl_jobs_queue(later[i]));
   }
   runs = retro_atomic_load_acquire_int(&s_runs);
   /* the title goes, its callers with it */
   curl_jobs_retire();
   for (i = 0; i < CURL_JOBS_WORKERS; i++)
   {
      CHECK(!curl_jobs_live(busy[i]));
      while (!retro_atomic_load_acquire_int(&s_orders[busy[i]].done))
         ;
      /* answered 0 */
      CHECK(retro_atomic_load_acquire_int(&s_orders[busy[i]].sum) == 0);
   }
   /* not begun: never run */
   for (i = 0; i < 4; i++)
      CHECK(!retro_atomic_load_acquire_int(&s_orders[later[i]].ran));
   CHECK(retro_atomic_load_acquire_int(&s_runs) == runs);
   /* and every slot free again: none held by a retired caller */
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      int s;
      while ((s = curl_jobs_claim()) < 0)
         ;
      order_init(s, 0);
   }
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      CHECK(curl_jobs_live(i));
      CHECK(curl_jobs_queue(i));
   }
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      serve(i, 0);
      curl_jobs_release(i);
   }
}

/* taken by a worker just as the title goes: not run */
static retro_atomic_int_t s_retired_now;

static void retire_now(void)
{
   curl_jobs_test_taken = NULL;
   curl_jobs_retire();
   retro_atomic_store_release_int(&s_retired_now, 1);
}

static void test_taken_retired(void)
{
   int slot, runs;
   while ((slot = curl_jobs_claim()) < 0)
      ;
   order_init(slot, 0);
   runs = retro_atomic_load_acquire_int(&s_runs);
   retro_atomic_int_init(&s_retired_now, 0);
   /* seen by the worker through the queue */
   curl_jobs_test_taken = retire_now;
   CHECK(curl_jobs_queue(slot));
   while (!retro_atomic_load_acquire_int(&s_retired_now))
      ;
   /* the worker let go; the retired caller's hold went with the title */
   while ((slot = curl_jobs_claim()) < 0)
      ;
   CHECK(retro_atomic_load_acquire_int(&s_runs) == runs);
   order_init(slot, 0);
   CHECK(curl_jobs_queue(slot));
   serve(slot, 0);
   curl_jobs_release(slot);
   CHECK(retro_atomic_load_acquire_int(&s_runs) == runs + 1);
}

/* an answer no one waited for is not the next job's */
static void test_stray_answer(void)
{
   int slot, i;
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      while ((slot = curl_jobs_claim()) < 0)
         ;
      order_init(slot, 0);
      curl_jobs_answer(slot, 77);
      CHECK(curl_jobs_queue(slot));
      serve(slot, 0);
      curl_jobs_release(slot);
   }
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      while ((slot = curl_jobs_claim()) < 0)
         ;
      order_init(slot, 1);
      CHECK(curl_jobs_queue(slot));
      serve(slot, 100);
      CHECK(retro_atomic_load_acquire_int(&s_orders[slot].sum) == 100);
      curl_jobs_release(slot);
   }
}

/* titles' threads ordering at once */
static void caller(void *ud)
{
   const int base = (int)(size_t)ud * 1000;
   int k;
   for (k = 0; k < 2000; k++)
   {
      const int slot = curl_jobs_claim();
      const int asks = k % 4;
      int want, j;
      if (slot < 0)
         continue;
      order_init(slot, asks);
      if (!curl_jobs_queue(slot))
      {
         curl_jobs_release(slot);
         continue;
      }
      serve(slot, base + k);
      for (want = 0, j = 0; j < asks; j++)
         want += base + k + j;
      if (retro_atomic_load_acquire_int(&s_orders[slot].sum) != want)
         failures++;
      curl_jobs_release(slot);
   }
}

static void test_threads(void)
{
   sthread_t *t[4];
   int i;
   for (i = 0; i < 4; i++)
      t[i] = sthread_create(caller, (void*)(size_t)(i + 1));
   for (i = 0; i < 4; i++)
      sthread_join(t[i]);
}

static void test_stop(void)
{
   int slot;
   /* a worker waiting on its caller when the core goes */
   slot = curl_jobs_claim();
   order_init(slot, 1);
   CHECK(curl_jobs_queue(slot));
   while (!retro_atomic_load_acquire_int(&s_orders[slot].asking))
      ;
   retro_atomic_int_init(&s_unblocked, 0);
   curl_jobs_stop(unblock);
   CHECK(retro_atomic_load_acquire_int(&s_unblocked) == 1);
   CHECK(retro_atomic_load_acquire_int(&s_orders[slot].done));
   CHECK(!curl_jobs_queue(curl_jobs_claim()));
   curl_jobs_stop(unblock);
   CHECK(retro_atomic_load_acquire_int(&s_unblocked) == 1);
}

int main(void)
{
   test_slots();
   test_orders();
   test_retire();
   test_taken_retired();
   test_stray_answer();
   test_threads();
   test_stop();
   if (failures)
   {
      fprintf(stderr, "curl_jobs_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("curl_jobs_test: all passed\n");
   return 0;
}
