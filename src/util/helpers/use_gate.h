#ifndef UTIL_HELPERS_USE_GATE_H
#define UTIL_HELPERS_USE_GATE_H

/* A resource used from any thread and now and then changed - a USB device's
 * handle opened and closed - with no lock. A use is one atomic add on the
 * gate's word and never waits: while the resource is closed or being
 * changed, it is turned away. A change turns new uses away, sleeps on an
 * eventcount until the ones already in have left, and has the resource to
 * itself until it ends, saying whether the resource is now open; changes
 * take turns. Everything is one word, so every step sees the others in one
 * order. C89. */

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>
#include <rthreads/retro_eventcount.h>

RETRO_BEGIN_DECLS

#define USE_GATE_USERS    0x1FFFFFFF
#define USE_GATE_CHANGING 0x20000000
#define USE_GATE_CLOSED   0x40000000

typedef struct use_gate
{
   retro_atomic_int_t state; /* the users in, and the two flags above */
   retro_eventcount_t wake;
} use_gate_t;

/* closed */
bool use_gate_init(use_gate_t *g);
void use_gate_free(use_gate_t *g);

/* any thread: false, and nothing to leave, while closed or changing */
bool use_gate_enter(use_gate_t *g);
void use_gate_leave(use_gate_t *g);

/* the resource to itself once it returns */
void use_gate_change_begin(use_gate_t *g);
void use_gate_change_end(use_gate_t *g, bool open);

/* a look, not a promise */
bool use_gate_is_open(use_gate_t *g);

#ifdef USE_GATE_TEST_HOOKS
/* a change between turning uses away and waiting for those in */
extern void (*use_gate_test_closing)(use_gate_t *g);
#endif

RETRO_END_DECLS

#endif
