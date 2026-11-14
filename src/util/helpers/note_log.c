/* Notes from one writer to any readers, with no lock (note_log.h) */

#include "note_log.h"

void note_log_init(note_log_t *l)
{
   int i, j;
   retro_atomic_int_init(&l->last, 0);
   for (i = 0; i < NOTE_LOG_SLOTS; i++)
   {
      retro_atomic_int_init(&l->slots[i].seq, 0);
      for (j = 0; j < NOTE_LOG_WORDS; j++)
         retro_atomic_int_init(&l->slots[i].w[j], 0);
   }
}

int note_log_post(note_log_t *l, const int32_t w[NOTE_LOG_WORDS])
{
   int j;
   const int id          = retro_atomic_load_relaxed_int(&l->last) + 1;
   note_log_slot_t *slot = &l->slots[(unsigned)id % NOTE_LOG_SLOTS];
   /* marked being written before the words change (a swap: nothing after
    * it moves before it) */
   retro_atomic_exchange_int(&slot->seq, 2 * id - 1);
   for (j = 0; j < NOTE_LOG_WORDS; j++)
      retro_atomic_store_release_int(&slot->w[j], (int)w[j]);
   retro_atomic_store_release_int(&slot->seq, 2 * id);
   retro_atomic_store_release_int(&l->last, id);
   return id;
}

bool note_log_next(note_log_t *l, int *prev, int32_t w[NOTE_LOG_WORDS])
{
   int id;
   const int last = retro_atomic_load_acquire_int(&l->last);
   id = *prev + 1;
   /* the older ones are written over */
   if (last - id >= NOTE_LOG_SLOTS)
      id = last - NOTE_LOG_SLOTS + 1;
   for (; id <= last; id++)
   {
      int j;
      note_log_slot_t *slot = &l->slots[(unsigned)id % NOTE_LOG_SLOTS];
      if (retro_atomic_load_acquire_int(&slot->seq) != 2 * id)
         continue; /* written over since last was read */
      for (j = 0; j < NOTE_LOG_WORDS; j++)
         w[j] = (int32_t)retro_atomic_load_acquire_int(&slot->w[j]);
      /* a read that writes, so it is not answered from before the words
       * were read */
      if (retro_atomic_fetch_add_int(&slot->seq, 0) != 2 * id)
         continue;
      *prev = id;
      return true;
   }
   return false;
}
