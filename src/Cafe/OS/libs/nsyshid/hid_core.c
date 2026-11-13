/* nsyshid's state (hid_core.h) */

#include <stddef.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

#include "hid_core.h"

/* A slot's ref: 0 free, 1 being filled or being freed, otherwise twice the
 * references held, plus one once the device is detached. Held: even and at
 * least 2. */
typedef struct dev
{
   retro_atomic_int_t ref;
   retro_atomic_int_t handle;
   retro_atomic_int_t id;
   retro_atomic_ptr_t key;
   retro_atomic_ptr_t obj;
   retro_atomic_ptr_t owner;
} dev_t_;

static dev_t_ s_devs[HID_CORE_DEVICES];
static retro_atomic_int_t s_last_handle; /* the first handed out is 2 */
static void (*s_release)(void *obj);

static retro_atomic_int_t s_clients[HID_CORE_CLIENTS];

#ifdef HID_CORE_TEST_HOOKS
void (*hid_core_test_stopping)(void);
/* in a lookup, between finding a slot and holding it */
void (*hid_core_test_mid_lookup)(void);
#define MID_LOOKUP() do { if (hid_core_test_mid_lookup) hid_core_test_mid_lookup(); } while (0)
#else
#define MID_LOOKUP() do { } while (0)
#endif

void hid_core_set_release(void (*release)(void *obj))
{
   s_release = release;
}

static bool pin(dev_t_ *d)
{
   int v = retro_atomic_load_acquire_int(&d->ref);
   while (v >= 2 && !(v & 1))
   {
      if (retro_atomic_cas_int(&d->ref, v, v + 2))
         return true;
      v = retro_atomic_load_acquire_int(&d->ref);
   }
   return false;
}

static void release(dev_t_ *d)
{
   void *obj = retro_atomic_load_acquire_ptr(&d->obj);
   retro_atomic_store_release_ptr(&d->key, NULL);
   retro_atomic_store_release_ptr(&d->obj, NULL);
   retro_atomic_store_release_ptr(&d->owner, NULL);
   retro_atomic_store_release_int(&d->handle, 0);
   retro_atomic_store_release_int(&d->id, 0);
   if (s_release && obj)
      s_release(obj);
   /* free only now: the slot's HID_t is not handed out again before */
   retro_atomic_store_release_int(&d->ref, 0);
}

void hid_core_put(int slot)
{
   dev_t_ *d;
   if (slot < 0 || slot >= HID_CORE_DEVICES)
      return;
   d = &s_devs[slot];
   if (retro_atomic_fetch_add_int(&d->ref, -2) - 2 == 1)
      release(d);
}

int hid_core_attach(const void *key, void *obj, const void *owner,
      uint32_t id, uint32_t *handle)
{
   int i;
   for (i = 0; i < HID_CORE_DEVICES; i++)
   {
      dev_t_ *d = &s_devs[i];
      if (retro_atomic_load_acquire_ptr(&d->key) == key && pin(d))
      {
         const bool same = retro_atomic_load_acquire_ptr(&d->key) == key;
         hid_core_put(i);
         if (same)
            return -1;
      }
   }
   for (i = 0; i < HID_CORE_DEVICES; i++)
   {
      dev_t_ *d = &s_devs[i];
      uint32_t h;
      if (retro_atomic_load_acquire_int(&d->ref) != 0
            || !retro_atomic_cas_int(&d->ref, 0, 1))
         continue;
      h = (uint32_t)retro_atomic_fetch_add_int(&s_last_handle, 1) + 2u;
      retro_atomic_store_release_ptr(&d->key, (void*)key);
      retro_atomic_store_release_ptr(&d->obj, obj);
      retro_atomic_store_release_ptr(&d->owner, (void*)owner);
      retro_atomic_store_release_int(&d->id, (int)id);
      retro_atomic_store_release_int(&d->handle, (int)h);
      /* the table's reference and the caller's */
      retro_atomic_store_release_int(&d->ref, 4);
      *handle = h;
      return i;
   }
   return -2;
}

