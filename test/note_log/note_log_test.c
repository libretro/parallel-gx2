/* Notes from one writer to any readers with no lock
 * (src/util/helpers/note_log.c): read in order, each once per reader, the
 * ones written over skipped, and never one half written - with readers
 * racing a writer that wraps the log many times over. C89. */

#include <stdio.h>
#include <string.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/note_log.h"

static int s_failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", \
   __FILE__, __LINE__, #c); s_failures++; } } while (0)

static note_log_t s_log;

static void note_for(int id, int32_t w[NOTE_LOG_WORDS])
{
   int j;
   for (j = 0; j < NOTE_LOG_WORDS; j++)
      w[j] = (int32_t)(id * 7 + j * 1000003);
}

static bool note_is(int id, const int32_t w[NOTE_LOG_WORDS])
{
   int32_t want[NOTE_LOG_WORDS];
   note_for(id, want);
   return memcmp(want, w, sizeof(want)) == 0;
}

static void test_in_order(void)
{
   int32_t w[NOTE_LOG_WORDS];
   int prev = 0, other = 0, i;
   note_log_init(&s_log);
   CHECK(!note_log_next(&s_log, &prev, w));
   for (i = 1; i <= 5; i++)
   {
      note_for(i, w);
      CHECK(note_log_post(&s_log, w) == i);
   }
   for (i = 1; i <= 5; i++)
   {
      CHECK(note_log_next(&s_log, &prev, w));
      CHECK(prev == i && note_is(i, w));
   }
   CHECK(!note_log_next(&s_log, &prev, w));
   /* a second reader reads them all too */
   CHECK(note_log_next(&s_log, &other, w) && other == 1 && note_is(1, w));
}

static void test_written_over(void)
{
   int32_t w[NOTE_LOG_WORDS];
   int prev = 0, i;
   note_log_init(&s_log);
   for (i = 1; i <= NOTE_LOG_SLOTS * 3 + 5; i++)
   {
      note_for(i, w);
      note_log_post(&s_log, w);
   }
   /* the oldest kept, not one written over */
   CHECK(note_log_next(&s_log, &prev, w));
   CHECK(prev == NOTE_LOG_SLOTS * 2 + 6 && note_is(prev, w));
   for (i = prev + 1; i <= NOTE_LOG_SLOTS * 3 + 5; i++)
      CHECK(note_log_next(&s_log, &prev, w) && prev == i && note_is(i, w));
   CHECK(!note_log_next(&s_log, &prev, w));
}

#define POSTS 400000
#define READERS 3
static retro_atomic_int_t s_done;
static int s_reader_ok[READERS];
static int s_reader_got[READERS];

static void reader(void *arg)
{
   int me = (int)(size_t)arg;
   int prev = 0, finished = 0;
   int32_t w[NOTE_LOG_WORDS];
   s_reader_ok[me] = 1;
   for (;;)
   {
      int before = prev;
      if (note_log_next(&s_log, &prev, w))
      {
         if (prev <= before || !note_is(prev, w))
            s_reader_ok[me] = 0;
         s_reader_got[me]++;
         continue;
      }
      if (finished)
         break;
      /* one last pass once the writer is done: all it posted is seen */
      finished = retro_atomic_load_acquire_int(&s_done);
   }
   if (prev != POSTS)
      s_reader_ok[me] = 0;
}

static void test_race(void)
{
   sthread_t *t[READERS];
   int i;
   note_log_init(&s_log);
   retro_atomic_int_init(&s_done, 0);
   for (i = 0; i < READERS; i++)
      t[i] = sthread_create(reader, (void *)(size_t)i);
   for (i = 1; i <= POSTS; i++)
   {
      int32_t w[NOTE_LOG_WORDS];
      note_for(i, w);
      note_log_post(&s_log, w);
   }
   retro_atomic_store_release_int(&s_done, 1);
   for (i = 0; i < READERS; i++)
   {
      sthread_join(t[i]);
      CHECK(s_reader_ok[i]);
      CHECK(s_reader_got[i] > 0);
   }
}

int main(void)
{
   test_in_order();
   test_written_over();
   test_race();
   if (s_failures)
   {
      fprintf(stderr, "note_log: %d failure(s)\n", s_failures);
      return 1;
   }
   printf("note_log: ok\n");
   return 0;
}
