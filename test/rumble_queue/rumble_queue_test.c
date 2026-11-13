/* A GamePad's rumble patterns (src/input/rumble_queue.c), as VPADControlMotor
 * hands them over: two bits of a pattern a step, a step every 1/60 s, five
 * patterns at most, a clear that drops what was pushed before it (the one
 * playing included) and nothing after, a pattern pushed to an idle queue
 * starting at once - and, under TSan, game threads pushing and clearing
 * while the stepper plays, with no lock. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "input/rumble_queue.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define STEP RUMBLE_QUEUE_STEP_US

/* the steps a pattern plays: one step each 1/60 s from t */
static unsigned play(rumble_queue_t *q, int64_t *t, unsigned steps, char *out)
{
   unsigned i, on = 0;
   for (i = 0; i < steps; i++, *t += STEP)
   {
      const bool b = rumble_queue_step(q, *t);
      out[i] = b ? '1' : '0';
      on += b;
   }
   out[steps] = 0;
   return on;
}

static void test_steps(void)
{
   rumble_queue_t *q = rumble_queue_new();
   int64_t t = 1000000;
   char seq[128];
   /* 0b11, 0b00, 0b10, 0b01: either bit of a pair turns the step on */
   const uint8_t p[] = { 0x63 };
   const uint8_t full[15] = { 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xC0 };
   CHECK(q != NULL);
   CHECK(!rumble_queue_step(q, t));
   CHECK(rumble_queue_push(q, p, 8));
   CHECK(rumble_queue_count(q) == 1);
   play(q, &t, 5, seq);
   CHECK(strcmp(seq, "10110") == 0);
   CHECK(rumble_queue_count(q) == 0);
   /* within a step it holds */
   CHECK(rumble_queue_push(q, p, 8));
   CHECK(rumble_queue_step(q, t));
   CHECK(rumble_queue_step(q, t + STEP / 2));
   CHECK(!rumble_queue_step(q, t + STEP));
   /* a pattern cut at an odd number of bits: its last pair is one bit */
   rumble_queue_clear(q);
   t += 10 * STEP;
   CHECK(rumble_queue_push(q, (const uint8_t*)"\x40", 7));
   play(q, &t, 5, seq);
   CHECK(strcmp(seq, "00010") == 0);
   /* 120 bits: 60 steps, the first and the last on; more bits are cut */
   CHECK(rumble_queue_push(q, full, 200));
   play(q, &t, 61, seq);
   CHECK(seq[0] == '1' && seq[59] == '1' && seq[60] == '0' && strspn(seq + 1, "0") == 58);
   /* 0 bits, or none: a clear */
   CHECK(rumble_queue_push(q, p, 8));
   CHECK(rumble_queue_push(q, p, 0));
   CHECK(rumble_queue_count(q) == 0 && !rumble_queue_step(q, t));
   CHECK(rumble_queue_push(q, p, 8));
   CHECK(rumble_queue_push(q, NULL, 8));
   CHECK(!rumble_queue_step(q, t));
   rumble_queue_free(q);
}

