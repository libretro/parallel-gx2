/* nlibcurl's transfer threads (curl_jobs.h) */

#include <stddef.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

#include "curl_jobs.h"

enum { CALLER = 1, WORKER = 2 };

typedef struct job
{
   retro_atomic_int_t held;     /* CALLER | WORKER; 0 free */
   retro_atomic_int_t gen;      /* the generation it was claimed in */
   retro_atomic_int_t answered;
   retro_atomic_int_t answer;
} job_t;

static job_t s_jobs[CURL_JOBS_SLOTS];
static retro_atomic_int_t s_gen;

typedef struct cell
{
   retro_atomic_int_t seq; /* kept less the cell's index */
   int slot;
} cell_t;

/* as many cells as slots: a claimed slot always fits */
static cell_t s_cells[CURL_JOBS_SLOTS];
static retro_atomic_int_t s_enqueue;
static retro_atomic_int_t s_dequeue;
static retro_atomic_int_t s_running;
static retro_atomic_int_t s_stop;
static retro_eventcount_t s_wake;
static sthread_t *s_workers[CURL_JOBS_WORKERS];
static void (*s_run)(int slot);

#ifdef CURL_JOBS_TEST_HOOKS
/* on a worker, between taking a job and looking whether it is retired */
void (*curl_jobs_test_taken)(void);
#endif

#define CELL(pos) ((unsigned)(pos) % CURL_JOBS_SLOTS)

static unsigned seq_of(int pos)
{
   return (unsigned)retro_atomic_load_acquire_int(&s_cells[CELL(pos)].seq) + CELL(pos);
}

static void set_seq(int pos, unsigned seq)
{
   retro_atomic_store_release_int(&s_cells[CELL(pos)].seq, (int)(seq - CELL(pos)));
}

static bool push(int slot)
{
   int pos;
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&s_enqueue);
      dif = (int)(seq_of(pos) - (unsigned)pos);
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&s_enqueue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return false;
   }
   s_cells[CELL(pos)].slot = slot;
   set_seq(pos, (unsigned)pos + 1);
   return true;
}

static int pop(void)
{
   int pos, slot;
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&s_dequeue);
      dif = (int)(seq_of(pos) - ((unsigned)pos + 1));
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&s_dequeue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return -1;
   }
   slot = s_cells[CELL(pos)].slot;
   set_seq(pos, (unsigned)pos + CURL_JOBS_SLOTS);
   return slot;
}

static bool queued(void)
{
   const int pos = retro_atomic_load_acquire_int(&s_dequeue);
   return seq_of(pos) == (unsigned)pos + 1;
}

/* who lets go: the last one frees the slot */
static void let_go(int slot, int who)
{
   job_t *j = &s_jobs[slot];
   int v = retro_atomic_load_acquire_int(&j->held);
   while (v & who)
   {
      if (retro_atomic_cas_int(&j->held, v, v & ~who))
         return;
      v = retro_atomic_load_acquire_int(&j->held);
   }
}

int curl_jobs_claim(void)
{
   int i;
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
   {
      job_t *j = &s_jobs[i];
      if (retro_atomic_load_acquire_int(&j->held) != 0
            || !retro_atomic_cas_int(&j->held, 0, CALLER | WORKER))
         continue;
      retro_atomic_store_release_int(&j->gen, retro_atomic_load_acquire_int(&s_gen));
      retro_atomic_store_release_int(&j->answered, 0);
      retro_atomic_store_release_int(&j->answer, 0);
      return i;
   }
   return -1;
}

bool curl_jobs_queue(int slot)
{
   if (!retro_atomic_load_acquire_int(&s_running) || !push(slot))
   {
      let_go(slot, WORKER);
      return false;
   }
   retro_eventcount_notify(&s_wake);
   return true;
}

void curl_jobs_release(int slot)
{
   let_go(slot, CALLER);
}

bool curl_jobs_live(int slot)
{
   return retro_atomic_load_acquire_int(&s_jobs[slot].gen)
      == retro_atomic_load_acquire_int(&s_gen);
}

void curl_jobs_retire(void)
{
   int i, slot;
   const int was = retro_atomic_fetch_add_int(&s_gen, 1);
   /* not begun: dropped */
   while ((slot = pop()) >= 0)
      let_go(slot, WORKER);
   /* their callers are gone */
   for (i = 0; i < CURL_JOBS_SLOTS; i++)
      if (retro_atomic_load_acquire_int(&s_jobs[i].held)
            && retro_atomic_load_acquire_int(&s_jobs[i].gen) == was)
         let_go(i, CALLER);
   /* the workers waiting on an answer */
   if (retro_atomic_load_acquire_int(&s_running))
      retro_eventcount_notify(&s_wake);
}

void curl_jobs_answer(int slot, uint32_t answer)
{
   job_t *j = &s_jobs[slot];
   retro_atomic_store_release_int(&j->answer, (int)answer);
   retro_atomic_store_release_int(&j->answered, 1);
   retro_eventcount_notify(&s_wake);
}

uint32_t curl_jobs_await(int slot)
{
   job_t *j = &s_jobs[slot];
   for (;;)
   {
      int key;
      if (retro_atomic_load_acquire_int(&j->answered))
      {
         retro_atomic_store_release_int(&j->answered, 0);
         return (uint32_t)retro_atomic_load_acquire_int(&j->answer);
      }
      if (!curl_jobs_live(slot))
         return 0;
      key = retro_eventcount_prepare_wait(&s_wake);
      if (retro_atomic_load_acquire_int(&j->answered) || !curl_jobs_live(slot))
         retro_eventcount_cancel_wait(&s_wake);
      else
         retro_eventcount_commit_wait(&s_wake, key);
   }
}

static void worker(void *unused)
{
   (void)unused;
   for (;;)
   {
      int key, slot;
      if (retro_atomic_load_acquire_int(&s_stop))
         return;
      if ((slot = pop()) >= 0)
      {
#ifdef CURL_JOBS_TEST_HOOKS
         if (curl_jobs_test_taken)
            curl_jobs_test_taken();
#endif
         if (curl_jobs_live(slot))
            s_run(slot);
         let_go(slot, WORKER);
         continue;
      }
      key = retro_eventcount_prepare_wait(&s_wake);
      if (retro_atomic_load_acquire_int(&s_stop) || queued())
         retro_eventcount_cancel_wait(&s_wake);
      else
         retro_eventcount_commit_wait(&s_wake, key);
   }
}

bool curl_jobs_start(void (*run)(int slot))
{
   int i;
   if (retro_atomic_load_acquire_int(&s_running) || !run)
      return false;
   if (!retro_eventcount_init(&s_wake))
      return false;
   s_run = run;
   retro_atomic_store_release_int(&s_stop, 0);
   for (i = 0; i < CURL_JOBS_WORKERS; i++)
      s_workers[i] = sthread_create(worker, NULL);
   retro_atomic_store_release_int(&s_running, 1);
   return true;
}

void curl_jobs_stop(void (*unblock)(void))
{
   int i;
   if (!retro_atomic_load_acquire_int(&s_running))
      return;
   retro_atomic_store_release_int(&s_stop, 1);
   curl_jobs_retire();
   retro_atomic_store_release_int(&s_running, 0);
   if (unblock)
      unblock();
   retro_eventcount_notify(&s_wake);
   for (i = 0; i < CURL_JOBS_WORKERS; i++)
   {
      if (s_workers[i])
         sthread_join(s_workers[i]);
      s_workers[i] = NULL;
   }
   retro_eventcount_free(&s_wake);
}
