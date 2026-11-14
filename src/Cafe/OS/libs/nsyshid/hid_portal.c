/* An emulated toy portal's commands and reports with no lock (hid_portal.h) */

#include <string.h>

#include "hid_portal.h"

#ifdef HID_PORTAL_TEST_HOOKS
void (*hid_portal_test_queued)(hid_portal_t *p);
#endif

bool hid_portal_init(hid_portal_t *p, hid_portal_run_t run, void *ctx)
{
   /* a ring's cells start zeroed: each one's sequence is its index */
   memset(&p->cmd_cells, 0, sizeof(p->cmd_cells));
   memset(&p->report_cells, 0, sizeof(p->report_cells));
   lf_ring_init(&p->cmds, p->cmd_cells.bytes, HID_PORTAL_SLOTS,
         sizeof(hid_portal_cmd_t));
   lf_ring_init(&p->reports, p->report_cells.bytes, HID_PORTAL_SLOTS,
         HID_PORTAL_REPORT);
   retro_atomic_int_init(&p->pending, 0);
   retro_atomic_int_init(&p->closed, 0);
   p->run = run;
   p->ctx = ctx;
   return retro_eventcount_init(&p->wake);
}

void hid_portal_free(hid_portal_t *p)
{
   retro_eventcount_free(&p->wake);
}

/* Runs commands until every one counted in has run. A command can be run
 * before its submit counts it in, which takes pending below what is still
 * queued, and below 0 once nothing is: its submit's count then only brings
 * pending back up, and the submit that next moves it from 0 has a command
 * of its own queued to run. Every count and run is a read-modify-write of
 * pending, so whoever runs next sees what the last one wrote. */
static void hid_portal_drain(hid_portal_t *p)
{
   for (;;)
   {
      hid_portal_cmd_t c;
      int ran = 0;
      while (lf_ring_pop(&p->cmds, &c))
      {
         p->run(p->ctx, c.data, c.len);
         ran++;
      }
      /* one counted in but still being queued is waited for here: its
       * submit is past claiming its cell and only short of filling it */
      if (retro_atomic_fetch_sub_int(&p->pending, ran) - ran <= 0)
         return;
   }
}

bool hid_portal_submit(hid_portal_t *p, const unsigned char *cmd,
      unsigned len)
{
   hid_portal_cmd_t c;
   if (len > HID_PORTAL_REPORT)
      len = HID_PORTAL_REPORT;
   memset(&c, 0, sizeof(c));
   memcpy(c.data, cmd, len);
   c.len = len;
   if (!lf_ring_push(&p->cmds, &c))
      return false;
#ifdef HID_PORTAL_TEST_HOOKS
   if (hid_portal_test_queued)
      hid_portal_test_queued(p);
#endif
   if (retro_atomic_fetch_add_int(&p->pending, 1) == 0)
      hid_portal_drain(p);
   return true;
}

bool hid_portal_report(hid_portal_t *p,
      const unsigned char report[HID_PORTAL_REPORT])
{
   if (!lf_ring_push(&p->reports, report))
      return false;
   retro_eventcount_notify(&p->wake);
   return true;
}

bool hid_portal_try_read(hid_portal_t *p,
      unsigned char out[HID_PORTAL_REPORT])
{
   return lf_ring_pop(&p->reports, out);
}

bool hid_portal_read(hid_portal_t *p, unsigned char out[HID_PORTAL_REPORT])
{
   for (;;)
   {
      int key;
      if (lf_ring_pop(&p->reports, out))
         return true;
      if (retro_atomic_load_acquire_int(&p->closed))
         return false;
      key = retro_eventcount_prepare_wait(&p->wake);
      if (lf_ring_pop(&p->reports, out))
      {
         retro_eventcount_cancel_wait(&p->wake);
         return true;
      }
      if (retro_atomic_load_acquire_int(&p->closed))
      {
         retro_eventcount_cancel_wait(&p->wake);
         return false;
      }
      retro_eventcount_commit_wait(&p->wake, key);
   }
}

void hid_portal_close(hid_portal_t *p)
{
   retro_atomic_store_release_int(&p->closed, 1);
   retro_eventcount_notify(&p->wake);
}