bool hid_core_detach(const void *key)
{
   int i;
   for (i = 0; i < HID_CORE_DEVICES; i++)
   {
      dev_t_ *d = &s_devs[i];
      int v;
      if (retro_atomic_load_acquire_ptr(&d->key) != key)
         continue;
      v = retro_atomic_load_acquire_int(&d->ref);
      while (v >= 2 && !(v & 1))
      {
         /* the table's reference dropped, the slot marked detached */
         const int n = (v | 1) - 2;
         if (retro_atomic_cas_int(&d->ref, v, n))
         {
            if (n == 1)
               release(d);
            return true;
         }
         v = retro_atomic_load_acquire_int(&d->ref);
      }
      /* a slot freed and taken again meanwhile can hold key again only
       * if it was attached again: look on */
   }
   return false;
}

int hid_core_get_key(const void *key)
{
   int i;
   if (!key)
      return -1;
   for (i = 0; i < HID_CORE_DEVICES; i++)
   {
      dev_t_ *d = &s_devs[i];
      if (retro_atomic_load_acquire_ptr(&d->key) != key)
         continue;
      MID_LOOKUP();
      if (!pin(d))
         continue;
      if (retro_atomic_load_acquire_ptr(&d->key) == key)
         return i;
      hid_core_put(i);
   }
   return -1;
}

int hid_core_get_handle(uint32_t handle)
{
   int i;
   if (!handle)
      return -1;
   for (i = 0; i < HID_CORE_DEVICES; i++)
   {
      dev_t_ *d = &s_devs[i];
      if ((uint32_t)retro_atomic_load_acquire_int(&d->handle) != handle)
         continue;
      MID_LOOKUP();
      if (!pin(d))
         continue;
      if ((uint32_t)retro_atomic_load_acquire_int(&d->handle) == handle)
         return i;
      hid_core_put(i);
   }
   return -1;
}

int hid_core_get_id(uint32_t id)
{
   int i;
   for (i = 0; i < HID_CORE_DEVICES; i++)
   {
      dev_t_ *d = &s_devs[i];
      if ((uint32_t)retro_atomic_load_acquire_int(&d->id) != id)
         continue;
      MID_LOOKUP();
      if (!pin(d))
         continue;
      if ((uint32_t)retro_atomic_load_acquire_int(&d->id) == id)
         return i;
      hid_core_put(i);
   }
   return -1;
}

int hid_core_next(int *cursor, const void *owner)
{
   for (; *cursor < HID_CORE_DEVICES; (*cursor)++)
   {
      dev_t_ *d = &s_devs[*cursor];
      if (!pin(d))
         continue;
      if (!owner || retro_atomic_load_acquire_ptr(&d->owner) == owner)
         return (*cursor)++;
      hid_core_put(*cursor);
   }
   return -1;
}

void *hid_core_obj(int slot)
{
   return retro_atomic_load_acquire_ptr(&s_devs[slot].obj);
}

uint32_t hid_core_handle(int slot)
{
   return (uint32_t)retro_atomic_load_acquire_int(&s_devs[slot].handle);
}

bool hid_core_client_add(uint32_t client)
{
   int i;
   if (!client)
      return false;
   for (i = 0; i < HID_CORE_CLIENTS; i++)
      if (retro_atomic_load_acquire_int(&s_clients[i]) == 0
            && retro_atomic_cas_int(&s_clients[i], 0, (int)client))
         return true;
   return false;
}

bool hid_core_client_del(uint32_t client)
{
   int i;
   if (!client)
      return false;
   for (i = 0; i < HID_CORE_CLIENTS; i++)
      if (retro_atomic_cas_int(&s_clients[i], (int)client, 0))
         return true;
   return false;
}

uint32_t hid_core_client_next(int *cursor)
{
   /* the highest slot first: the one added last, while none were removed */
   while (*cursor < HID_CORE_CLIENTS)
   {
      const int i = HID_CORE_CLIENTS - 1 - (*cursor)++;
      const uint32_t c = (uint32_t)retro_atomic_load_acquire_int(&s_clients[i]);
      if (c)
         return c;
   }
   return 0;
}

void hid_core_clients_reset(void)
{
   int i;
   for (i = 0; i < HID_CORE_CLIENTS; i++)
      retro_atomic_store_release_int(&s_clients[i], 0);
}

