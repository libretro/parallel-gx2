/* The FSA clients a title has added, with no lock (fsa_clients.h) */

#include <retro_atomic.h>

#include "fsa_clients.h"

static retro_atomic_int_t s_clients[FSA_CLIENTS_MAX];

static retro_atomic_int_t *slot_of(uint32_t handle)
{
   const uint32_t i = handle & 0xFFF;
   if (!handle || i >= FSA_CLIENTS_MAX)
      return NULL;
   return &s_clients[i];
}

void fsa_clients_reset(void)
{
   int i;
   for (i = 0; i < FSA_CLIENTS_MAX; i++)
      retro_atomic_int_init(&s_clients[i], 0);
}

bool fsa_clients_add(uint32_t handle)
{
   retro_atomic_int_t *s = slot_of(handle);
   if (!s)
      return false;
   /* a handle left here is closed: the IOSU gave its index out again */
   retro_atomic_store_release_int(s, (int)handle);
   return true;
}

bool fsa_clients_remove(uint32_t handle)
{
   retro_atomic_int_t *s = slot_of(handle);
   return s && retro_atomic_cas_int(s, (int)handle, 0);
}

bool fsa_clients_has(uint32_t handle)
{
   retro_atomic_int_t *s = slot_of(handle);
   return s && (uint32_t)retro_atomic_load_acquire_int(s) == handle;
}
