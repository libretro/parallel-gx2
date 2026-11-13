/* nsyshid's state (src/Cafe/OS/libs/nsyshid/hid_core.c): devices attached
 * and detached, held and put back, an object released once and only when
 * nothing holds it; the clients; transfers on the workers, those not begun
 * dropped when they stop - and, under TSan and ASan, devices coming and
 * going while threads look them up and transfer, with no lock. C89, as the
 * C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/nsyshid/hid_core.h"

extern void (*hid_core_test_stopping)(void);
extern void (*hid_core_test_mid_lookup)(void);

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

typedef struct obj
{
   retro_atomic_int_t alive;
   retro_atomic_int_t released;
   retro_atomic_int_t runs;
} obj_t;

static retro_atomic_int_t s_releases;

static void on_release(void *p)
{
   obj_t *o = (obj_t*)p;
   retro_atomic_fetch_add_int(&s_releases, 1);
   retro_atomic_fetch_add_int(&o->released, 1);
   retro_atomic_store_release_int(&o->alive, 0);
}

static void obj_init(obj_t *o)
{
   retro_atomic_int_init(&o->alive, 1);
   retro_atomic_int_init(&o->released, 0);
   retro_atomic_int_init(&o->runs, 0);
}

static int keys[HID_CORE_DEVICES + 1];

static void test_devices(void)
{
   static obj_t objs[HID_CORE_DEVICES + 1];
   const int owner_a = 0, owner_b = 0;
   uint32_t h, h1;
   int s, i, cursor, n;
   for (i = 0; i <= HID_CORE_DEVICES; i++)
      obj_init(&objs[i]);
   retro_atomic_int_init(&s_releases, 0);
   hid_core_set_release(on_release);

   s = hid_core_attach(&keys[0], &objs[0], &owner_a, HID_CORE_ID(0x1430, 0x0150), &h);
   CHECK(s == 0 && h == 2);
   hid_core_put(s);
   s = hid_core_attach(&keys[1], &objs[1], &owner_b, HID_CORE_ID(0x0E6F, 0x0129), &h1);
   CHECK(s == 1 && h1 == 3);
   hid_core_put(s);
   /* the same device twice: refused */
   CHECK(hid_core_attach(&keys[0], &objs[2], &owner_a, 1, &h) == -1);

   /* found by handle, id and key; not by what is not there */
   CHECK(hid_core_get_handle(2) == 0 && hid_core_obj(0) == &objs[0]);
   hid_core_put(0);
   CHECK(hid_core_get_handle(3) == 1 && hid_core_handle(1) == 3);
   hid_core_put(1);
   CHECK(hid_core_get_handle(0) == -1 && hid_core_get_handle(4) == -1);
   CHECK(hid_core_get_id(HID_CORE_ID(0x0E6F, 0x0129)) == 1);
   hid_core_put(1);
   CHECK(hid_core_get_id(HID_CORE_ID(0x0E6F, 0x0241)) == -1);
   CHECK(hid_core_get_key(&keys[0]) == 0);
   hid_core_put(0);
   CHECK(hid_core_get_key(&keys[5]) == -1 && hid_core_get_key(NULL) == -1);

   /* by owner, or all */
   cursor = 0;
   CHECK(hid_core_next(&cursor, &owner_b) == 1);
   hid_core_put(1);
   CHECK(hid_core_next(&cursor, &owner_b) == -1);
   cursor = 0;
   n = 0;
   while ((s = hid_core_next(&cursor, NULL)) >= 0)
   {
      n++;
      hid_core_put(s);
   }
   CHECK(n == 2);

   /* detached while held: gone from lookups, released at the last put */
   CHECK(hid_core_get_handle(2) == 0);
   CHECK(hid_core_get_handle(2) == 0);
   CHECK(hid_core_detach(&keys[0]));
   CHECK(!hid_core_detach(&keys[0]));
   CHECK(hid_core_get_handle(2) == -1 && hid_core_get_key(&keys[0]) == -1);
   cursor = 0;
   CHECK(hid_core_next(&cursor, &owner_a) == -1);
   hid_core_put(0);
   CHECK(retro_atomic_load_acquire_int(&objs[0].released) == 0);
   hid_core_put(0);
   CHECK(retro_atomic_load_acquire_int(&objs[0].released) == 1);
   /* its slot free again, under a new handle */
   s = hid_core_attach(&keys[0], &objs[HID_CORE_DEVICES], &owner_a, 7, &h);
   CHECK(s == 0 && h == 4);
   hid_core_put(s);
   CHECK(hid_core_get_handle(2) == -1);
   /* detached when nothing holds it: released at once */
   CHECK(hid_core_detach(&keys[1]));
   CHECK(retro_atomic_load_acquire_int(&objs[1].released) == 1);
   CHECK(!hid_core_detach(&keys[7]));

   /* full */
   for (i = 1; i < HID_CORE_DEVICES; i++)
   {
      s = hid_core_attach(&keys[i], &objs[i], &owner_a, 9, &h);
      CHECK(s == i);
      hid_core_put(s);
   }
   CHECK(hid_core_attach(&keys[HID_CORE_DEVICES], &objs[HID_CORE_DEVICES], &owner_a, 9, &h) == -2);
   for (i = 0; i < HID_CORE_DEVICES; i++)
      CHECK(hid_core_detach(&keys[i]));
   CHECK(retro_atomic_load_acquire_int(&s_releases) == 2 + HID_CORE_DEVICES);
   for (i = 1; i < HID_CORE_DEVICES; i++)
      CHECK(retro_atomic_load_acquire_int(&objs[i].released) == 1 + (i == 1));
}