/* the workers' queue */
typedef struct cell
{
   retro_atomic_int_t seq; /* kept less the cell's index: all zero is empty */
   hid_core_req_t req;
} cell_t;

static cell_t s_cells[HID_CORE_REQUESTS];
static retro_atomic_int_t s_enqueue;
static retro_atomic_int_t s_dequeue;
static retro_atomic_int_t s_running;
static retro_atomic_int_t s_stop;
static retro_eventcount_t s_wake;
static sthread_t *s_workers[HID_CORE_WORKERS];
static void (*s_run)(const hid_core_req_t *req);


#define CELL(pos) ((unsigned)(pos) % HID_CORE_REQUESTS)

static unsigned cell_seq(int pos)
{
   return (unsigned)retro_atomic_load_acquire_int(&s_cells[CELL(pos)].seq) + CELL(pos);
}

static void cell_set(int pos, unsigned seq)
{
   retro_atomic_store_release_int(&s_cells[CELL(pos)].seq, (int)(seq - CELL(pos)));
}

static bool push(const hid_core_req_t *req)
{
   int pos;
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&s_enqueue);
      dif = (int)(cell_seq(pos) - (unsigned)pos);
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&s_enqueue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return false;
   }
   s_cells[CELL(pos)].req = *req;
   cell_set(pos, (unsigned)pos + 1);
   return true;
}

static bool pop(hid_core_req_t *req)
{
   int pos;
   for (;;)
   {
      int dif;
      pos = retro_atomic_load_acquire_int(&s_dequeue);
      dif = (int)(cell_seq(pos) - ((unsigned)pos + 1));
      if (dif == 0)
      {
         if (retro_atomic_cas_int(&s_dequeue, pos, (int)((unsigned)pos + 1)))
            break;
      }
      else if (dif < 0)
         return false;
   }
   *req = s_cells[CELL(pos)].req;
   cell_set(pos, (unsigned)pos + HID_CORE_REQUESTS);
   return true;
}

static void worker(void *unused)
{
   hid_core_req_t req;
   (void)unused;
   for (;;)
   {
      int key;
      if (pop(&req))
      {
         s_run(&req);
         hid_core_put(req.slot);
         continue;
      }
      if (retro_atomic_load_acquire_int(&s_stop))
         return;
      key = retro_eventcount_prepare_wait(&s_wake);
      if (retro_atomic_load_acquire_int(&s_stop)
            || cell_seq(retro_atomic_load_acquire_int(&s_dequeue))
               == (unsigned)retro_atomic_load_acquire_int(&s_dequeue) + 1)
         retro_eventcount_cancel_wait(&s_wake);
      else
         retro_eventcount_commit_wait(&s_wake, key);
   }
}

bool hid_core_start(void (*run)(const hid_core_req_t *req))
{
   int i;
   if (retro_atomic_load_acquire_int(&s_running) || !run)
      return false;
   s_run = run;
   retro_atomic_store_release_int(&s_stop, 0);
   if (!retro_eventcount_init(&s_wake))
      return false;
   for (i = 0; i < HID_CORE_WORKERS; i++)
      s_workers[i] = sthread_create(worker, NULL);
   retro_atomic_store_release_int(&s_running, 1);
   return true;
}

void hid_core_stop(void)
{
   int i;
   hid_core_req_t req;
   if (!retro_atomic_load_acquire_int(&s_running))
      return;
   retro_atomic_store_release_int(&s_running, 0);
   /* what is queued now is dropped, not run */
   while (pop(&req))
      hid_core_put(req.slot);
#ifdef HID_CORE_TEST_HOOKS
   if (hid_core_test_stopping)
      hid_core_test_stopping();
#endif
   retro_atomic_store_release_int(&s_stop, 1);
   retro_eventcount_notify(&s_wake);
   for (i = 0; i < HID_CORE_WORKERS; i++)
   {
      if (s_workers[i])
         sthread_join(s_workers[i]);
      s_workers[i] = NULL;
   }
   while (pop(&req))
      hid_core_put(req.slot);
   retro_eventcount_free(&s_wake);
}

bool hid_core_submit(const hid_core_req_t *req)
{
   if (!retro_atomic_load_acquire_int(&s_running) || !push(req))
      return false;
   retro_eventcount_notify(&s_wake);
   return true;
}
