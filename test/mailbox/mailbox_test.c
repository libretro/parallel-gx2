/* One slot from a writer to a reader with no lock (src/util/helpers/
 * mailbox.c, an amiibo touched on the host's side for the title's): taken
 * once, a later put replacing one not taken, no put into one being read -
 * and, under TSan and ASan, a writer and readers at once never seeing a
 * slot half written. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/mailbox.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static mailbox_t s_m;

static void test_basic(void)
{
   mailbox_init(&s_m);
   CHECK(!mailbox_begin_take(&s_m));                 /* empty */
   CHECK(mailbox_begin_put(&s_m));
   CHECK(!mailbox_begin_put(&s_m));                  /* being filled */
   CHECK(!mailbox_begin_take(&s_m));                 /* not yet full */
   mailbox_end_put(&s_m);
   CHECK(mailbox_begin_put(&s_m));                   /* full, not taken: replaced */
   mailbox_end_put(&s_m);
   CHECK(mailbox_begin_take(&s_m));
   CHECK(!mailbox_begin_take(&s_m));                 /* being read */
   CHECK(!mailbox_begin_put(&s_m));                  /* not into one being read */
   mailbox_end_take(&s_m);
   CHECK(!mailbox_begin_take(&s_m));                 /* taken once */
   CHECK(mailbox_begin_put(&s_m));
   mailbox_end_put(&s_m);
   CHECK(mailbox_begin_take(&s_m));
   mailbox_end_take(&s_m);
}

/* a writer filling a slot with one value throughout, readers checking it
 * is one value throughout */
#define WORDS 64
#define PUTS 200000
static unsigned s_slot[WORDS];
static retro_atomic_int_t s_done;
static retro_atomic_int_t s_torn;
static retro_atomic_int_t s_taken;
static retro_atomic_int_t s_last;

static void writer(void *ud)
{
   unsigned v;
   (void)ud;
   for (v = 1; v <= PUTS; v++)
   {
      int i;
      if (!mailbox_begin_put(&s_m))
         continue;
      for (i = 0; i < WORDS; i++)
         s_slot[i] = v;
      mailbox_end_put(&s_m);
   }
   retro_atomic_store_release_int(&s_done, 1);
}

static void reader(void *ud)
{
   (void)ud;
   for (;;)
   {
      int i;
      unsigned first;
      /* done read before the take: a take that fails after it is the end */
      const int done = retro_atomic_load_acquire_int(&s_done);
      if (!mailbox_begin_take(&s_m))
      {
         if (done)
            return;
         sthread_yield();
         continue;
      }
      first = s_slot[0];
      for (i = 1; i < WORDS; i++)
         if (s_slot[i] != first)
            retro_atomic_fetch_add_int(&s_torn, 1);
      /* never one older than one taken before */
      if ((int)first < retro_atomic_load_acquire_int(&s_last))
         retro_atomic_fetch_add_int(&s_torn, 1);
      retro_atomic_store_release_int(&s_last, (int)first);
      mailbox_end_take(&s_m);
      retro_atomic_fetch_add_int(&s_taken, 1);
   }
}

static void test_threads(void)
{
   sthread_t *w, *r1, *r2;
   mailbox_init(&s_m);
   retro_atomic_int_init(&s_done, 0);
   retro_atomic_int_init(&s_torn, 0);
   retro_atomic_int_init(&s_taken, 0);
   retro_atomic_int_init(&s_last, 0);
   r1 = sthread_create(reader, NULL);
   r2 = sthread_create(reader, NULL);
   w  = sthread_create(writer, NULL);
   sthread_join(w);
   sthread_join(r1);
   sthread_join(r2);
   CHECK(retro_atomic_load_acquire_int(&s_torn) == 0);
   CHECK(retro_atomic_load_acquire_int(&s_taken) > 0);
   CHECK(!mailbox_begin_take(&s_m));                 /* all taken */
}

int main(void)
{
   test_basic();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "mailbox_test: %d failed\n", failures);
      return 1;
   }
   printf("mailbox_test: all passed\n");
   return 0;
}