static void test_clients(void)
{
   int cursor, i;
   hid_core_clients_reset();
   CHECK(!hid_core_client_add(0));
   CHECK(hid_core_client_add(0x10000000) && hid_core_client_add(0x10000100) && hid_core_client_add(0x10000200));
   cursor = 0;
   CHECK(hid_core_client_next(&cursor) == 0x10000200);
   CHECK(hid_core_client_next(&cursor) == 0x10000100);
   CHECK(hid_core_client_next(&cursor) == 0x10000000);
   CHECK(hid_core_client_next(&cursor) == 0);
   CHECK(hid_core_client_del(0x10000100) && !hid_core_client_del(0x10000100));
   cursor = 0;
   CHECK(hid_core_client_next(&cursor) == 0x10000200);
   CHECK(hid_core_client_next(&cursor) == 0x10000000);
   CHECK(hid_core_client_next(&cursor) == 0);
   for (i = 3; i <= HID_CORE_CLIENTS; i++)
      CHECK(hid_core_client_add(0x20000000 + i));
   CHECK(!hid_core_client_add(0x30000000));
   hid_core_clients_reset();
   cursor = 0;
   CHECK(hid_core_client_next(&cursor) == 0);
}

/* a lookup that has found a slot when its device goes and another takes
 * the slot: not handed the other one */
static obj_t s_first, s_second;
static int s_owner;

static void swap_device(void)
{
   uint32_t h;
   int s;
   hid_core_test_mid_lookup = NULL;
   CHECK(hid_core_detach(&keys[0]));
   s = hid_core_attach(&keys[1], &s_second, &s_owner, 2, &h);
   CHECK(s == 0);
   hid_core_put(s);
}

static void test_lookup_race(void)
{
   uint32_t h;
   int s, which;
   hid_core_set_release(on_release);
   for (which = 0; which < 3; which++)
   {
      obj_init(&s_first);
      obj_init(&s_second);
      s = hid_core_attach(&keys[0], &s_first, &s_owner, 1, &h);
      CHECK(s == 0);
      hid_core_put(s);
      hid_core_test_mid_lookup = swap_device;
      if (which == 0)
         CHECK(hid_core_get_handle(h) == -1);
      else if (which == 1)
         CHECK(hid_core_get_id(1) == -1);
      else
         CHECK(hid_core_get_key(&keys[0]) == -1);
      hid_core_test_mid_lookup = NULL;
      CHECK(retro_atomic_load_acquire_int(&s_first.released) == 1);
      CHECK(hid_core_detach(&keys[1]));
   }
}

/* a slot is not taken again before its object is released */
static int s_reattached;
static obj_t s_third;

static void release_reattach(void *p)
{
   uint32_t h;
   on_release(p);
   if (p != &s_first)
      return;
   s_reattached = hid_core_attach(&keys[2], &s_third, &s_owner, 3, &h);
   if (s_reattached >= 0)
      hid_core_put(s_reattached);
}

static void test_release_order(void)
{
   uint32_t h;
   int s;
   obj_init(&s_first);
   obj_init(&s_third);
   hid_core_set_release(release_reattach);
   s = hid_core_attach(&keys[0], &s_first, &s_owner, 1, &h);
   CHECK(s == 0);
   hid_core_put(s);
   s_reattached = -9;
   CHECK(hid_core_detach(&keys[0]));
   CHECK(s_reattached == 1);
   hid_core_set_release(on_release);
   CHECK(hid_core_detach(&keys[2]));
   CHECK(hid_core_get_key(&keys[0]) == -1);
}

