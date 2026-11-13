/* A fixed map of 64-bit keys to pointers (u64_map.h) */

#include <string.h>

#include "u64_map.h"

static size_t home_of(const u64_map_t *m, uint64_t key)
{
   /* the key a hash already, mixed once more: its low bits pick the slot */
   key ^= key >> 33;
   key *= 0xff51afd7ed558ccdULL;
   key ^= key >> 33;
   return (size_t)key & (m->cap - 1);
}

void u64_map_init(u64_map_t *m, u64_map_slot_t *slots, size_t cap)
{
   m->slots = slots;
   m->cap = cap;
   m->count = 0;
}

static size_t find(const u64_map_t *m, uint64_t key)
{
   size_t i = home_of(m, key);
   while (m->slots[i].value)
   {
      if (m->slots[i].key == key)
         return i;
      i = (i + 1) & (m->cap - 1);
   }
   return m->cap;
}

void *u64_map_get(const u64_map_t *m, uint64_t key)
{
   const size_t i = find(m, key);
   return i < m->cap ? m->slots[i].value : NULL;
}

bool u64_map_put(u64_map_t *m, uint64_t key, void *value)
{
   size_t i;
   if (!value)
      return false;
   i = find(m, key);
   if (i < m->cap)
   {
      m->slots[i].value = value;
      return true;
   }
   if ((m->count + 1) * 4 > m->cap * 3)
      return false;
   i = home_of(m, key);
   while (m->slots[i].value)
      i = (i + 1) & (m->cap - 1);
   m->slots[i].key = key;
   m->slots[i].value = value;
   m->count++;
   return true;
}

void *u64_map_take(u64_map_t *m, uint64_t key)
{
   size_t i = find(m, key), j;
   void *value;
   if (i >= m->cap)
      return NULL;
   value = m->slots[i].value;
   /* the ones after it, back into the hole where their probe would pass it */
   j = i;
   for (;;)
   {
      size_t home;
      j = (j + 1) & (m->cap - 1);
      if (!m->slots[j].value)
         break;
      home = home_of(m, m->slots[j].key);
      /* j's home lies cyclically in (i, j]: it stays */
      if (i <= j ? (i < home && home <= j) : (i < home || home <= j))
         continue;
      m->slots[i] = m->slots[j];
      i = j;
   }
   m->slots[i].value = NULL;
   m->slots[i].key = 0;
   m->count--;
   return value;
}

void *u64_map_next(const u64_map_t *m, size_t *cursor, uint64_t *key)
{
   for (; *cursor < m->cap; (*cursor)++)
   {
      if (m->slots[*cursor].value)
      {
         if (key)
            *key = m->slots[*cursor].key;
         return m->slots[(*cursor)++].value;
      }
   }
   return NULL;
}

void u64_map_clear(u64_map_t *m)
{
   memset(m->slots, 0, m->cap * sizeof(*m->slots));
   m->count = 0;
}
