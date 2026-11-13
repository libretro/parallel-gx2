/* Objects held by key, freed by the last to let go (pin_table.h) */

#include "pin_table.h"

#ifdef PIN_TABLE_TEST_HOOKS
/* in a lookup, between finding a slot and holding it */
void (*pin_table_test_mid_lookup)(void);
/* in an add, between seeing a slot free and taking it */
void (*pin_table_test_mid_add)(void);
#define MID_LOOKUP() do { if (pin_table_test_mid_lookup) pin_table_test_mid_lookup(); } while (0)
#define MID_ADD() do { if (pin_table_test_mid_add) pin_table_test_mid_add(); } while (0)
#else
#define MID_LOOKUP() do { } while (0)
#define MID_ADD() do { } while (0)
#endif

void pin_table_init(pin_table_t *t, pin_slot_t *slots, int count, void (*release)(void *obj))
{
   t->slots = slots;
   t->count = count;
   t->release = release;
}

static bool pin(pin_slot_t *s)
{
   int v = retro_atomic_load_acquire_int(&s->ref);
   while (v >= 2 && !(v & 1))
   {
      if (retro_atomic_cas_int(&s->ref, v, v + 2))
         return true;
      v = retro_atomic_load_acquire_int(&s->ref);
   }
   return false;
}

static void release(pin_table_t *t, pin_slot_t *s)
{
   void *obj = retro_atomic_load_acquire_ptr(&s->obj);
   retro_atomic_store_release_int(&s->key, 0);
   retro_atomic_store_release_ptr(&s->obj, NULL);
   if (t->release && obj)
      t->release(obj);
   /* free only now: not given out again while being released */
   retro_atomic_store_release_int(&s->ref, 0);
}

bool pin_table_add(pin_table_t *t, uint32_t key, void *obj)
{
   int i;
   if (!key)
      return false;
   for (i = 0; i < t->count; i++)
   {
      pin_slot_t *s = &t->slots[i];
      if (retro_atomic_load_acquire_int(&s->ref) != 0)
         continue;
      MID_ADD();
      /* another may have taken it since */
      if (!retro_atomic_cas_int(&s->ref, 0, 1))
         continue;
      retro_atomic_store_release_int(&s->key, (int)key);
      retro_atomic_store_release_ptr(&s->obj, obj);
      retro_atomic_store_release_int(&s->ref, 2); /* the table's */
      return true;
   }
   return false;
}

int pin_table_get(pin_table_t *t, uint32_t key)
{
   int i;
   if (!key)
      return -1;
   for (i = 0; i < t->count; i++)
   {
      pin_slot_t *s = &t->slots[i];
      if ((uint32_t)retro_atomic_load_acquire_int(&s->key) != key)
         continue;
      MID_LOOKUP();
      if (!pin(s))
         continue;
      /* freed and taken for another meanwhile: not this one */
      if ((uint32_t)retro_atomic_load_acquire_int(&s->key) == key)
         return i;
      pin_table_put(t, i);
   }
   return -1;
}

void *pin_table_obj(pin_table_t *t, int slot)
{
   return retro_atomic_load_acquire_ptr(&t->slots[slot].obj);
}

void pin_table_put(pin_table_t *t, int slot)
{
   pin_slot_t *s;
   if (slot < 0 || slot >= t->count)
      return;
   s = &t->slots[slot];
   if (retro_atomic_fetch_add_int(&s->ref, -2) - 2 == 1)
      release(t, s);
}

static bool kill(pin_table_t *t, pin_slot_t *s)
{
   int v = retro_atomic_load_acquire_int(&s->ref);
   while (v >= 2 && !(v & 1))
   {
      /* the table's reference dropped, the slot marked dead */
      const int n = (v | 1) - 2;
      if (retro_atomic_cas_int(&s->ref, v, n))
      {
         if (n == 1)
            release(t, s);
         return true;
      }
      v = retro_atomic_load_acquire_int(&s->ref);
   }
   return false;
}

bool pin_table_remove(pin_table_t *t, uint32_t key)
{
   int i;
   if (!key)
      return false;
   for (i = 0; i < t->count; i++)
   {
      pin_slot_t *s = &t->slots[i];
      bool killed;
      /* held while looked at: its key is not another's when it is killed */
      if ((uint32_t)retro_atomic_load_acquire_int(&s->key) != key)
         continue;
      MID_LOOKUP();
      if (!pin(s))
         continue;
      killed = (uint32_t)retro_atomic_load_acquire_int(&s->key) == key && kill(t, s);
      pin_table_put(t, i);
      if (killed)
         return true;
   }
   return false;
}

void pin_table_remove_all(pin_table_t *t)
{
   int i;
   for (i = 0; i < t->count; i++)
      kill(t, &t->slots[i]);
}
