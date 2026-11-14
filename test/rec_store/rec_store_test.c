/* The recompiler's bookkeeping without a lock (src/Cafe/HW/Espresso/
 * Recompiler/rec_store.c): the functions by the code they cover, found by
 * any thread while the recompiler's thread puts them in and takes them out,
 * a node handed back for freeing only once no look can still be on it, a
 * look on one taken out going on past it; the log of invalidations, which
 * says so when it can no longer tell; and - the point of both - a function
 * put in while an invalidation of its code comes is always seen by one of
 * the two. Under TSan and ASan, threads at once. C89, as the C89 lane
 * builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/HW/Espresso/Recompiler/rec_store.h"

extern void (*rec_store_test_mid_find)(void);

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define BUCKETS 8
#define GRAN 0x100
static retro_atomic_ptr_t s_heads[BUCKETS];
static rec_store_t s_s;

static int s_freed;
static void free_group(rec_node_t *first, void *ud)
{
   (void)ud;
   s_freed++;
   free(first);
}

static rec_node_t *put(void *obj, uint32_t start, uint32_t end)
{
   size_t n = rec_store_nodes(&s_s, start, end);
   rec_node_t *nodes = (rec_node_t*)calloc(n, sizeof(rec_node_t));
   rec_store_insert(&s_s, nodes, obj, start, end);
   return nodes;
}

/* what a look found: a bit per object */
static unsigned s_found;
static void found(void *obj, uint32_t start, uint32_t end, void *ud)
{
   (void)start; (void)end; (void)ud;
   s_found |= 1u << (unsigned)(size_t)obj;
}

static unsigned look(uint32_t start, uint32_t end)
{
   s_found = 0;
   rec_store_find(&s_s, start, end, found, NULL);
   return s_found;
}

static void test_basic(void)
{
   rec_node_t *a, *b, *c, *d;
   rec_store_init(&s_s, s_heads, BUCKETS, GRAN);
   CHECK(rec_store_nodes(&s_s, 0x10, 0x20) == 1);
   CHECK(rec_store_nodes(&s_s, 0xF0, 0x110) == 2);
   CHECK(rec_store_nodes(&s_s, 0x100, 0x100) == 1);           /* empty: its bucket */
   CHECK(rec_store_nodes(&s_s, 0, 0x10000) == BUCKETS);        /* every bucket, once */
   a = put((void*)1, 0x10, 0x20);
   b = put((void*)2, 0xF0, 0x110);                             /* across two buckets */
   c = put((void*)3, 0x800 + 0x10, 0x800 + 0x20);              /* a's bucket, 8 on */
   d = put((void*)4, 0, 0x10000);                              /* everywhere */
   CHECK(look(0x10, 0x11) == ((1u << 1) | (1u << 4)));         /* not c, in a's bucket */
   CHECK(look(0x100, 0x104) == ((1u << 2) | (1u << 4)));
   CHECK(look(0xEF, 0xF0) == (1u << 4));                       /* end is past it */
   CHECK(look(0x20, 0x30) == (1u << 4));                       /* a ends where this starts */
   CHECK(look(0x810, 0x811) == ((1u << 3) | (1u << 4)));
   CHECK(look(0x20000, 0x20010) == 0);                         /* d ends at 0x10000 */
   rec_store_remove(&s_s, b, b[0].group);
   CHECK(look(0x100, 0x104) == (1u << 4));
   CHECK(look(0xF0, 0xF1) == (1u << 4));
   s_freed = 0;
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(s_freed == 1);
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(s_freed == 1);                                        /* once */
   rec_store_remove(&s_s, d, d[0].group);
   rec_store_remove(&s_s, a, a[0].group);
   rec_store_remove(&s_s, c, c[0].group);
   CHECK(look(0, 0x10000) == 0);
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(s_freed == 4);
}

/* taken out while a look is on it: the look goes on past it, and it is
 * not freed until the look is done */
static rec_node_t *s_victim;
static void remove_mid_find(void)
{
   rec_store_test_mid_find = NULL;
   rec_store_remove(&s_s, s_victim, s_victim[0].group);
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(s_freed == 0);
}

static void test_remove_under_look(void)
{
   rec_node_t *first, *second;
   rec_store_init(&s_s, s_heads, BUCKETS, GRAN);
   s_freed = 0;
   second = put((void*)2, 0x10, 0x20);
   first  = put((void*)1, 0x30, 0x40); /* the head: passed first */
   s_victim = first;
   rec_store_test_mid_find = remove_mid_find;
   CHECK(look(0, 0x100) == ((1u << 1) | (1u << 2)));
   rec_store_test_mid_find = NULL;
   CHECK(s_freed == 0);
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(s_freed == 1);
   CHECK(look(0, 0x100) == (1u << 2));
   rec_store_remove(&s_s, second, second[0].group);
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(s_freed == 2);
}

