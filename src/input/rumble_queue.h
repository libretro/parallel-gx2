#ifndef INPUT_RUMBLE_QUEUE_H
#define INPUT_RUMBLE_QUEUE_H

/* A GamePad's rumble patterns, as VPADControlMotor hands them over: up to
 * five queued, each played a step every 1/60 s - two bits of the pattern a
 * step, so its 120 bits are 60 steps, one second. C89.
 *
 * The game's threads push and clear, the thread that drives the motor
 * (retro_run's) steps; with no lock and nothing allocated after the queue
 * itself. A bounded queue of fixed slots, each with a sequence number
 * (D. Vyukov's): a push reserves a slot with one compare-and-swap and
 * publishes it with a release store. A clear marks where the queue stood:
 * everything pushed before it is dropped by the stepper, the pattern
 * playing included, and nothing pushed after. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define RUMBLE_QUEUE_MAX 5       /* patterns queued at most */
#define RUMBLE_QUEUE_MAX_BITS 120
#define RUMBLE_QUEUE_STEP_US (1000000 / 60)

typedef struct rumble_queue rumble_queue_t;

/* made once, where the controller is (never while a game plays); NULL for
 * no memory */
rumble_queue_t *rumble_queue_new(void);
void rumble_queue_free(rumble_queue_t *q);

/* length bits of pattern (at most RUMBLE_QUEUE_MAX_BITS are read); 0 bits
 * clear. false where RUMBLE_QUEUE_MAX are queued already. */
bool rumble_queue_push(rumble_queue_t *q, const uint8_t *pattern, unsigned length);
/* everything queued so far dropped, the pattern playing included */
void rumble_queue_clear(rumble_queue_t *q);
/* patterns queued, the one playing included */
unsigned rumble_queue_count(const rumble_queue_t *q);

/* The stepper's side, one thread: whether the motor is on at now_us (a
 * monotonic clock in microseconds). A step every RUMBLE_QUEUE_STEP_US; a
 * pattern pushed to an idle queue starts at once. */
bool rumble_queue_step(rumble_queue_t *q, int64_t now_us);

#ifdef RUMBLE_QUEUE_TEST_HOOKS
/* an empty queue's positions moved to pos, to see them wrap */
void rumble_queue_test_start_at(rumble_queue_t *q, int pos);
#endif

RETRO_END_DECLS

#endif
