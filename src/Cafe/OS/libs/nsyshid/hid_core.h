#ifndef CAFE_OS_LIBS_NSYSHID_HID_CORE_H
#define CAFE_OS_LIBS_NSYSHID_HID_CORE_H

/* nsyshid's state: the USB devices a title sees (the portals, emulated or
 * passed through), the clients told of them, and the threads that carry
 * out a title's transfers. C89.
 *
 * No lock and nothing allocated while a title runs. A device is one of
 * HID_CORE_DEVICES fixed slots, its index also its HID_t's in the pool the
 * title is handed. A slot holds a count of references: the table's own,
 * dropped when the device is detached, and one for each caller holding it
 * (hid_core_get_*, put back with hid_core_put). The last one put frees the
 * slot, calling the release function on the object the slot held, so a
 * device detached while a transfer runs on it lives until that transfer
 * ends. Transfers run on HID_CORE_WORKERS threads started with the module,
 * taken from a bounded queue (D. Vyukov's) of requests. */

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define HID_CORE_DEVICES 128
#define HID_CORE_CLIENTS 32
#define HID_CORE_WORKERS 4
#define HID_CORE_REQUESTS 64

/* a device's vendor and product in one word */
#define HID_CORE_ID(vid, pid) (((uint32_t)(vid) << 16) | (uint32_t)(pid))

/* what is called with a slot's object once nothing holds it */
void hid_core_set_release(void (*release)(void *obj));

/* key is what tells devices apart (attached twice: refused), obj what the
 * slot holds, owner the backend. The slot, held once by the caller, and
 * its new handle; -1 when key is attached already, -2 when no slot is
 * free. */
int hid_core_attach(const void *key, void *obj, const void *owner,
      uint32_t id, uint32_t *handle);
/* key's slot no longer found; false when it was not attached */
bool hid_core_detach(const void *key);

/* a slot, held, or -1 */
int hid_core_get_key(const void *key);
int hid_core_get_handle(uint32_t handle);
int hid_core_get_id(uint32_t id);
/* the attached devices after *cursor (start at 0), owner's or, for NULL,
 * every one */
int hid_core_next(int *cursor, const void *owner);
void hid_core_put(int slot);

void *hid_core_obj(int slot);
uint32_t hid_core_handle(int slot);

/* the clients told of devices coming and going: guest addresses */
bool hid_core_client_add(uint32_t client);
bool hid_core_client_del(uint32_t client);
/* the clients after *cursor (start at 0), newest first; 0 when no more */
uint32_t hid_core_client_next(int *cursor);
void hid_core_clients_reset(void);

/* a transfer for the workers: slot is held by the request and put back
 * once run has run (or the request is dropped by hid_core_stop) */
typedef struct hid_core_req
{
   int slot;
   int kind;
   uint32_t handle;
   uint32_t arg[6];
   void *ptr[3];
} hid_core_req_t;

bool hid_core_start(void (*run)(const hid_core_req_t *req));
/* the workers joined after the transfers they are in; those not begun are
 * put back without running */
void hid_core_stop(void);
/* false when the queue is full or no workers run: the caller runs it */
bool hid_core_submit(const hid_core_req_t *req);

RETRO_END_DECLS

#endif
