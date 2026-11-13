/* The camera library's state (src/Cafe/OS/libs/camera/cam_state.c): handles
 * taken lowest first and given back, surfaces handed back to an open
 * camera's handler one a round in the order they came, none of what a
 * handle had queued handed to whoever takes it next or to the next title,
 * a full queue refusing - and, under TSan, a title's threads submitting,
 * opening and giving handles back while the alarm hands surfaces over, with
 * no lock. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/camera/cam_state.h"

extern void (*cam_state_test_mid_init)(void);
extern void (*cam_state_test_mid_take)(void);

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* one alarm round: how many frames, and the last's surface */
static unsigned round_of(uint32_t *handlers, uint32_t *surfaces, unsigned max)
{
   int cursor = 0;
   unsigned n = 0;
   uint32_t h, s, z;
   while (cam_state_next(&cursor, &h, &s, &z))
   {
      if (n < max)
      {
         handlers[n] = h;
         surfaces[n] = s;
      }
      n++;
   }
   return n;
}

/* before any reset, as the first title finds it: an empty queue, past one
 * lap of it */
static void test_unreset(void)
{
   uint32_t h[8], s[8];
   bool first = false;
   int a;
   unsigned i, n = 0;
   a = cam_state_init(1, 1, 0x02000000, &first);
   CHECK(a == 1 && first);
   CHECK(cam_state_open(a));
   for (i = 0; i < CAM_STATE_QUEUE + 3; i++)
   {
      CHECK(cam_state_submit(a, 0x10000000 + i, 1) == CAM_SUBMIT_QUEUED);
      CHECK(round_of(h, s, 8) == 1 && s[0] == 0x10000000 + i);
      n++;
   }
   CHECK(n == CAM_STATE_QUEUE + 3);
}

static void test_handles(void)
{
   bool first = false;
   int a, b, c, i;
   cam_state_reset();
   a = cam_state_init(640, 480, 0x02000000, &first);
   CHECK(a == 1 && first);
   b = cam_state_init(640, 480, 0x02000100, &first);
   CHECK(b == 2 && !first);
   /* the lowest free one again */
   CHECK(cam_state_exit(a));
   CHECK(!cam_state_exit(a));
   c = cam_state_init(320, 240, 0x02000200, &first);
   CHECK(c == 1 && !first);
   /* no more than there are */
   for (i = 3; i <= CAM_STATE_HANDLES; i++)
      CHECK(cam_state_init(1, 1, 0x02000300, &first) == i);
   CHECK(cam_state_init(1, 1, 0x02000300, &first) == 0);
   /* handles out of range or not taken */
   CHECK(!cam_state_open(0) && !cam_state_open(CAM_STATE_HANDLES + 1) && !cam_state_open(-3));
   CHECK(cam_state_submit(0, 1, 1) == CAM_SUBMIT_BAD_HANDLE);
   CHECK(cam_state_exit(b));
   CHECK(!cam_state_open(b) && !cam_state_close(b));
   CHECK(cam_state_submit(b, 1, 1) == CAM_SUBMIT_BAD_HANDLE);
   /* a reset: everything free, the next init is the first */
   cam_state_reset();
   CHECK(cam_state_init(1, 1, 0x02000000, &first) == 1 && first);
}

static void test_surfaces(void)
{
   uint32_t h[8], s[8];
   bool first;
   int a, b;
   unsigned i;
   cam_state_reset();
   a = cam_state_init(640, 480, 0x02000000, &first);
   b = cam_state_init(640, 480, 0x02000100, &first);
   /* queued while closed: kept, not handed over */
   CHECK(cam_state_submit(a, 0x10000000, 100) == CAM_SUBMIT_QUEUED);
   CHECK(cam_state_submit(a, 0x10001000, 100) == CAM_SUBMIT_QUEUED);
   CHECK(cam_state_submit(b, 0x20000000, 50) == CAM_SUBMIT_QUEUED);
   CHECK(round_of(h, s, 8) == 0);
   CHECK(cam_state_open(a) && cam_state_open(a) && cam_state_open(b));
   /* one each a round, in order, to each one's handler */
   CHECK(round_of(h, s, 8) == 2);
   CHECK(h[0] == 0x02000000 && s[0] == 0x10000000 && h[1] == 0x02000100 && s[1] == 0x20000000);
   CHECK(round_of(h, s, 8) == 1 && s[0] == 0x10001000);
   CHECK(round_of(h, s, 8) == 0);
   /* closed: held back until it opens again */
   CHECK(cam_state_close(a) && cam_state_close(a));
   CHECK(cam_state_submit(a, 0x10002000, 100) == CAM_SUBMIT_QUEUED);
   CHECK(round_of(h, s, 8) == 0);
   CHECK(cam_state_open(a));
   CHECK(round_of(h, s, 8) == 1 && s[0] == 0x10002000);

   /* what a handle had queued is not handed to the next holder */
   CHECK(cam_state_submit(a, 0x10003000, 100) == CAM_SUBMIT_QUEUED);
   CHECK(cam_state_submit(a, 0x10004000, 100) == CAM_SUBMIT_QUEUED);
   CHECK(cam_state_exit(a));
   CHECK(cam_state_init(640, 480, 0x03000000, &first) == a);
   CHECK(cam_state_open(a));
   CHECK(round_of(h, s, 8) == 0);
   CHECK(cam_state_submit(a, 0x30000000, 100) == CAM_SUBMIT_QUEUED);
   CHECK(round_of(h, s, 8) == 1 && h[0] == 0x03000000 && s[0] == 0x30000000);

   /* full: refused, and room again once the alarm has taken one */
   for (i = 0; i < CAM_STATE_QUEUE; i++)
      CHECK(cam_state_submit(b, 0x40000000 + i, 1) == CAM_SUBMIT_QUEUED);
   CHECK(cam_state_submit(b, 0x50000000, 1) == CAM_SUBMIT_FULL);
   CHECK(round_of(h, s, 8) == 1 && s[0] == 0x40000000);
   CHECK(cam_state_submit(b, 0x50000000, 1) == CAM_SUBMIT_QUEUED);
   for (i = 1; i < CAM_STATE_QUEUE; i++)
      CHECK(round_of(h, s, 8) == 1 && s[0] == 0x40000000 + i);
   CHECK(round_of(h, s, 8) == 1 && s[0] == 0x50000000);

   /* the next title: nothing of this one's, and its own handed over */
   CHECK(cam_state_submit(b, 0x60000000, 1) == CAM_SUBMIT_QUEUED);
   CHECK(cam_state_submit(b, 0x60001000, 1) == CAM_SUBMIT_QUEUED);
   cam_state_reset();
   CHECK(round_of(h, s, 8) == 0);
   a = cam_state_init(1, 1, 0x04000000, &first);
   CHECK(a == 1);
   b = cam_state_init(1, 1, 0x04000100, &first);
   CHECK(cam_state_open(a) && cam_state_open(b) && round_of(h, s, 8) == 0);
   CHECK(cam_state_submit(b, 0x70000000, 1) == CAM_SUBMIT_QUEUED);
   CHECK(round_of(h, s, 8) == 1 && h[0] == 0x04000100 && s[0] == 0x70000000);
}