static rec_inval_t s_v;

static void test_inval(void)
{
   int from, i;
   rec_inval_init(&s_v);
   from = rec_inval_now(&s_v);
   CHECK(!rec_inval_since(&s_v, from, 0, 0x1000));
   rec_inval_add(&s_v, 0x100, 0x120);
   CHECK(rec_inval_since(&s_v, from, 0x110, 0x111));
   CHECK(rec_inval_since(&s_v, from, 0x0, 0x101));
   CHECK(!rec_inval_since(&s_v, from, 0x120, 0x200));          /* end is past it */
   CHECK(!rec_inval_since(&s_v, from, 0, 0x100));
   CHECK(!rec_inval_since(&s_v, rec_inval_now(&s_v), 0, 0x1000));
   /* exactly as many as it keeps: still told apart */
   from = rec_inval_now(&s_v);
   for (i = 0; i < REC_INVAL_SLOTS; i++)
      rec_inval_add(&s_v, 0x1000 + (uint32_t)i, 0x1001 + (uint32_t)i);
   CHECK(!rec_inval_since(&s_v, from, 0, 0x1000));
   CHECK(rec_inval_since(&s_v, from, 0x1000 + REC_INVAL_SLOTS - 1, 0x1000 + REC_INVAL_SLOTS));
   /* one more: the first is gone, so everything is touched */
   rec_inval_add(&s_v, 0x5000, 0x5001);
   CHECK(rec_inval_since(&s_v, from, 0, 0x10));
   CHECK(!rec_inval_since(&s_v, from + 1, 0, 0x10));
   /* one being written: touched */
   from = rec_inval_now(&s_v);
   rec_inval_add(&s_v, 0x9000, 0x9001);
   retro_atomic_store_release_int(&s_v.slots[(unsigned)from % REC_INVAL_SLOTS].seq, 0);
   CHECK(rec_inval_since(&s_v, from, 0, 0x10));
}

/* The recompiler's thread putting functions in and taking them out, the
 * cores looking (ASan: nothing read once freed); and a function put in
 * while its code is invalidated, each round: the recompiler sees the
 * invalidation in the log or the invalidation finds it in the store */
#define ROUNDS 5000
static retro_atomic_int_t s_go;
static retro_atomic_int_t s_round;
static retro_atomic_int_t s_done_inval;
static retro_atomic_int_t s_found_obj;
static retro_atomic_int_t s_missed;
static retro_atomic_int_t s_stop;

static void found_round(void *obj, uint32_t start, uint32_t end, void *ud)
{
   (void)start; (void)end;
   if ((int)(size_t)obj == *(int*)ud)
      retro_atomic_store_release_int(&s_found_obj, 1);
}

static void invalidator(void *ud)
{
   int r;
   (void)ud;
   for (r = 1; r <= ROUNDS; r++)
   {
      while (retro_atomic_load_acquire_int(&s_round) != r)
         sthread_yield();
      rec_inval_add(&s_v, 0x200, 0x204);
      rec_store_find(&s_s, 0x200, 0x204, found_round, &r);
      retro_atomic_store_release_int(&s_done_inval, r);
   }
}

static void looker(void *ud)
{
   unsigned seed = (unsigned)(size_t)ud;
   while (!retro_atomic_load_acquire_int(&s_stop))
   {
      uint32_t a;
      seed = seed * 1103515245u + 12345u;
      a = (seed >> 8) % 0x2000;
      rec_store_find(&s_s, a, a + 0x40, found_round, &seed);
      sthread_yield();
   }
}

