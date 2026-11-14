#ifndef UTIL_HELPERS_MAILBOX_H
#define UTIL_HELPERS_MAILBOX_H

/* One slot handed from a writer to a reader with no lock: the state of
 * storage its owner keeps, which the writer fills between begin_put and
 * end_put and the reader reads between begin_take and end_take. A put
 * replaces one not yet taken; a take never sees one half written, and a
 * put never writes into one being read. C89. */

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct mailbox
{
   retro_atomic_int_t state; /* 0 empty, 1 being filled, 2 full, 3 being read */
} mailbox_t;

void mailbox_init(mailbox_t *m);
/* false while another fills it or it is being read */
bool mailbox_begin_put(mailbox_t *m);
void mailbox_end_put(mailbox_t *m);
/* false unless it is full */
bool mailbox_begin_take(mailbox_t *m);
void mailbox_end_take(mailbox_t *m);

RETRO_END_DECLS

#endif
