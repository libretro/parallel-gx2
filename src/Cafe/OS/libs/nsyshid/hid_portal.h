#ifndef NSYSHID_HID_PORTAL_H
#define NSYSHID_HID_PORTAL_H

/* An emulated toy portal's two directions with no lock. Commands from the
 * title (writes and SET_REPORTs, on any of the transfer workers) run one at
 * a time in submission order: the thread whose submit finds none pending
 * runs every command queued until none is left, and the others only queue
 * theirs, so a device's state is only ever touched by one thread and needs
 * no lock. A command's answer is queued as a report the title's next
 * interrupt read takes; a read with none queued sleeps on an eventcount
 * until one is, or until the portal is closed for the title's exit. C89. */

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>
#include <rthreads/retro_eventcount.h>

#include "util/helpers/lf_ring.h"

RETRO_BEGIN_DECLS

#define HID_PORTAL_REPORT 64 /* bytes in a command or a report */
#define HID_PORTAL_SLOTS 64  /* commands, and reports, queued at most */

/* runs one command, on whichever thread is running them */
typedef void (*hid_portal_run_t)(void *ctx, const unsigned char *cmd,
      unsigned len);

typedef struct hid_portal_cmd
{
   unsigned len;
   unsigned char data[HID_PORTAL_REPORT];
} hid_portal_cmd_t;

typedef union hid_portal_cells
{
   double align;
   unsigned char bytes[HID_PORTAL_SLOTS
      * LF_RING_STRIDE(sizeof(hid_portal_cmd_t))];
} hid_portal_cells_t;

typedef struct hid_portal
{
   lf_ring_t cmds;
   lf_ring_t reports;
   retro_eventcount_t wake;
   /* commands counted in by their submits less those run: the submit that
    * moves it from 0 runs them; it can dip below 0 while a command already
    * run is still being counted in */
   retro_atomic_int_t pending;
   retro_atomic_int_t closed;
   hid_portal_run_t run;
   void *ctx;
   hid_portal_cells_t cmd_cells;
   hid_portal_cells_t report_cells;
} hid_portal_t;

bool hid_portal_init(hid_portal_t *p, hid_portal_run_t run, void *ctx);
/* with no transfer running or to come */
void hid_portal_free(hid_portal_t *p);

/* false (and the command dropped) when HID_PORTAL_SLOTS are already
 * queued; len is cut to HID_PORTAL_REPORT */
bool hid_portal_submit(hid_portal_t *p, const unsigned char *cmd,
      unsigned len);
/* from a command's run: false (and the report dropped) when full */
bool hid_portal_report(hid_portal_t *p,
      const unsigned char report[HID_PORTAL_REPORT]);
/* false when none is queued */
bool hid_portal_try_read(hid_portal_t *p,
      unsigned char out[HID_PORTAL_REPORT]);
/* waits for one; false once the portal is closed and none is queued */
bool hid_portal_read(hid_portal_t *p, unsigned char out[HID_PORTAL_REPORT]);
/* wakes every read waiting, and every one to come returns at once */
void hid_portal_close(hid_portal_t *p);

#ifdef HID_PORTAL_TEST_HOOKS
/* between a submit's queueing its command and counting it in */
extern void (*hid_portal_test_queued)(hid_portal_t *p);
#endif

RETRO_END_DECLS

#endif
