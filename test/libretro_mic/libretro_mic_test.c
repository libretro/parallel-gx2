/* The GamePad microphone from the frontend's (src/libretro/libretro_mic.c),
 * against a frontend microphone of the test's own: opened only when the
 * title opens the GamePad's and on only while it has it open, read every
 * frame for what the frame captures, resampled to 32 kHz from the rate it
 * runs at, the title never more than about 70 ms behind and never handed
 * what was left from before it closed the microphone - and, under TSan,
 * retro_run and the AX thread at once handing every sample over in order.
 * C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "libretro/libretro_mic.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* the frontend's microphone: a ramp, one more each sample */
static struct
{
   unsigned rate;
   int opens, closes, fail_open;
   int on;
   int state_calls;
   long asked;      /* samples asked for, all reads */
   int reads;
   int paused;
   int next;        /* the ramp's next step */
   int stride;      /* the ramp's rise a sample (0: 1) */
} fe;

static int s_handle;

static retro_microphone_t * RETRO_CALLCONV fe_open(const retro_microphone_params_t *p)
{
   fe.opens++;
   if (fe.fail_open)
      return NULL;
   CHECK(p && p->rate == LIBRETRO_MIC_RATE);
   return (retro_microphone_t*)&s_handle;
}

static void RETRO_CALLCONV fe_close(retro_microphone_t *m)
{
   CHECK(m == (retro_microphone_t*)&s_handle);
   fe.closes++;
}

static bool RETRO_CALLCONV fe_params(const retro_microphone_t *m, retro_microphone_params_t *p)
{
   (void)m;
   p->rate = fe.rate;
   return true;
}

static bool RETRO_CALLCONV fe_set(retro_microphone_t *m, bool state)
{
   (void)m;
   fe.on = state ? 1 : 0;
   fe.state_calls++;
   return true;
}

static bool RETRO_CALLCONV fe_get(const retro_microphone_t *m)
{
   (void)m;
   return fe.on != 0;
}

static int RETRO_CALLCONV fe_read(retro_microphone_t *m, int16_t *s, size_t n)
{
   size_t i;
   (void)m;
   fe.reads++;
   if (!fe.on || fe.paused)
      return -1;
   fe.asked += (long)n;
   for (i = 0; i < n; i++)
      s[i] = fe.stride ? (int16_t)((fe.next++ * fe.stride) & 0x7FFF)
                       : (int16_t)(fe.next++ & 0x3FFF);
   return (int)n;
}

static struct retro_microphone_interface s_iface;

static void reset_fe(unsigned rate)
{
   memset(&fe, 0, sizeof(fe));
   fe.rate = rate;
   s_iface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
   s_iface.open_mic          = fe_open;
   s_iface.close_mic         = fe_close;
   s_iface.get_params        = fe_params;
   s_iface.set_mic_state     = fe_set;
   s_iface.get_mic_state     = fe_get;
   s_iface.read_mic          = fe_read;
}

static void test_none(void)
{
   int16_t buf[96];
   struct retro_microphone_interface empty;
   memset(&empty, 0, sizeof(empty));
   CHECK(!libretro_mic_init(NULL));
   CHECK(!libretro_mic_init(&empty));
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(!libretro_mic_live());
   CHECK(libretro_mic_take(buf, 96) == 0);
   libretro_mic_deinit();
}

static void test_on_off(void)
{
   int16_t buf[96];
   int i;
   reset_fe(LIBRETRO_MIC_RATE);
   CHECK(libretro_mic_init(&s_iface));
   libretro_mic_run(60.0);
   CHECK(fe.opens == 0 && !libretro_mic_live()); /* not wanted: not opened */
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(fe.opens == 1 && fe.on && libretro_mic_live());
   /* a second of frames: a second of samples asked for */
   for (i = 1; i < 60; i++)
      libretro_mic_run(60.0);
   CHECK(fe.asked >= LIBRETRO_MIC_RATE - 1 && fe.asked <= LIBRETRO_MIC_RATE);
   /* at 50 fps too */
   fe.asked = 0;
   for (i = 0; i < 50; i++)
      libretro_mic_run(50.0);
   CHECK(fe.asked >= LIBRETRO_MIC_RATE - 1 && fe.asked <= LIBRETRO_MIC_RATE + 1);
   libretro_mic_want(false);
   libretro_mic_run(60.0);
   CHECK(!fe.on && !libretro_mic_live());
   CHECK(libretro_mic_take(buf, 96) == 0);
   fe.reads = 0;
   libretro_mic_run(60.0);
   CHECK(fe.reads == 0); /* off: not read */
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(fe.opens == 1 && fe.on);  /* the same microphone on again */
   libretro_mic_deinit();
   CHECK(fe.closes == 1 && !libretro_mic_live());
   libretro_mic_deinit();
   CHECK(fe.closes == 1);
}

/* the title's 3 ms from the ring: what was left before it closed it is
 * dropped, then the ramp in order */