static void test_threads(void)
{
   sthread_t *inv, *l1, *l2;
   int r, i;
   rec_node_t *keep[16];
   memset(keep, 0, sizeof(keep));
   rec_store_init(&s_s, s_heads, BUCKETS, GRAN);
   rec_inval_init(&s_v);
   s_freed = 0;
   retro_atomic_int_init(&s_go, 0);
   retro_atomic_int_init(&s_round, 0);
   retro_atomic_int_init(&s_done_inval, 0);
   retro_atomic_int_init(&s_missed, 0);
   retro_atomic_int_init(&s_stop, 0);
   inv = sthread_create(invalidator, NULL);
   l1  = sthread_create(looker, (void*)(size_t)7);
   l2  = sthread_create(looker, (void*)(size_t)13);
   for (r = 1; r <= ROUNDS; r++)
   {
      int since;
      rec_node_t *nodes;
      bool saw;
      /* churn: functions elsewhere put in and taken out under the lookers */
      i = r & 15;
      if (keep[i])
         rec_store_remove(&s_s, keep[i], keep[i][0].group);
      keep[i] = put((void*)(size_t)(100000 + r), 0x400 + (uint32_t)i * 0x80, 0x480 + (uint32_t)i * 0x80);
      rec_store_collect(&s_s, free_group, NULL);
      /* the race: translated from here on, then put in and checked */
      since = rec_inval_now(&s_v);
      retro_atomic_int_init(&s_found_obj, 0);
      retro_atomic_store_release_int(&s_round, r);
      nodes = put((void*)(size_t)r, 0x1F0, 0x210);
      saw   = rec_inval_since(&s_v, since, 0x1F0, 0x210);
      while (retro_atomic_load_acquire_int(&s_done_inval) != r)
         sthread_yield();
      if (!saw && !retro_atomic_load_acquire_int(&s_found_obj))
         retro_atomic_fetch_add_int(&s_missed, 1);
      rec_store_remove(&s_s, nodes, nodes[0].group);
   }
   retro_atomic_store_release_int(&s_stop, 1);
   sthread_join(inv);
   sthread_join(l1);
   sthread_join(l2);
   for (i = 0; i < 16; i++)
      if (keep[i])
         rec_store_remove(&s_s, keep[i], keep[i][0].group);
   rec_store_collect(&s_s, free_group, NULL);
   CHECK(retro_atomic_load_acquire_int(&s_missed) == 0);
   CHECK(s_freed == ROUNDS + ROUNDS);
   CHECK(look(0, 0x10000) == 0);
}

/* The same race as tight as it goes, one bucket, two threads spinning:
 * where a store can be passed by the load after it (x86 too), a function
 * put in and an invalidation of its code each miss the other unless the
 * store says otherwise */
#define SB_ROUNDS 200000
static retro_atomic_ptr_t s_sb_heads[1];
static rec_store_t s_sb;
static rec_node_t s_sb_node;
static retro_atomic_int_t s_sb_go, s_sb_done, s_sb_found;

static void sb_found(void *obj, uint32_t start, uint32_t end, void *ud)
{
   (void)obj; (void)start; (void)end; (void)ud;
   retro_atomic_store_relaxed_int(&s_sb_found, 1);
}

static void sb_wait(retro_atomic_int_t *v, int r)
{
   unsigned spins = 0;
   while (retro_atomic_load_acquire_int(v) != r)
      if (!(++spins & 1023))
         sthread_yield();
}

static void sb_invalidator(void *ud)
{
   int r;
   (void)ud;
   for (r = 1; r <= SB_ROUNDS; r++)
   {
      sb_wait(&s_sb_go, r);
      rec_inval_add(&s_v, 0, 4);
      rec_store_find(&s_sb, 0, 4, sb_found, NULL);
      retro_atomic_store_release_int(&s_sb_done, r);
   }
}

static void sb_free(rec_node_t *first, void *ud)
{
   (void)first; (void)ud;
}

static void test_store_buffering(void)
{
   sthread_t *t;
   int r, missed = 0;
   rec_store_init(&s_sb, s_sb_heads, 1, 0x1000);
   rec_inval_init(&s_v);
   retro_atomic_int_init(&s_sb_go, 0);
   retro_atomic_int_init(&s_sb_done, 0);
   t = sthread_create(sb_invalidator, NULL);
   for (r = 1; r <= SB_ROUNDS; r++)
   {
      const int since = rec_inval_now(&s_v);
      bool saw;
      retro_atomic_store_relaxed_int(&s_sb_found, 0);
      retro_atomic_store_release_int(&s_sb_go, r);
      rec_store_insert(&s_sb, &s_sb_node, (void*)1, 0, 4);
      saw = rec_inval_since(&s_v, since, 0, 4);
      sb_wait(&s_sb_done, r);
      if (!saw && !retro_atomic_load_relaxed_int(&s_sb_found))
         missed++;
      rec_store_remove(&s_sb, &s_sb_node, 1);
      rec_store_collect(&s_sb, sb_free, NULL);
   }
   sthread_join(t);
   CHECK(missed == 0);
}

int main(void)
{
   test_basic();
   test_remove_under_look();
   test_inval();
   test_threads();
   test_store_buffering();
   if (failures)
   {
      fprintf(stderr, "rec_store_test: %d failed\n", failures);
      return 1;
   }
   printf("rec_store_test: all passed\n");
   return 0;
}