/* the workers */
static retro_atomic_int_t s_hold;
static retro_atomic_int_t s_holding;
static retro_atomic_int_t s_ran;
static retro_atomic_int_t s_dead;

enum { KIND_PLAIN = 0, KIND_HOLD };

static void run(const hid_core_req_t *req)
{
   obj_t *o = (obj_t*)hid_core_obj(req->slot);
   if (!retro_atomic_load_acquire_int(&o->alive) || hid_core_handle(req->slot) != req->handle)
      retro_atomic_fetch_add_int(&s_dead, 1);
   retro_atomic_fetch_add_int(&o->runs, 1);
   if (req->kind == KIND_HOLD)
   {
      retro_atomic_fetch_add_int(&s_holding, 1);
      while (retro_atomic_load_acquire_int(&s_hold))
         ;
   }
   retro_atomic_fetch_add_int(&s_ran, 1);
}

static void let_go(void)
{
   retro_atomic_store_release_int(&s_hold, 0);
}

static bool submit(int kind, uint32_t handle)
{
   hid_core_req_t req;
   int slot = hid_core_get_handle(handle);
   if (slot < 0)
      return false;
   memset(&req, 0, sizeof(req));
   req.slot = slot;
   req.kind = kind;
   req.handle = handle;
   if (hid_core_submit(&req))
      return true;
   hid_core_put(slot);
   return false;
}

static void test_workers(void)
{
   static obj_t o;
   const int owner = 0;
   uint32_t h;
   int s, i;
   obj_init(&o);
   retro_atomic_int_init(&s_releases, 0);
   retro_atomic_int_init(&s_ran, 0);
   retro_atomic_int_init(&s_dead, 0);
   retro_atomic_int_init(&s_holding, 0);
   retro_atomic_int_init(&s_hold, 1);

   s = hid_core_attach(&keys[0], &o, &owner, 1, &h);
   hid_core_put(s);
   CHECK(!submit(KIND_PLAIN, h)); /* no workers yet */
   CHECK(!hid_core_start(NULL));
   CHECK(hid_core_start(run));
   CHECK(!hid_core_start(run));

   /* every worker held in a transfer: the queue fills and refuses */
   for (i = 0; i < HID_CORE_WORKERS; i++)
      CHECK(submit(KIND_HOLD, h));
   while (retro_atomic_load_acquire_int(&s_holding) < HID_CORE_WORKERS)
      ;
   for (i = 0; i < HID_CORE_REQUESTS; i++)
      CHECK(submit(KIND_PLAIN, h));
   CHECK(!submit(KIND_PLAIN, h));

   /* detached with transfers queued and running: not released until the
    * last is done or dropped */
   CHECK(hid_core_detach(&keys[0]));
   CHECK(retro_atomic_load_acquire_int(&o.released) == 0);
   /* stopped: the queued ones dropped, the running ones finished */
   hid_core_test_stopping = let_go;
   hid_core_stop();
   hid_core_test_stopping = NULL;
   CHECK(retro_atomic_load_acquire_int(&s_ran) == HID_CORE_WORKERS);
   CHECK(retro_atomic_load_acquire_int(&o.released) == 1);
   CHECK(retro_atomic_load_acquire_int(&s_dead) == 0);
   CHECK(!submit(KIND_PLAIN, h));

   /* started again: each one run once */
   obj_init(&o);
   retro_atomic_int_init(&s_ran, 0);
   s = hid_core_attach(&keys[0], &o, &owner, 1, &h);
   hid_core_put(s);
   CHECK(hid_core_start(run));
   for (i = 0; i < 1000; i++)
      while (!submit(KIND_PLAIN, h))
         ;
   while (retro_atomic_load_acquire_int(&s_ran) < 1000)
      ;
   hid_core_stop();
   CHECK(retro_atomic_load_acquire_int(&s_ran) == 1000);
   CHECK(retro_atomic_load_acquire_int(&o.runs) == 1000);
   CHECK(hid_core_detach(&keys[0]));
   CHECK(retro_atomic_load_acquire_int(&o.released) == 1);
}

/* devices plugged and pulled while titles' threads transfer to them */
#define STRESS_KEYS 8
static retro_atomic_int_t s_attached;
static retro_atomic_int_t s_stop_users;