static void test_order_and_stale(void)
{
   int16_t buf[96];
   int i, prev = -1, steps = 0, bad = 0;
   reset_fe(LIBRETRO_MIC_RATE);
   CHECK(libretro_mic_init(&s_iface));
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   libretro_mic_run(60.0);
   /* the first take: anything from before this time turned on is dropped */
   CHECK(libretro_mic_take(buf, 96) == 0);
   /* a session starts with the first sample captured, not a 0 before it:
    * turned on while paused, so the reader is there before the first */
   libretro_mic_want(false);
   libretro_mic_run(60.0);
   fe.paused = 1;
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 0);
   fe.paused = 0;
   fe.next   = 5000;
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 96);
   CHECK(buf[0] == 5000 && buf[1] == 5001);
   prev = buf[95];
   libretro_mic_run(60.0);
   for (i = 0; i < 5; i++)
   {
      size_t k, got = libretro_mic_take(buf, 96);
      CHECK(got == 96);
      for (k = 0; k < got; k++)
      {
         if (prev >= 0 && buf[k] != ((prev + 1) & 0x3FFF))
            bad++;
         prev = buf[k];
         steps++;
      }
   }
   CHECK(bad == 0 && steps == 480);
   /* closed and opened again: none of the old ones */
   libretro_mic_want(false);
   libretro_mic_run(60.0);
   fe.next = 10000;
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 0);
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 96);
   CHECK(buf[0] >= 10000 && buf[0] < 10000 + 2 * 534);
   libretro_mic_deinit();
}

/* 48 kHz to 32: two out for three in, on the line between them - a ramp
 * rising 2 a sample in, so 3 a sample out, exactly */
static void test_resample(void)
{
   int16_t buf[96];
   int i, bad = 0;
   long total = 0;
   reset_fe(48000);
   fe.stride = 2;
   CHECK(libretro_mic_init(&s_iface));
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 0);
   /* ten frames: 8000 in, short of where the ramp wraps */
   fe.asked = 0;
   for (i = 0; i < 10; i++)
   {
      size_t k, got;
      libretro_mic_run(60.0);
      while ((got = libretro_mic_take(buf, 96)) > 0)
      {
         for (k = 1; k < got; k++)
         {
            if (buf[k] - buf[k - 1] != 3)
               bad++;
         }
         total += (long)got;
      }
   }
   CHECK(bad == 0);
   CHECK(fe.asked == 8000);
   CHECK(total >= 5333 - 2 && total <= 5334);
   libretro_mic_deinit();
}

static void test_lag_and_failures(void)
{
   int16_t buf[96];
   int i;
   reset_fe(LIBRETRO_MIC_RATE);
   CHECK(libretro_mic_init(&s_iface));
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 0);
   /* the AX thread stalled for a second: the title is caught up after */
   for (i = 0; i < 60; i++)
      libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 96);
   {
      int behind = 0;
      while (libretro_mic_take(buf, 96) == 96)
         behind += 96;
      CHECK(behind <= LIBRETRO_MIC_RATE / 15);
   }
   /* paused: nothing pushed */
   fe.paused = 1;
   libretro_mic_run(60.0);
   CHECK(libretro_mic_take(buf, 96) == 0);
   fe.paused = 0;
   libretro_mic_deinit();
   /* none to open: not asked again until the title opens it again */
   reset_fe(LIBRETRO_MIC_RATE);
   fe.fail_open = 1;
   CHECK(libretro_mic_init(&s_iface));
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   libretro_mic_run(60.0);
   CHECK(fe.opens == 1 && !libretro_mic_live());
   fe.fail_open = 0;
   libretro_mic_want(true);
   libretro_mic_run(60.0);
   CHECK(fe.opens == 2 && libretro_mic_live());
   libretro_mic_deinit();
}

/* retro_run and the AX thread at once */
static retro_atomic_int_t s_stop;
static retro_atomic_int_t s_bad;
static retro_atomic_int_t s_taken;

static void ax_thread(void *ud)
{
   int16_t buf[96];
   int prev = -1;
   (void)ud;
   while (!retro_atomic_load_acquire_int(&s_stop))
   {
      size_t k, got = libretro_mic_take(buf, 96);
      for (k = 0; k < got; k++)
      {
         /* in order, or a jump forward where the old ones were dropped */
         if (prev >= 0 && buf[k] == prev)
            retro_atomic_fetch_add_int(&s_bad, 1);
         prev = buf[k];
      }
      retro_atomic_fetch_add_int(&s_taken, (int)got);
      if (!got)
         sthread_yield();
   }
}

static void test_threads(void)
{
   sthread_t *t;
   int i;
   reset_fe(LIBRETRO_MIC_RATE);
   CHECK(libretro_mic_init(&s_iface));
   retro_atomic_int_init(&s_stop, 0);
   retro_atomic_int_init(&s_bad, 0);
   retro_atomic_int_init(&s_taken, 0);
   libretro_mic_want(true);
   t = sthread_create(ax_thread, NULL);
   for (i = 0; i < 3000; i++)
   {
      libretro_mic_run(60.0);
      if (i % 500 == 250)
      {
         /* the title closes and opens it again meanwhile */
         libretro_mic_want(false);
         libretro_mic_run(60.0);
         libretro_mic_want(true);
      }
      if (!(i & 7))
         sthread_yield();
   }
   retro_atomic_store_release_int(&s_stop, 1);
   sthread_join(t);
   CHECK(retro_atomic_load_acquire_int(&s_bad) == 0);
   CHECK(retro_atomic_load_acquire_int(&s_taken) > 0);
   libretro_mic_deinit();
}

int main(void)
{
   test_none();
   test_on_off();
   test_order_and_stale();
   test_resample();
   test_lag_and_failures();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "libretro_mic_test: %d failed\n", failures);
      return 1;
   }
   printf("libretro_mic_test: all passed\n");
   return 0;
}