/* the alarm with a surface in hand when its handle is given back and taken
 * again, and the new handler in before the take is finished: a thread for
 * the alarm, stepped through it with flags */
static retro_atomic_int_t s_step;
static unsigned s_mid_frames;
static uint32_t s_mid_handler;

static void wait_step(int n)
{
   while (retro_atomic_load_acquire_int(&s_step) < n)
      ;
}

static void mid_take(void)
{
   retro_atomic_store_release_int(&s_step, 1);
   wait_step(2);
}

static void mid_init(void)
{
   retro_atomic_store_release_int(&s_step, 2);
   wait_step(3);
}

static void retaken_alarm(void *unused)
{
   uint32_t h[8], s[8];
   (void)unused;
   s_mid_frames = round_of(h, s, 8);
   s_mid_handler = s_mid_frames ? h[0] : 0;
   retro_atomic_store_release_int(&s_step, 3);
}

static void test_retaken(void)
{
   uint32_t h[8], s[8];
   sthread_t *t;
   bool first;
   int a;
   cam_state_reset();
   retro_atomic_int_init(&s_step, 0);
   a = cam_state_init(1, 1, 0x02000000, &first);
   CHECK(cam_state_open(a));
   CHECK(cam_state_submit(a, 0x10000000, 1) == CAM_SUBMIT_QUEUED);
   cam_state_test_mid_take = mid_take;
   cam_state_test_mid_init = mid_init;
   t = sthread_create(retaken_alarm, NULL);
   wait_step(1);
   CHECK(cam_state_exit(a));
   CHECK(cam_state_init(1, 1, 0x03000000, &first) == a);
   sthread_join(t);
   cam_state_test_mid_take = NULL;
   cam_state_test_mid_init = NULL;
   /* not the old holder's surface to the new holder's handler */
   CHECK(s_mid_frames == 0 && s_mid_handler == 0);
   CHECK(cam_state_open(a) && round_of(h, s, 8) == 0);
}

static retro_atomic_int_t s_stop;
static retro_atomic_int_t s_bad;

static void title_thread(void *ud)
{
   const uint32_t id = (uint32_t)(size_t)ud;
   unsigned i;
   bool first;
   for (i = 0; i < 3000; i++)
   {
      const int h = cam_state_init(1, 1, 0x02000000 + id * 0x100, &first);
      unsigned j;
      if (!h)
         continue;
      cam_state_open(h);
      for (j = 0; j < 8; j++)
         cam_state_submit(h, (id << 24) | (i << 4) | j, 1);
      if (i & 1)
         cam_state_close(h);
      cam_state_exit(h);
   }
}

static void alarm_thread(void *unused)
{
   (void)unused;
   while (!retro_atomic_load_acquire_int(&s_stop))
   {
      int cursor = 0;
      uint32_t h, s, z;
      while (cam_state_next(&cursor, &h, &s, &z))
      {
         /* a surface goes to the handler of the thread that queued it */
         if ((h - 0x02000000) / 0x100 != (s >> 24))
            retro_atomic_fetch_add_int(&s_bad, 1);
      }
   }
}

static void test_threads(void)
{
   sthread_t *t[4], *alarm;
   int i;
   cam_state_reset();
   retro_atomic_int_init(&s_stop, 0);
   retro_atomic_int_init(&s_bad, 0);
   alarm = sthread_create(alarm_thread, NULL);
   for (i = 0; i < 4; i++)
      t[i] = sthread_create(title_thread, (void*)(size_t)(i + 1));
   for (i = 0; i < 4; i++)
      sthread_join(t[i]);
   retro_atomic_store_release_int(&s_stop, 1);
   sthread_join(alarm);
   CHECK(retro_atomic_load_acquire_int(&s_bad) == 0);
}

int main(void)
{
   test_unreset();
   test_handles();
   test_surfaces();
   test_retaken();
   test_threads();
   cam_state_reset();
   if (failures)
   {
      fprintf(stderr, "cam_state_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("cam_state_test: all passed\n");
   return 0;
}
