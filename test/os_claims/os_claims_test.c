/* coreinit's small claims without a lock (src/Cafe/OS/libs/coreinit/
 * os_claims.c): GHS's file locks taken lowest first, each by one thread and
 * given back once; and the system area bumped aligned, refused when it
 * would run past its end without moving, and - under TSan and ASan -
 * threads taking flags and space at once never handed the same. C89, as
 * the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/coreinit/os_claims.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define FLAGS 100
static retro_atomic_int_t s_flags[FLAGS];
static retro_atomic_int_t s_off;

static void clear_flags(void)
{
   int i;
   for (i = 0; i < FLAGS; i++)
      retro_atomic_int_init(&s_flags[i], 0);
}

static void test_flags(void)
{
   int i;
   clear_flags();
   CHECK(os_claim_first(s_flags, FLAGS) == 0);
   CHECK(os_claim_first(s_flags, FLAGS) == 1);
   CHECK(os_claim_first(s_flags, FLAGS) == 2);
   CHECK(os_claim_release(s_flags, FLAGS, 1));
   CHECK(!os_claim_release(s_flags, FLAGS, 1));          /* once */
   CHECK(!os_claim_release(s_flags, FLAGS, 50));         /* never taken */
   CHECK(!os_claim_release(s_flags, FLAGS, -1));
   CHECK(!os_claim_release(s_flags, FLAGS, FLAGS));
   CHECK(os_claim_first(s_flags, FLAGS) == 1);           /* the lowest free */
   for (i = 3; i < FLAGS; i++)
      CHECK(os_claim_first(s_flags, FLAGS) == i);
   CHECK(os_claim_first(s_flags, FLAGS) == -1);
   CHECK(os_claim_release(s_flags, FLAGS, FLAGS - 1));
   CHECK(os_claim_first(s_flags, FLAGS) == FLAGS - 1);
   CHECK(os_claim_first(s_flags, 0) == -1);
}

static void test_bump(void)
{
   retro_atomic_int_init(&s_off, 0);
   CHECK(os_bump_alloc(&s_off, 5, 1, 1000) == 0);        /* 5 takes 8 */
   CHECK(retro_atomic_load_acquire_int(&s_off) == 8);
   CHECK(os_bump_alloc(&s_off, 4, 32, 1000) == 32);
   CHECK(os_bump_alloc(&s_off, 1, 0, 1000) == 36);       /* 0: no alignment */
   CHECK(os_bump_alloc(&s_off, 0, 24, 1000) == 48);      /* any alignment */
   CHECK(retro_atomic_load_acquire_int(&s_off) == 48);
   CHECK(os_bump_alloc(&s_off, 100, 1, 1000) == 48);
   /* past the end, or reaching it: refused, nothing moved */
   CHECK(os_bump_alloc(&s_off, 852, 1, 1000) == 0xFFFFFFFFu);
   CHECK(retro_atomic_load_acquire_int(&s_off) == 148);
   CHECK(os_bump_alloc(&s_off, 0xFFFFFFFFu, 1, 1000) == 0xFFFFFFFFu);
   CHECK(os_bump_alloc(&s_off, 4, 0x80000000u, 1000) == 0xFFFFFFFFu);
   CHECK(retro_atomic_load_acquire_int(&s_off) == 148);
   CHECK(os_bump_alloc(&s_off, 848, 1, 1000) == 148);    /* ends at 996 */
   CHECK(os_bump_alloc(&s_off, 4, 1, 1000) == 0xFFFFFFFFu);
   /* offsets past 2 GiB */
   retro_atomic_int_init(&s_off, (int)0x90000000u);
   CHECK(os_bump_alloc(&s_off, 16, 0x100, 0xA0000000u) == 0x90000000u);
   CHECK(os_bump_alloc(&s_off, 16, 0x100, 0xA0000000u) == 0x90000100u);
   CHECK((uint32_t)retro_atomic_load_acquire_int(&s_off) == 0x90000110u);
   /* aligned past 4 GiB: refused, not wrapped round to 0 */
   CHECK(os_bump_alloc(&s_off, 16, 0x80000000u, 0xA0000000u) == 0xFFFFFFFFu);
   CHECK((uint32_t)retro_atomic_load_acquire_int(&s_off) == 0x90000110u);
}

/* threads claiming flags and space at once */
#define THREADS 4
#define ROUNDS 20000
#define AREA (THREADS * ROUNDS * 32 + 64)
static retro_atomic_int_t s_owner[FLAGS];
static unsigned char *s_area_owner;
static retro_atomic_int_t s_bad;
static retro_atomic_int_t s_go;

static void worker(void *ud)
{
   int id = (int)(size_t)ud;
   int i;
   while (!retro_atomic_load_acquire_int(&s_go))
      sthread_yield();
   for (i = 0; i < ROUNDS; i++)
   {
      uint32_t off, j;
      int f = os_claim_first(s_flags, FLAGS);
      if (f >= 0)
      {
         if (!retro_atomic_cas_int(&s_owner[f], 0, id))
            retro_atomic_fetch_add_int(&s_bad, 1);
         if (!retro_atomic_cas_int(&s_owner[f], id, 0))
            retro_atomic_fetch_add_int(&s_bad, 1);
         if (!os_claim_release(s_flags, FLAGS, f))
            retro_atomic_fetch_add_int(&s_bad, 1);
      }
      off = os_bump_alloc(&s_off, 5 + (uint32_t)(i & 7), (i & 1) ? 16 : 4, AREA);
      if (off == 0xFFFFFFFFu)
      {
         retro_atomic_fetch_add_int(&s_bad, 1);
         continue;
      }
      /* each byte written by one thread only: TSan and the check below */
      for (j = 0; j < 5 + (uint32_t)(i & 7); j++)
      {
         if (s_area_owner[off + j])
            retro_atomic_fetch_add_int(&s_bad, 1);
         s_area_owner[off + j] = (unsigned char)id;
      }
   }
}

static void test_threads(void)
{
   sthread_t *t[THREADS];
   int i;
   clear_flags();
   for (i = 0; i < FLAGS; i++)
      retro_atomic_int_init(&s_owner[i], 0);
   /* a few taken for good, so the threads meet over the rest */
   for (i = 0; i < FLAGS - 3; i++)
      os_claim_first(s_flags, FLAGS);
   retro_atomic_int_init(&s_off, 0);
   retro_atomic_int_init(&s_bad, 0);
   retro_atomic_int_init(&s_go, 0);
   s_area_owner = (unsigned char*)calloc(AREA, 1);
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)(i + 1));
   retro_atomic_store_release_int(&s_go, 1);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   CHECK(retro_atomic_load_acquire_int(&s_bad) == 0);
   /* the three left free again, and the space used up to what was taken */
   CHECK(os_claim_first(s_flags, FLAGS) == FLAGS - 3);
   CHECK(retro_atomic_load_acquire_int(&s_off) <= AREA);
   free(s_area_owner);
}

int main(void)
{
   test_flags();
   test_bump();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "os_claims_test: %d failed\n", failures);
      return 1;
   }
   printf("os_claims_test: all passed\n");
   return 0;
}
