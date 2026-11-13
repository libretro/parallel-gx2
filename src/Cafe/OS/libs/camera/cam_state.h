#ifndef CAFE_OS_LIBS_CAMERA_CAM_STATE_H
#define CAFE_OS_LIBS_CAMERA_CAM_STATE_H

/* The camera library's state: its handles and the surfaces a title hands
 * each one to be filled, which a 60 Hz alarm gives back to its handler one
 * at a time. C89.
 *
 * No lock and nothing allocated: a fixed table of handles, each taken and
 * given back with a compare-and-swap, and for each a bounded queue of
 * surfaces (D. Vyukov's) the title's threads push into and the alarm takes
 * from. Each queued surface carries the generation of the handle it was
 * queued for, so what a handle had queued when it was given back is never
 * handed to the next title or caller that takes it. */

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define CAM_STATE_HANDLES 8
#define CAM_STATE_QUEUE 128 /* surfaces queued per handle at most */

/* cam_state_submit */
enum cam_submit_result
{
   CAM_SUBMIT_QUEUED = 0,
   CAM_SUBMIT_BAD_HANDLE,
   CAM_SUBMIT_FULL
};

/* a handle (1..CAM_STATE_HANDLES, the lowest free) for a camera whose
 * frames go to handler; 0 when all are taken. *first set when this is the
 * first handle since cam_state_reset, for the caller to start its alarm. */
int cam_state_init(uint32_t width, uint32_t height, uint32_t handler, bool *first);
/* given back, closed first; false for a handle not taken */
bool cam_state_exit(int handle);
/* false for a handle not taken */
bool cam_state_open(int handle);
bool cam_state_close(int handle);
enum cam_submit_result cam_state_submit(int handle, uint32_t surface, uint32_t size);

/* The alarm's side, one thread: the next frame to hand over, from the open
 * handle after *cursor (start at 0); false when there is none left this
 * round. A handle's surfaces come back in the order they were submitted,
 * one a round. */
bool cam_state_next(int *cursor, uint32_t *handler, uint32_t *surface, uint32_t *size);

/* every handle given back and every queue emptied, for a title starting:
 * only while nothing else calls these. The state starts out so; the first
 * title needs no reset. */
void cam_state_reset(void);

RETRO_END_DECLS

#endif
