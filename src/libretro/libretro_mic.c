/* The GamePad microphone from the frontend's (libretro_mic.h) */

#include <string.h>

#include <retro_atomic.h>
#include <retro_spsc.h>

#include "libretro_mic.h"

/* captured samples in a frame at most: 192 kHz at 24 fps */
#define MIC_MAX_IN 8192
/* the ring: about 0.25 s at 32 kHz */
#define MIC_RING_SAMPLES 8192
/* the most the title is behind the frontend: about 70 ms */
#define MIC_MAX_LAG (LIBRETRO_MIC_RATE / 15)

static struct
{
   retro_spsc_t ring;
   struct retro_microphone_interface iface;
   retro_microphone_t *mic;
   retro_atomic_int_t wanted;
   /* 0 off; while on, which time it was turned on: the reader drops what is
    * left from the time before */
   retro_atomic_int_t live;
   int generation;
   int seen;       /* the reader's: the generation it last read */
   bool ready;
   bool on;
   unsigned in_rate;
   double owed;    /* captured samples due, carried between frames */
   uint32_t step;  /* input samples an output sample moves on, 16.16 */
   uint32_t pos;   /* the next output's place past prev, 16.16 */
   int16_t prev;   /* the last input sample of the frame before */
   int16_t in[MIC_MAX_IN];
   int16_t out[MIC_MAX_IN * 8];
} s_mic;

bool libretro_mic_init(const struct retro_microphone_interface *iface)
{
   libretro_mic_deinit();
   retro_atomic_int_init(&s_mic.wanted, 0);
   retro_atomic_int_init(&s_mic.live, 0);
   s_mic.generation = 0;
   s_mic.seen       = 0;
   if (!iface || !iface->open_mic || !iface->read_mic || !iface->set_mic_state)
      return false;
   if (!retro_spsc_init(&s_mic.ring, MIC_RING_SAMPLES * sizeof(int16_t)))
      return false;
   s_mic.iface = *iface;
   s_mic.ready = true;
   return true;
}

void libretro_mic_deinit(void)
{
   if (!s_mic.ready)
      return;
   retro_atomic_store_release_int(&s_mic.live, 0);
   if (s_mic.mic && s_mic.iface.close_mic)
      s_mic.iface.close_mic(s_mic.mic);
   s_mic.mic   = NULL;
   s_mic.on    = false;
   s_mic.ready = false;
   retro_spsc_free(&s_mic.ring);
}

void libretro_mic_want(bool want)
{
   retro_atomic_store_release_int(&s_mic.wanted, want ? 1 : 0);
}

bool libretro_mic_live(void)
{
   return retro_atomic_load_acquire_int(&s_mic.live) != 0;
}

static bool mic_open(void)
{
   retro_microphone_params_t params;
   params.rate = LIBRETRO_MIC_RATE;
   s_mic.mic = s_mic.iface.open_mic(&params);
   if (!s_mic.mic)
      return false;
   /* the rate it runs at may not be the one asked for */
   s_mic.in_rate = LIBRETRO_MIC_RATE;
   if (s_mic.iface.get_params && s_mic.iface.get_params(s_mic.mic, &params)
         && params.rate >= 8000 && params.rate <= 192000)
      s_mic.in_rate = params.rate;
   s_mic.step = (uint32_t)(((uint64_t)s_mic.in_rate << 16) / LIBRETRO_MIC_RATE);
   return true;
}

/* n captured samples to 32 kHz, by straight lines between them */
static size_t mic_resample(size_t n)
{
   size_t   k = 0;
   uint32_t pos = s_mic.pos;
   while ((pos >> 16) < n)
   {
      const uint32_t i = pos >> 16;
      const int32_t  a = i ? s_mic.in[i - 1] : s_mic.prev;
      const int32_t  b = s_mic.in[i];
      s_mic.out[k++]   = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(pos & 0xFFFF)) >> 16));
      pos += s_mic.step;
   }
   s_mic.pos  = pos - ((uint32_t)n << 16);
   s_mic.prev = s_mic.in[n - 1];
   return k;
}

void libretro_mic_run(double fps)
{
   const bool want = retro_atomic_load_acquire_int(&s_mic.wanted) != 0;
   size_t want_n;
   int got;
   if (!s_mic.ready)
      return;
   if (want != s_mic.on)
   {
      if (want && !s_mic.mic && !mic_open())
      {
         /* none to open: ask again only when the title opens it again */
         libretro_mic_want(false);
         return;
      }
      if (s_mic.mic)
         s_mic.iface.set_mic_state(s_mic.mic, want);
      s_mic.on    = want;
      s_mic.owed  = 0.0;
      /* from the first sample captured, not one from before */
      s_mic.pos   = 1u << 16;
      s_mic.prev  = 0;
      if (want && ++s_mic.generation <= 0)
         s_mic.generation = 1;
      retro_atomic_store_release_int(&s_mic.live, want ? s_mic.generation : 0);
   }
   if (!s_mic.on)
      return;
   /* what a frame captures, the fractions carried */
   s_mic.owed += (double)s_mic.in_rate / (fps > 1.0 ? fps : 60.0);
   want_n      = (size_t)s_mic.owed;
   if (want_n > MIC_MAX_IN)
      want_n = MIC_MAX_IN;
   s_mic.owed -= (double)want_n;
   if (!want_n)
      return;
   got = s_mic.iface.read_mic(s_mic.mic, s_mic.in, want_n);
   if (got <= 0)
      return; /* paused, or nothing: the title hears silence */
   if ((size_t)got > want_n)
      got = (int)want_n;
   {
      const size_t k = mic_resample((size_t)got);
      /* what does not fit is dropped: the reader is far behind */
      retro_spsc_write(&s_mic.ring, s_mic.out, k * sizeof(int16_t));
   }
}

size_t libretro_mic_take(int16_t *out, size_t n)
{
   size_t avail, got;
   const int live = retro_atomic_load_acquire_int(&s_mic.live);
   if (!live)
      return 0;
   avail = retro_spsc_read_avail(&s_mic.ring) / sizeof(int16_t);
   if (live != s_mic.seen)
   {
      /* turned on again: what is left is from before */
      s_mic.seen = live;
      retro_spsc_skip(&s_mic.ring, avail * sizeof(int16_t));
      return 0;
   }
   /* far behind the frontend (a stall): the oldest go, to stay near it */
   if (avail > MIC_MAX_LAG + n)
      retro_spsc_skip(&s_mic.ring, (avail - MIC_MAX_LAG - n) * sizeof(int16_t));
   got = retro_spsc_read(&s_mic.ring, out, n * sizeof(int16_t)) / sizeof(int16_t);
   return got;
}
