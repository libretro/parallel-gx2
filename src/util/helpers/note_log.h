#ifndef UTIL_HELPERS_NOTE_LOG_H
#define UTIL_HELPERS_NOTE_LOG_H

/* The last NOTE_LOG_SLOTS notes one writer has posted, each read by any
 * number of readers, with no lock and nothing allocated. A note is a few
 * words; each gets the next id, from 1. A reader keeps the id of the last
 * note it took and asks for the next: it gets them in order, skips those
 * already written over, and never sees one half written. C89. */

#include <stdint.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define NOTE_LOG_SLOTS 64 /* a power of two */
#define NOTE_LOG_WORDS 4

typedef struct note_log_slot
{
   retro_atomic_int_t seq; /* 2 * id once written, odd while being written */
   retro_atomic_int_t w[NOTE_LOG_WORDS];
} note_log_slot_t;

typedef struct note_log
{
   retro_atomic_int_t last; /* the last id posted */
   note_log_slot_t slots[NOTE_LOG_SLOTS];
} note_log_t;

/* not while others use it */
void note_log_init(note_log_t *l);
/* the one writer; returns the note's id */
int note_log_post(note_log_t *l, const int32_t w[NOTE_LOG_WORDS]);
/* any reader: the first note after *prev still kept, its id put in *prev;
 * false when there is none yet */
bool note_log_next(note_log_t *l, int *prev, int32_t w[NOTE_LOG_WORDS]);

RETRO_END_DECLS

#endif
