/* coreinit's async callback queue (async_cb.h) */

#include <retro_atomic.h>

#include "async_cb.h"

typedef struct cell
{
   retro_atomic_int_t seq; /* kept less the cell's index */
   async_cb_t cb;
} cell_t;

static cell_t s_cells[ASYNC_CB_SLOTS];
static retro_atomic_int_t s_enqueue;
static int s_dequeue; /* the callback thread's alone */

#define CELL(pos) ((unsigned)(pos) % ASYNC_CB_SLOTS)

static unsigned seq_of(int pos)
{
   return (unsigned)retro_atomic_load_acquire_int(&s_cells[CELL(pos)].seq) + CELL(pos);
}

static void set_seq(int pos, unsigned seq)
{
   retro_atomic_store_release_int(&s_cells[CELL(pos)].seq, (int)(seq - CELL(pos)));
}

bool async_cb_push(const async_cb_t *cb)
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
   s_cells[CELL(pos)].cb = *cb;
   set_seq(pos, (unsigned)pos + 1);
   return true;
}

bool async_cb_pop(async_cb_t *cb)
{
   const int pos = s_dequeue;
   if (seq_of(pos) != (unsigned)pos + 1)
      return false;
   *cb = s_cells[CELL(pos)].cb;
   set_seq(pos, (unsigned)pos + ASYNC_CB_SLOTS);
   s_dequeue = (int)((unsigned)pos + 1);
   return true;
}

void async_cb_reset(void)
{
   int i;
   for (i = 0; i < ASYNC_CB_SLOTS; i++)
      retro_atomic_store_release_int(&s_cells[i].seq, 0);
   retro_atomic_store_release_int(&s_enqueue, 0);
   s_dequeue = 0;
}
