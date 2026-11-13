#ifndef CAFE_OS_LIBS_COREINIT_ASYNC_CB_H
#define CAFE_OS_LIBS_COREINIT_ASYNC_CB_H

/* The callbacks coreinit's callback thread runs for the rest of the
 * system - a file's async read done, a USB transfer done - queued from any
 * thread, host or guest, and taken by that one thread. C89.
 *
 * No lock and nothing allocated: a bounded queue (D. Vyukov's) of fixed
 * entries, whose positions are kept so that a queue never reset - all
 * zero - is already empty. */

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define ASYNC_CB_SLOTS 4096

typedef struct async_cb
{
   uint32_t func;      /* guest address */
   uint32_t nparams;
   uint32_t params[8]; /* r3 to r10 */
} async_cb_t;

/* false when the queue is full */
bool async_cb_push(const async_cb_t *cb);
/* the callback thread's side, one thread: false when empty */
bool async_cb_pop(async_cb_t *cb);
/* empty, for a title starting: only while nothing else calls these */
void async_cb_reset(void);

RETRO_END_DECLS

#endif
