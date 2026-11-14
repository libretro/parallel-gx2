/* An emulated toy portal's commands and reports with no lock
 * (src/Cafe/OS/libs/nsyshid/hid_portal.c): commands run one at a time, each
 * once, in each submitter's order, whoever runs them; a command run before
 * its submit counts it in is not run again and does not stop the next one;
 * reads take reports in order, sleep for one, and are woken by close. C89. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/nsyshid/hid_portal.h"

static int s_failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", \
   __FILE__, __LINE__, #c); s_failures++; } } while (0)

#define THREADS 4
#define PER_THREAD 20000

static hid_portal_t s_portal;
static retro_atomic_int_t s_inside;
static int s_ran;                     /* plain: only ever one runner */
static int s_last[THREADS];           /* plain, as above */
static int s_order_ok;
static int s_reported;

static void run_stress(void *ctx, const unsigned char *cmd, unsigned len)
{
   int who, seq;
   (void)ctx;
   if (retro_atomic_exchange_int(&s_inside, 1) != 0)
      s_order_ok = 0;
   who = cmd[0];
   seq = cmd[1] | (cmd[2] << 8) | (cmd[3] << 16);
   if (len != 4 || who >= THREADS || seq != s_last[who] + 1)
      s_order_ok = 0;
   else
      s_last[who] = seq;
   s_ran++;
   retro_atomic_store_release_int(&s_inside, 0);
}

static void submitter(void *arg)
{
   int who = (int)(size_t)arg;
   int seq;
   for (seq = 1; seq <= PER_THREAD; seq++)
   {
      unsigned char c[4];
      c[0] = (unsigned char)who;
      c[1] = (unsigned char)seq;
      c[2] = (unsigned char)(seq >> 8);
      c[3] = (unsigned char)(seq >> 16);
      /* a full queue has a runner busy with it: try again */
      while (!hid_portal_submit(&s_portal, c, sizeof(c)))
         ;
   }
}

static void test_one_runner(void)
{
   sthread_t *t[THREADS];
   int i;
   CHECK(hid_portal_init(&s_portal, run_stress, NULL));
   retro_atomic_int_init(&s_inside, 0);
   s_ran = 0;
   s_order_ok = 1;
   memset(s_last, 0, sizeof(s_last));
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(submitter, (void *)(size_t)i);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   CHECK(s_order_ok);
   CHECK(s_ran == THREADS * PER_THREAD);
   for (i = 0; i < THREADS; i++)
      CHECK(s_last[i] == PER_THREAD);
   CHECK(retro_atomic_load_acquire_int(&s_portal.pending) == 0);
   hid_portal_free(&s_portal);
}

/* each command's answer is its own first byte */
static void run_echo(void *ctx, const unsigned char *cmd, unsigned len)
{
   unsigned char r[HID_PORTAL_REPORT];
   (void)ctx;
   (void)len;
   memset(r, 0, sizeof(r));
   r[0] = cmd[0];
   if (hid_portal_report(&s_portal, r))
      s_reported++;
}

static void test_reports_in_order(void)
{
   unsigned char c[HID_PORTAL_REPORT + 8];
   unsigned char r[HID_PORTAL_REPORT];
   int i;
   CHECK(hid_portal_init(&s_portal, run_echo, NULL));
   s_reported = 0;
   CHECK(!hid_portal_try_read(&s_portal, r));
   memset(c, 0, sizeof(c));
   for (i = 1; i <= HID_PORTAL_SLOTS + 1; i++)
   {
      c[0] = (unsigned char)i;
      /* longer than a report: cut, not overrun */
      CHECK(hid_portal_submit(&s_portal, c, sizeof(c)));
   }
   /* the last found every report slot taken */
   CHECK(s_reported == HID_PORTAL_SLOTS);
   for (i = 1; i <= HID_PORTAL_SLOTS; i++)
   {
      CHECK(hid_portal_try_read(&s_portal, r));
      CHECK(r[0] == i);
   }
   CHECK(!hid_portal_try_read(&s_portal, r));
   hid_portal_free(&s_portal);
}

/* commands submitted from inside a run queue behind it and run after it,
 * by the same runner, until the queue is full */