static void on_release_free(void *p)
{
   obj_t *o = (obj_t*)p;
   if (retro_atomic_load_acquire_int(&o->runs) < 0)
      retro_atomic_fetch_add_int(&s_dead, 1);
   retro_atomic_fetch_add_int(&s_releases, 1);
   free(o);
}

static void run_check(const hid_core_req_t *req)
{
   /* freed would be caught by ASan here */
   obj_t *o = (obj_t*)hid_core_obj(req->slot);
   if (!retro_atomic_load_acquire_int(&o->alive) || hid_core_handle(req->slot) != req->handle)
      retro_atomic_fetch_add_int(&s_dead, 1);
   retro_atomic_fetch_add_int(&o->runs, 1);
   retro_atomic_fetch_add_int(&s_ran, 1);
}

static void plugger(void *ud)
{
   const int base = (int)(size_t)ud * STRESS_KEYS;
   int i;
   /* on until the users have met the devices often enough */
   for (i = 0; i < 4000 || retro_atomic_load_acquire_int(&s_ran) < 2000; i++)
   {
      const int k = base + (i % STRESS_KEYS);
      uint32_t h;
      const int held = hid_core_get_key(&keys[k]);
      if (held >= 0)
      {
         CHECK(hid_core_detach(&keys[k]));
         hid_core_put(held);
      }
      else
      {
         obj_t *o = (obj_t*)malloc(sizeof(*o));
         int s;
         obj_init(o);
         s = hid_core_attach(&keys[k], o, NULL, (uint32_t)k, &h);
         if (s >= 0)
         {
            retro_atomic_fetch_add_int(&s_attached, 1);
            hid_core_put(s);
         }
         else
            free(o);
      }
   }
}

static void user(void *ud)
{
   unsigned x = (unsigned)(size_t)ud * 2654435761u + 1;
   while (!retro_atomic_load_acquire_int(&s_stop_users))
   {
      hid_core_req_t req;
      int s;
      x = x * 1103515245u + 12345u;
      s = hid_core_get_id((x >> 8) % (2 * STRESS_KEYS));
      if (s < 0)
         continue;
      memset(&req, 0, sizeof(req));
      req.slot = s;
      req.handle = hid_core_handle(s);
      if (x & 0x10000)
      {
         if (!hid_core_submit(&req))
            hid_core_put(s);
      }
      else
      {
         run_check(&req);
         hid_core_put(s);
      }
   }
}

static void test_stress(void)
{
   sthread_t *p[2], *u[3];
   int i, cursor, s;
   retro_atomic_int_init(&s_attached, 0);
   retro_atomic_int_init(&s_releases, 0);
   retro_atomic_int_init(&s_dead, 0);
   retro_atomic_int_init(&s_ran, 0);
   retro_atomic_int_init(&s_stop_users, 0);
   hid_core_set_release(on_release_free);
   CHECK(hid_core_start(run_check));
   for (i = 0; i < 3; i++)
      u[i] = sthread_create(user, (void*)(size_t)(i + 1));
   for (i = 0; i < 2; i++)
      p[i] = sthread_create(plugger, (void*)(size_t)i);
   for (i = 0; i < 2; i++)
      sthread_join(p[i]);
   retro_atomic_store_release_int(&s_stop_users, 1);
   for (i = 0; i < 3; i++)
      sthread_join(u[i]);
   hid_core_stop();
   /* what is still attached goes now */
   cursor = 0;
   while ((s = hid_core_next(&cursor, NULL)) >= 0)
   {
      const void *k = NULL;
      int j;
      for (j = 0; j < 2 * STRESS_KEYS; j++)
      {
         const int t = hid_core_get_key(&keys[j]);
         if (t == s)
            k = &keys[j];
         if (t >= 0)
            hid_core_put(t);
      }
      hid_core_put(s);
      CHECK(k != NULL && hid_core_detach(k));
   }
   CHECK(retro_atomic_load_acquire_int(&s_dead) == 0);
   CHECK(retro_atomic_load_acquire_int(&s_releases) == retro_atomic_load_acquire_int(&s_attached));
   CHECK(retro_atomic_load_acquire_int(&s_ran) > 0);
}

int main(void)
{
   test_devices();
   test_clients();
   test_lookup_race();
   test_release_order();
   test_workers();
   test_stress();
   if (failures)
   {
      fprintf(stderr, "hid_core_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("hid_core_test: all passed\n");
   return 0;
}
