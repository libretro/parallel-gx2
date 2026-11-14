/* One slot from a writer to a reader with no lock (mailbox.h) */

#include "mailbox.h"

#define MB_EMPTY   0
#define MB_FILLING 1
#define MB_FULL    2
#define MB_READING 3

void mailbox_init(mailbox_t *m)
{
   retro_atomic_int_init(&m->state, MB_EMPTY);
}

bool mailbox_begin_put(mailbox_t *m)
{
   /* empty, or full and not taken yet: this one replaces it */
   return retro_atomic_cas_int(&m->state, MB_EMPTY, MB_FILLING)
      || retro_atomic_cas_int(&m->state, MB_FULL, MB_FILLING);
}

void mailbox_end_put(mailbox_t *m)
{
   retro_atomic_store_release_int(&m->state, MB_FULL);
}

bool mailbox_begin_take(mailbox_t *m)
{
   return retro_atomic_load_acquire_int(&m->state) == MB_FULL
      && retro_atomic_cas_int(&m->state, MB_FULL, MB_READING);
}

void mailbox_end_take(mailbox_t *m)
{
   retro_atomic_store_release_int(&m->state, MB_EMPTY);
}