static void test_limit_and_clear(void)
{
   rumble_queue_t *q = rumble_queue_new();
   int64_t t = 0;
   char seq[64];
   const uint8_t on[] = { 0xFF };
   const uint8_t off[] = { 0x00 };
   int i;
   for (i = 0; i < 5; i++)
      CHECK(rumble_queue_push(q, i == 4 ? off : on, 8));
   /* five queued: a sixth refused */
   CHECK(!rumble_queue_push(q, on, 8));
   CHECK(rumble_queue_count(q) == 5);
   /* played in order, one after another */
   play(q, &t, 20, seq);
   CHECK(strcmp(seq, "11111111111111110000") == 0);
   CHECK(rumble_queue_count(q) == 0 && !rumble_queue_step(q, t));

   /* a clear while one plays: it stops, and what is pushed after plays,
    * from its first step at once */
   CHECK(rumble_queue_push(q, on, 8));
   CHECK(rumble_queue_push(q, on, 8));
   play(q, &t, 2, seq);
   rumble_queue_clear(q);
   CHECK(rumble_queue_count(q) == 0);
   CHECK(rumble_queue_push(q, (const uint8_t*)"\x0C", 8));
   t -= STEP / 2; /* before the step the dropped pattern would take next */
   play(q, &t, 5, seq);
   CHECK(strcmp(seq, "01000") == 0);
   /* a clear with nothing pushed after it: silence */
   CHECK(rumble_queue_push(q, on, 8));
   rumble_queue_clear(q);
   CHECK(!rumble_queue_step(q, t));
   /* the limit counts again from the clear: five more */
   for (i = 0; i < 5; i++)
      CHECK(rumble_queue_push(q, on, 8));
   rumble_queue_clear(q);
   for (i = 0; i < 5; i++)
      CHECK(rumble_queue_push(q, on, 8));
   CHECK(!rumble_queue_push(q, on, 8));
   /* what the clears covered is still in the ring until the stepper drops
    * it: 10 of its 16 slots now, then 15, then all 16 - and a push the full
    * ring refuses gives its place in the count back */
   rumble_queue_clear(q);
   for (i = 0; i < 5; i++)
      CHECK(rumble_queue_push(q, on, 8));
   rumble_queue_clear(q);
   CHECK(rumble_queue_push(q, (const uint8_t*)"\x0C", 8));
   CHECK(!rumble_queue_push(q, on, 8));
   CHECK(rumble_queue_count(q) == 1);
   /* the stepper drops the 15 the clears covered and plays the last */
   play(q, &t, 4, seq);
   CHECK(strcmp(seq, "0100") == 0);
   CHECK(rumble_queue_count(q) == 0);
   /* and with the ring drained, five once more */
   for (i = 0; i < 5; i++)
      CHECK(rumble_queue_push(q, off, 8));
   CHECK(rumble_queue_count(q) == 5);
   rumble_queue_free(q);
}

/* positions through where an int wraps: a clear still covers what came
 * before it and nothing after */
static void test_wrap(void)
{
   rumble_queue_t *q = rumble_queue_new();
   int64_t t = 0;
   char seq[16];
   long i;
   /* one pushed at the last positive position, a clear, one pushed at the
    * first negative: the first is dropped, the second plays */
   rumble_queue_test_start_at(q, 0x7FFFFFFF);
   CHECK(rumble_queue_push(q, (const uint8_t*)"\xFF", 8));
   rumble_queue_clear(q);
   CHECK(rumble_queue_push(q, (const uint8_t*)"\x0C", 8));
   play(q, &t, 4, seq);
   CHECK(strcmp(seq, "0100") == 0);
   rumble_queue_test_start_at(q, 0x7FFFFFF0);
   CHECK(rumble_queue_push(q, (const uint8_t*)"\x03", 2));
   CHECK(rumble_queue_push(q, (const uint8_t*)"\x03", 2));
   play(q, &t, 1, seq);
   for (i = 0; i < 40; i++)
   {
      /* a clear, then one pushed after it that must play */
      rumble_queue_clear(q);
      CHECK(rumble_queue_push(q, (const uint8_t*)"\x03", 2));
      t += 100 * STEP;
      play(q, &t, 2, seq);
      CHECK(strcmp(seq, "10") == 0);
   }
   for (i = 0; i < 100000; i++)
   {
      CHECK(rumble_queue_push(q, (const uint8_t*)"\x01", 2));
      if (i % 7 == 0)
         rumble_queue_clear(q);
      play(q, &t, 2, seq);
      if (failures)
         break;
   }
   rumble_queue_free(q);
}

#define PUSHERS 4

static rumble_queue_t *s_q;
static retro_atomic_int_t s_stop;
static retro_atomic_int_t s_pushed;
static int64_t s_stepped_to; /* the stepper's time when it stopped */

