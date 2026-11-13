/* A bounded lock-free queue of fixed-size items (lf_ring.h) */

#include <string.h>

#include "lf_ring.h"

static retro_atomic_int_t *seq_at(const lf_ring_t *r, unsigned i)
{
   return (retro_atomic_int_t*)(r->cells + (size_t)i * r->stride + (r->stride - 8u));
}

static unsigned seq_of(const lf_ring_t *r, int pos)
{
   const unsigned i = (unsigned)pos & (r->cap - 1u);
   return (unsigned)retro_atomic_load_acquire_int(seq_at(r, i)) + i;
}

static void set_seq(lf_ring_t *r, int pos, unsigned seq)
{
   const unsigned i = (unsigned)pos & (r->cap - 1u);
   retro_atomic_store_release_int(seq_at(r, i), (int)(seq - i));
}

void lf_ring_init(lf_ring_t *r, void *cells, unsigned cap, unsigned size)
{
   retro_atomic_int_init(&r->enqueue, 0);
   retro_atomic_int_init(&r->dequeue, 0);
   r->cells = (unsigned char*)cells;
   r->cap = cap;
   r->size = size;
   r->stride = (unsigned)LF_RING_STRIDE(size);
}

bool lf_ring_push(lf_ring_t *r, const void *item)
{
   int pos;
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&r->enqueue);
      dif = (int)(seq_of(r, pos) - (unsigned)pos);
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&r->enqueue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return false;
   }
   memcpy(r->cells + (size_t)((unsigned)pos & (r->cap - 1u)) * r->stride, item, r->size);
   set_seq(r, pos, (unsigned)pos + 1);
   return true;
}

bool lf_ring_pop(lf_ring_t *r, void *item)
{
   int pos;
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&r->dequeue);
      dif = (int)(seq_of(r, pos) - ((unsigned)pos + 1));
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&r->dequeue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return false;
   }
   memcpy(item, r->cells + (size_t)((unsigned)pos & (r->cap - 1u)) * r->stride, r->size);
   set_seq(r, pos, (unsigned)pos + r->cap);
   return true;
}

unsigned lf_ring_count(lf_ring_t *r)
{
   const int n = (int)((unsigned)retro_atomic_load_acquire_int(&r->enqueue)
         - (unsigned)retro_atomic_load_acquire_int(&r->dequeue));
   return n > 0 ? (unsigned)n : 0;
}
