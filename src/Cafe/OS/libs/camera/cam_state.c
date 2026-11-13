/* The camera library's state (cam_state.h) */

#include <string.h>

#include <retro_atomic.h>

#include "cam_state.h"

enum { FREE = 0, TAKEN, OPEN };

/* seq is kept less the entry's index, so that a table never reset - all
 * zero - is already an empty queue */
typedef struct entry
{
   retro_atomic_int_t seq;
   uint32_t surface;
   uint32_t size;
   int gen;
} entry_t;

typedef struct cam
{
   retro_atomic_int_t state;
   /* bumped to odd before a new holder's handler goes in and to even
    * after: the alarm hands a surface over only if the gen it was queued
    * under is the even gen on both sides of reading the handler, so it
    * never pairs one holder's surface with the next one's handler */
   retro_atomic_int_t gen;
   retro_atomic_int_t handler;
   uint32_t width, height;
   retro_atomic_int_t enqueue;
   int dequeue;                 /* the alarm's alone */
   entry_t q[CAM_STATE_QUEUE];
} cam_t;

static cam_t s_cams[CAM_STATE_HANDLES];
static retro_atomic_int_t s_started;

#ifdef CAM_STATE_TEST_HOOKS
void (*cam_state_test_mid_init)(void);
void (*cam_state_test_mid_take)(void);
#endif

static void reset_cam(cam_t *c)
{
   int i;
   retro_atomic_store_release_int(&c->state, FREE);
   retro_atomic_store_release_int(&c->gen, 0);
   retro_atomic_store_release_int(&c->handler, 0);
   retro_atomic_store_release_int(&c->enqueue, 0);
   c->dequeue = 0;
   for (i = 0; i < CAM_STATE_QUEUE; i++)
      retro_atomic_store_release_int(&c->q[i].seq, 0);
}

#define IDX(pos) ((unsigned)(pos) % CAM_STATE_QUEUE)

static unsigned seq_of(cam_t *c, int pos)
{
   return (unsigned)retro_atomic_load_acquire_int(&c->q[IDX(pos)].seq) + IDX(pos);
}

static void set_seq(cam_t *c, int pos, unsigned seq)
{
   retro_atomic_store_release_int(&c->q[IDX(pos)].seq, (int)(seq - IDX(pos)));
}

void cam_state_reset(void)
{
   int i;
   for (i = 0; i < CAM_STATE_HANDLES; i++)
      reset_cam(&s_cams[i]);
   retro_atomic_store_release_int(&s_started, 0);
}

static cam_t *cam_of(int handle)
{
   if (handle <= 0 || handle > CAM_STATE_HANDLES)
      return NULL;
   return &s_cams[handle - 1];
}

int cam_state_init(uint32_t width, uint32_t height, uint32_t handler, bool *first)
{
   int i;
   *first = false;
   for (i = 0; i < CAM_STATE_HANDLES; i++)
   {
      cam_t *c = &s_cams[i];
      if (retro_atomic_load_acquire_int(&c->state) != FREE
            || !retro_atomic_cas_int(&c->state, FREE, TAKEN))
         continue;
      c->width = width;
      c->height = height;
      /* what the last holder left queued is dropped as the alarm meets it */
      retro_atomic_fetch_add_int(&c->gen, 1);
      retro_atomic_store_release_int(&c->handler, (int)handler);
#ifdef CAM_STATE_TEST_HOOKS
      if (cam_state_test_mid_init)
         cam_state_test_mid_init();
#endif
      retro_atomic_fetch_add_int(&c->gen, 1);
      *first = retro_atomic_cas_int(&s_started, 0, 1) != 0;
      return i + 1;
   }
   return 0;
}

bool cam_state_exit(int handle)
{
   cam_t *c = cam_of(handle);
   int s;
   if (!c)
      return false;
   s = retro_atomic_load_acquire_int(&c->state);
   while (s != FREE)
   {
      if (retro_atomic_cas_int(&c->state, s, FREE))
         return true;
      s = retro_atomic_load_acquire_int(&c->state);
   }
   return false;
}

bool cam_state_open(int handle)
{
   cam_t *c = cam_of(handle);
   if (!c)
      return false;
   /* taken: open; open already: as it is */
   return retro_atomic_cas_int(&c->state, TAKEN, OPEN)
      || retro_atomic_load_acquire_int(&c->state) == OPEN;
}

bool cam_state_close(int handle)
{
   cam_t *c = cam_of(handle);
   if (!c)
      return false;
   return retro_atomic_cas_int(&c->state, OPEN, TAKEN)
      || retro_atomic_load_acquire_int(&c->state) == TAKEN;
}

enum cam_submit_result cam_state_submit(int handle, uint32_t surface, uint32_t size)
{
   cam_t *c = cam_of(handle);
   entry_t *e;
   int pos, gen;
   if (!c || retro_atomic_load_acquire_int(&c->state) == FREE)
      return CAM_SUBMIT_BAD_HANDLE;
   gen = retro_atomic_load_acquire_int(&c->gen);
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&c->enqueue);
      e = &c->q[IDX(pos)];
      dif = (int)(seq_of(c, pos) - (unsigned)pos);
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&c->enqueue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return CAM_SUBMIT_FULL;
   }
   e->surface = surface;
   e->size = size;
   e->gen = gen;
   set_seq(c, pos, (unsigned)pos + 1);
   return CAM_SUBMIT_QUEUED;
}

/* the next surface queued for c's current holder and that holder's
 * handler, if there is one */
static bool take(cam_t *c, uint32_t *handler, uint32_t *surface, uint32_t *size)
{
   const int gen = retro_atomic_load_acquire_int(&c->gen);
   if (gen & 1)
      return false; /* a new holder going in: next round */
   for (;;)
   {
      const int pos = c->dequeue;
      entry_t *e = &c->q[IDX(pos)];
      int egen;
      if (seq_of(c, pos) != (unsigned)pos + 1)
         return false;
      *surface = e->surface;
      *size = e->size;
      egen = e->gen;
      set_seq(c, pos, (unsigned)pos + CAM_STATE_QUEUE);
      c->dequeue = (int)((unsigned)pos + 1);
      if (egen != gen)
         continue;
#ifdef CAM_STATE_TEST_HOOKS
      if (cam_state_test_mid_take)
         cam_state_test_mid_take();
#endif
      *handler = (uint32_t)retro_atomic_load_acquire_int(&c->handler);
      /* given back and taken again meanwhile: the surface was the last
       * holder's, and the handler may be the next one's */
      return retro_atomic_load_acquire_int(&c->gen) == gen;
   }
}

bool cam_state_next(int *cursor, uint32_t *handler, uint32_t *surface, uint32_t *size)
{
   for (; *cursor < CAM_STATE_HANDLES; (*cursor)++)
   {
      cam_t *c = &s_cams[*cursor];
      if (retro_atomic_load_acquire_int(&c->state) != OPEN)
         continue;
      if (!take(c, handler, surface, size))
         continue;
      (*cursor)++;
      return true;
   }
   return false;
}