static void pusher(void *ud)
{
   uint8_t pattern[2];
   unsigned i;
   pattern[0] = (uint8_t)(size_t)ud;
   pattern[1] = 0x55;
   for (i = 0; i < 20000; i++)
   {
      if (rumble_queue_push(s_q, pattern, 16))
         retro_atomic_fetch_add_int(&s_pushed, 1);
      if ((i & 63) == 0)
         rumble_queue_clear(s_q);
   }
}

static void stepper(void *unused)
{
   int64_t t = 0;
   (void)unused;
   while (!retro_atomic_load_acquire_int(&s_stop))
   {
      rumble_queue_step(s_q, t);
      t += STEP;
      if (rumble_queue_count(s_q) > RUMBLE_QUEUE_MAX)
         failures++;
   }
   s_stepped_to = t;
}

/* pushers racing for the last places: exactly five get in */
static retro_atomic_int_t s_ok;
static retro_atomic_int_t s_go;

static void racer(void *unused)
{
   (void)unused;
   /* yielding: eight of these on two cores, under TSan, would starve the
    * thread making the rest */
   while (!retro_atomic_load_acquire_int(&s_go))
      sthread_yield();
   if (rumble_queue_push(s_q, (const uint8_t*)"\x03", 2))
      retro_atomic_fetch_add_int(&s_ok, 1);
   if (rumble_queue_push(s_q, (const uint8_t*)"\x03", 2))
      retro_atomic_fetch_add_int(&s_ok, 1);
}

static void test_race_for_places(void)
{
   sthread_t *t[8];
   int round, i;
   for (round = 0; round < 300; round++)
   {
      s_q = rumble_queue_new();
      retro_atomic_int_init(&s_ok, 0);
      retro_atomic_int_init(&s_go, 0);
      for (i = 0; i < 8; i++)
         t[i] = sthread_create(racer, NULL);
      retro_atomic_store_release_int(&s_go, 1);
      for (i = 0; i < 8; i++)
         sthread_join(t[i]);
      CHECK(retro_atomic_load_acquire_int(&s_ok) == RUMBLE_QUEUE_MAX);
      CHECK(rumble_queue_count(s_q) == RUMBLE_QUEUE_MAX);
      rumble_queue_free(s_q);
      if (failures)
         break;
   }
}

static void test_threads(void)
{
   sthread_t *p[PUSHERS], *s;
   int64_t t = 0;
   int i;
   s_q = rumble_queue_new();
   retro_atomic_int_init(&s_stop, 0);
   retro_atomic_int_init(&s_pushed, 0);
   s = sthread_create(stepper, NULL);
   for (i = 0; i < PUSHERS; i++)
      p[i] = sthread_create(pusher, (void*)(size_t)(i + 1));
   for (i = 0; i < PUSHERS; i++)
      sthread_join(p[i]);
   retro_atomic_store_release_int(&s_stop, 1);
   sthread_join(s);
   CHECK(retro_atomic_load_acquire_int(&s_pushed) > 0);
   /* whatever was left plays out, and the queue comes back empty and usable:
    * on from the stepper's time, which only goes forward. A clear zeroes
    * the count at once and leaves its slots for the next step to drop, so
    * one step at least before the queue takes another */
   t = s_stepped_to;
   rumble_queue_step(s_q, t);
   for (i = 0; i < 2000 && rumble_queue_count(s_q); i++, t += STEP)
      rumble_queue_step(s_q, t);
   CHECK(rumble_queue_count(s_q) == 0);
   CHECK(rumble_queue_push(s_q, (const uint8_t*)"\x03", 2));
   CHECK(rumble_queue_step(s_q, t + 1000 * STEP));
   rumble_queue_free(s_q);
}

int main(void)
{
   test_steps();
   test_limit_and_clear();
   test_wrap();
   test_race_for_places();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "rumble_queue_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("rumble_queue_test: all passed\n");
   return 0;
}