static int s_nested_ran;
static int s_nested_refused;
static void run_nested(void *ctx, const unsigned char *cmd, unsigned len)
{
   (void)ctx;
   (void)len;
   if (cmd[0] == 0)
   {
      int i;
      unsigned char c[1];
      c[0] = 1;
      for (i = 0; i < HID_PORTAL_SLOTS + 4; i++)
         if (!hid_portal_submit(&s_portal, c, 1))
            s_nested_refused++;
      /* none of them ran yet: this one is still running */
      CHECK(s_nested_ran == 0);
   }
   else
      s_nested_ran++;
}

static void test_nested_submit(void)
{
   unsigned char c[1];
   CHECK(hid_portal_init(&s_portal, run_nested, NULL));
   s_nested_ran = 0;
   s_nested_refused = 0;
   c[0] = 0;
   CHECK(hid_portal_submit(&s_portal, c, 1));
   CHECK(s_nested_ran == HID_PORTAL_SLOTS);
   CHECK(s_nested_refused == 4);
   CHECK(retro_atomic_load_acquire_int(&s_portal.pending) == 0);
   hid_portal_free(&s_portal);
}

/* A submit is overtaken between queueing its command and counting it in:
 * another submit runs both. The late count must neither run anything again
 * nor leave the next command waiting. */
static int s_counts[3];
static int s_overtaken;
static void run_count(void *ctx, const unsigned char *cmd, unsigned len)
{
   (void)ctx;
   (void)len;
   s_counts[cmd[0]]++;
}

static void overtake(hid_portal_t *p)
{
   unsigned char c[1];
   if (s_overtaken)
      return;
   s_overtaken = 1;
   c[0] = 1;
   CHECK(hid_portal_submit(p, c, 1));
   /* both ran on this submit, the first not counted in yet */
   CHECK(s_counts[0] == 1 && s_counts[1] == 1);
   CHECK(retro_atomic_load_acquire_int(&p->pending) == -1);
}

static void test_overtaken_submit(void)
{
   unsigned char c[1];
   CHECK(hid_portal_init(&s_portal, run_count, NULL));
   memset(s_counts, 0, sizeof(s_counts));
   s_overtaken = 0;
   hid_portal_test_queued = overtake;
   c[0] = 0;
   CHECK(hid_portal_submit(&s_portal, c, 1));
   hid_portal_test_queued = NULL;
   CHECK(s_overtaken);
   CHECK(s_counts[0] == 1 && s_counts[1] == 1);
   CHECK(retro_atomic_load_acquire_int(&s_portal.pending) == 0);
   c[0] = 2;
   CHECK(hid_portal_submit(&s_portal, c, 1));
   CHECK(s_counts[0] == 1 && s_counts[1] == 1 && s_counts[2] == 1);
   CHECK(retro_atomic_load_acquire_int(&s_portal.pending) == 0);
   hid_portal_free(&s_portal);
}

/* a reader sleeping for a report: woken by each one, and by close */
#define HANDOFFS 20000
static retro_atomic_int_t s_got;
static retro_atomic_int_t s_read_done;
static int s_read_order_ok;

static void reader(void *arg)
{
   unsigned char r[HID_PORTAL_REPORT];
   int expect = 1;
   (void)arg;
   while (hid_portal_read(&s_portal, r))
   {
      int v = r[0] | (r[1] << 8);
      if (v != expect)
         s_read_order_ok = 0;
      expect++;
      retro_atomic_fetch_add_int(&s_got, 1);
   }
   retro_atomic_store_release_int(&s_read_done, 1);
}

static void test_read_wakes(void)
{
   sthread_t *t;
   unsigned char r[HID_PORTAL_REPORT];
   int i;
   CHECK(hid_portal_init(&s_portal, run_echo, NULL));
   retro_atomic_int_init(&s_got, 0);
   retro_atomic_int_init(&s_read_done, 0);
   s_read_order_ok = 1;
   t = sthread_create(reader, NULL);
   memset(r, 0, sizeof(r));
   for (i = 1; i <= HANDOFFS; i++)
   {
      r[0] = (unsigned char)i;
      r[1] = (unsigned char)(i >> 8);
      /* one at a time: the reader is asleep for most of them */
      CHECK(hid_portal_report(&s_portal, r));
      while (retro_atomic_load_acquire_int(&s_got) < i)
         ;
   }
   CHECK(!retro_atomic_load_acquire_int(&s_read_done));
   hid_portal_close(&s_portal);
   sthread_join(t);
   CHECK(retro_atomic_load_acquire_int(&s_read_done));
   CHECK(retro_atomic_load_acquire_int(&s_got) == HANDOFFS);
   CHECK(s_read_order_ok);
   /* closed: no wait; but what is queued is still read */
   r[0] = 7;
   r[1] = 0;
   CHECK(hid_portal_report(&s_portal, r));
   memset(r, 0, sizeof(r));
   CHECK(hid_portal_read(&s_portal, r) && r[0] == 7);
   CHECK(!hid_portal_read(&s_portal, r));
   hid_portal_free(&s_portal);
}

