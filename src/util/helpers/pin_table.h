#ifndef UTIL_HELPERS_PIN_TABLE_H
#define UTIL_HELPERS_PIN_TABLE_H

/* Objects found by a 32-bit key, held while used, freed by whoever lets go
 * last - with no lock and nothing allocated: a fixed array of slots in
 * storage its owner gives it, each with a count of references, the
 * table's own and one for each holder. An object taken out while held is
 * released (the release function) when the last holder puts it back, and
 * its slot is not given out again before. C89. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct pin_slot
{
   retro_atomic_int_t ref; /* 0 free, 1 changing, odd dead, else twice the holds */
   retro_atomic_int_t key;
   retro_atomic_ptr_t obj;
} pin_slot_t;

typedef struct pin_table
{
   pin_slot_t *slots;
   int count;
   void (*release)(void *obj);
} pin_table_t;

/* slots: count of them, all zero */
void pin_table_init(pin_table_t *t, pin_slot_t *slots, int count, void (*release)(void *obj));
/* obj under key (not 0, not in it already); false when no slot is free */
bool pin_table_add(pin_table_t *t, uint32_t key, void *obj);
/* key's slot, held; -1 when not in it */
int pin_table_get(pin_table_t *t, uint32_t key);
void *pin_table_obj(pin_table_t *t, int slot);
void pin_table_put(pin_table_t *t, int slot);
/* key taken out: no longer found, released once nothing holds it; false
 * when it was not in it */
bool pin_table_remove(pin_table_t *t, uint32_t key);
/* every key taken out */
void pin_table_remove_all(pin_table_t *t);

RETRO_END_DECLS

#endif
