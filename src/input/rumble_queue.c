/* A GamePad's rumble patterns (rumble_queue.h) */

#include <stdlib.h>

#include <retro_atomic.h>

#include "rumble_queue.h"

/* slots, a power of two: a clear empties the count at once and the stepper
 * drops what it covered later, so up to this many can be in the ring */
#define SLOTS 16

typedef struct slot
{
   retro_atomic_int_t seq;
   uint64_t steps;   /* step i is bit i */
   unsigned count;
} slot_t;

struct rumble_queue
{
   slot_t slots[SLOTS];
   retro_atomic_int_t enqueue;      /* next position a push takes */
   retro_atomic_int_t queued;       /* for the limit of RUMBLE_QUEUE_MAX */
   retro_atomic_int_t clear_before; /* positions below it are dropped */
   /* the stepper's alone */
   int dequeue;
   int playing_pos;
   bool playing;
   bool idle;
   bool on;
   unsigned bit;
   uint64_t steps;
   unsigned count;
   int64_t next_us;
};

/* a before b, positions wrapping */
static bool before(int a, int b)
{
   return (int)((unsigned)a - (unsigned)b) < 0;
}

rumble_queue_t *rumble_queue_new(void)
{
   rumble_queue_t *q = (rumble_queue_t*)calloc(1, sizeof(*q));
   int i;
   if (!q)
      return NULL;
   for (i = 0; i < SLOTS; i++)
      retro_atomic_int_init(&q->slots[i].seq, i);
   retro_atomic_int_init(&q->enqueue, 0);
   retro_atomic_int_init(&q->queued, 0);
   retro_atomic_int_init(&q->clear_before, 0);
   q->idle = true;
   return q;
}

void rumble_queue_free(rumble_queue_t *q)
{
   free(q);
}

/* one queued pattern done or dropped: never below zero, as a clear zeroes
 * the count before the stepper drops what it covered */
static void done_one(rumble_queue_t *q)
{
   int n = retro_atomic_load_acquire_int(&q->queued);
   while (n > 0 && !retro_atomic_cas_int(&q->queued, n, n - 1))
      n = retro_atomic_load_acquire_int(&q->queued);
}

bool rumble_queue_push(rumble_queue_t *q, const uint8_t *pattern, unsigned length)
{
   uint64_t steps = 0;
   unsigned count = 0, byte = 0;
   int len, n, pos;
   slot_t *s;
   if (!pattern || length == 0)
   {
      rumble_queue_clear(q);
      return true;
   }
   if (length > RUMBLE_QUEUE_MAX_BITS)
      length = RUMBLE_QUEUE_MAX_BITS;
   /* two bits a step, set if either is */
   for (len = (int)length; len > 0; byte++, len -= 8)
   {
      int j;
      for (j = 0; j < 8 && j < len; j += 2)
      {
         if (pattern[byte] & (3 << j))
            steps |= (uint64_t)1 << count;
         count++;
      }
   }
   /* the limit: one more queued, if there is room */
   n = retro_atomic_load_acquire_int(&q->queued);
   for (;;)
   {
      if (n >= RUMBLE_QUEUE_MAX)
         return false;
      if (retro_atomic_cas_int(&q->queued, n, n + 1))
         break;
      n = retro_atomic_load_acquire_int(&q->queued);
   }
   /* a slot: the one at the enqueue position, once the stepper has freed it */
   for (;;)
   {
      int seq, dif;
      pos = retro_atomic_load_acquire_int(&q->enqueue);
      s = &q->slots[(unsigned)pos & (SLOTS - 1)];
      seq = retro_atomic_load_acquire_int(&s->seq);
      dif = (int)((unsigned)seq - (unsigned)pos);
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&q->enqueue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
      {
         /* every slot still holds what a clear left for the stepper */
         done_one(q);
         return false;
      }
   }
   s->steps = steps;
   s->count = count;
   retro_atomic_store_release_int(&s->seq, (int)((unsigned)pos + 1));
   return true;
}

void rumble_queue_clear(rumble_queue_t *q)
{
   retro_atomic_store_release_int(&q->queued, 0);
   retro_atomic_store_release_int(&q->clear_before, retro_atomic_load_acquire_int(&q->enqueue));
}

unsigned rumble_queue_count(const rumble_queue_t *q)
{
   return (unsigned)retro_atomic_load_acquire_int((retro_atomic_int_t*)&q->queued);
}

/* the next pattern pushed and not cleared, if there is one */
static void take(rumble_queue_t *q, int clear_before)
{
   while (!q->playing)
   {
      const int pos = q->dequeue;
      slot_t *s = &q->slots[(unsigned)pos & (SLOTS - 1)];
      if (retro_atomic_load_acquire_int(&s->seq) != (int)((unsigned)pos + 1))
         return;
      q->steps = s->steps;
      q->count = s->count;
      retro_atomic_store_release_int(&s->seq, (int)((unsigned)pos + SLOTS));
      q->dequeue = (int)((unsigned)pos + 1);
      if (before(pos, clear_before))
      {
         q->idle = true;
         continue;
      }
      q->playing = true;
      q->playing_pos = pos;
      q->bit = 0;
      if (q->idle)
         q->next_us = 0;
      q->idle = false;
   }
}

bool rumble_queue_step(rumble_queue_t *q, int64_t now_us)
{
   const int clear_before = retro_atomic_load_acquire_int(&q->clear_before);
   /* cleared while it played: what is pushed next starts at once, as on
    * an idle queue */
   if (q->playing && before(q->playing_pos, clear_before))
   {
      q->playing = false;
      q->idle = true;
   }
   take(q, clear_before);
   if (!q->playing)
   {
      q->idle = true;
      q->on = false;
      return false;
   }
   if (now_us < q->next_us)
      return q->on;
   q->next_us = now_us + RUMBLE_QUEUE_STEP_US;
   q->on = ((q->steps >> q->bit) & 1) != 0;
   if (++q->bit >= q->count)
   {
      q->playing = false;
      done_one(q);
   }
   return q->on;
}

#ifdef RUMBLE_QUEUE_TEST_HOOKS
void rumble_queue_test_start_at(rumble_queue_t *q, int pos)
{
   int i;
   for (i = 0; i < SLOTS; i++)
   {
      const int p = (int)((unsigned)pos + (unsigned)i);
      retro_atomic_store_release_int(&q->slots[(unsigned)p & (SLOTS - 1)].seq, p);
   }
   retro_atomic_store_release_int(&q->enqueue, pos);
   retro_atomic_store_release_int(&q->clear_before, pos);
   q->dequeue = pos;
}
#endif