/* close from another thread while the reader sleeps, many times over */
#define CLOSES 500
static void close_reader(void *arg)
{
   unsigned char r[HID_PORTAL_REPORT];
   (void)arg;
   CHECK(!hid_portal_read(&s_portal, r));
}

static void test_close_wakes(void)
{
   int i;
   for (i = 0; i < CLOSES; i++)
   {
      sthread_t *t;
      CHECK(hid_portal_init(&s_portal, run_echo, NULL));
      t = sthread_create(close_reader, NULL);
      hid_portal_close(&s_portal);
      sthread_join(t);
      hid_portal_free(&s_portal);
   }
}

/* The window between a read finding nothing and its registering to sleep:
 * a report or a close landing there must still wake it. A reader and the
 * main thread start each round together, fifty thousand times; a
 * read that sleeps through its round fails the test after a second. */
#define LITMUS_ROUNDS 50000
static retro_atomic_int_t s_round;      /* the round the reader may start */
static retro_atomic_int_t s_round_done; /* the last round it finished */
static int s_litmus_close;

static void litmus_reader(void *arg)
{
   int r;
   (void)arg;
   for (r = 1; r <= LITMUS_ROUNDS; r++)
   {
      unsigned char out[HID_PORTAL_REPORT];
      while (retro_atomic_load_acquire_int(&s_round) < r)
         ;
      if (s_litmus_close)
         CHECK(!hid_portal_read(&s_portal, out));
      else
         CHECK(hid_portal_read(&s_portal, out) && out[0] == (unsigned char)r);
      retro_atomic_store_release_int(&s_round_done, r);
   }
}

static bool litmus(int close_it)
{
   sthread_t *t;
   unsigned char rep[HID_PORTAL_REPORT];
   int r;
   bool ok = true;
   s_litmus_close = close_it;
   retro_atomic_int_init(&s_round, 0);
   retro_atomic_int_init(&s_round_done, 0);
   CHECK(hid_portal_init(&s_portal, run_echo, NULL));
   t = sthread_create(litmus_reader, NULL);
   memset(rep, 0, sizeof(rep));
   for (r = 1; r <= LITMUS_ROUNDS && ok; r++)
   {
      time_t start;
      if (close_it)
         retro_atomic_store_release_int(&s_portal.closed, 0);
      retro_atomic_store_release_int(&s_round, r);
      if (close_it)
         hid_portal_close(&s_portal);
      else
      {
         rep[0] = (unsigned char)r;
         CHECK(hid_portal_report(&s_portal, rep));
      }
      start = time(NULL);
      while (retro_atomic_load_acquire_int(&s_round_done) < r)
         if (time(NULL) - start > 2)
         {
            fprintf(stderr, "round %d: a read slept through its %s\n", r,
                  close_it ? "close" : "report");
            ok = false;
            break;
         }
   }
   if (!ok)
      return false; /* the reader is asleep for good: leave it */
   sthread_join(t);
   hid_portal_free(&s_portal);
   return true;
}

int main(void)
{
   test_reports_in_order();
   test_nested_submit();
   test_overtaken_submit();
   test_one_runner();
   test_read_wakes();
   test_close_wakes();
   if (!litmus(0) || !litmus(1))
   {
      fprintf(stderr, "hid_portal: a read was not woken\n");
      return 1;
   }
   if (s_failures)
   {
      fprintf(stderr, "hid_portal: %d failure(s)\n", s_failures);
      return 1;
   }
   printf("hid_portal: ok\n");
   return 0;
}
