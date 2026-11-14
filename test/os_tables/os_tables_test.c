/* coreinit's tables without locks: the virtual address range a title
 * allocates from (src/Cafe/OS/libs/coreinit/virt_range.c) - the lowest
 * free pages that fit, freed once and only where an allocation begins, a
 * range across words given back whole when another takes part of it - and
 * the FSA clients (fsa_clients.c) - found by handle, a closed one not
 * mistaken for the next in its slot, taken out once. Under TSan and ASan,
 * threads allocate and free at once and none is handed pages another
 * holds. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/libs/coreinit/virt_range.h"
#include "Cafe/OS/libs/coreinit/fsa_clients.h"

extern void (*virt_range_test_mid_alloc)(void);

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define BASE 0xA0000000u
#define SIZE 0x40000000u
#define PAGE 0x20000u
#define AT(p) (BASE + (uint32_t)(p) * PAGE)

static virt_range_t s_r;

static void test_basic(void)
{
   virt_range_init(&s_r, BASE, SIZE, PAGE);
   CHECK(virt_range_alloc(&s_r, 1, 1) == AT(0));
   CHECK(virt_range_alloc(&s_r, PAGE + 1, 1) == AT(1)); /* two pages */
   CHECK(virt_range_alloc(&s_r, 0, 1) == AT(3));        /* still a page */
   CHECK(!virt_range_free(&s_r, AT(2)));                /* inside, not a start */
   CHECK(!virt_range_free(&s_r, AT(1) + 4));
   CHECK(!virt_range_free(&s_r, BASE - PAGE));
   CHECK(!virt_range_free(&s_r, AT(8192)));
   CHECK(!virt_range_free(&s_r, AT(9)));                /* nothing there */
   CHECK(virt_range_free(&s_r, AT(1)));
   CHECK(!virt_range_free(&s_r, AT(1)));                /* once */
   /* the lowest hole that fits */
   CHECK(virt_range_alloc(&s_r, 3 * PAGE, 1) == AT(4));
   CHECK(virt_range_alloc(&s_r, 2 * PAGE, 1) == AT(1));
   CHECK(virt_range_alloc(&s_r, PAGE, 1) == AT(7));
   /* aligned past a page */
   CHECK(virt_range_alloc(&s_r, PAGE, 16 * PAGE) == AT(16));
   CHECK(virt_range_alloc(&s_r, PAGE, 1) == AT(8));
}

static void test_words(void)
{
   uint32_t a;
   virt_range_init(&s_r, BASE, SIZE, PAGE);
   CHECK(virt_range_alloc(&s_r, 30 * PAGE, 1) == AT(0));
   a = virt_range_alloc(&s_r, 5 * PAGE, 1);             /* across the first two words */
   CHECK(a == AT(30));
   CHECK(virt_range_alloc(&s_r, 31 * PAGE, 1) == AT(35));
   CHECK(virt_range_free(&s_r, a));
   CHECK(virt_range_alloc(&s_r, 5 * PAGE, 1) == AT(30));
   /* full words passed over whole, and nothing past them missed */
   virt_range_init(&s_r, BASE, SIZE, PAGE);
   CHECK(virt_range_alloc(&s_r, 64 * PAGE, 1) == AT(0));
   CHECK(virt_range_alloc(&s_r, PAGE, 1) == AT(64));
   CHECK(virt_range_alloc(&s_r, 31 * PAGE, 1) == AT(65));
   CHECK(virt_range_alloc(&s_r, PAGE, 1) == AT(96));
   /* all of it, then nothing */
   virt_range_init(&s_r, BASE, SIZE, PAGE);
   CHECK(virt_range_alloc(&s_r, SIZE + 1, 1) == 0);
   CHECK(virt_range_alloc(&s_r, SIZE, 1) == BASE);
   CHECK(virt_range_alloc(&s_r, 1, 1) == 0);
   CHECK(virt_range_free(&s_r, BASE));
   CHECK(virt_range_alloc(&s_r, 0xFFFFFFFFu, 1) == 0);
   CHECK(virt_range_alloc(&s_r, SIZE - PAGE, 1) == BASE);
   CHECK(virt_range_alloc(&s_r, PAGE, 1) == AT(8191));
   CHECK(virt_range_alloc(&s_r, PAGE, 1) == 0);
}

/* another allocation takes part of the pages found, between finding and
 * taking them */
static uint32_t s_inner;
static uint32_t s_inner_size, s_inner_align;

static void alloc_inside(void)
{
   virt_range_test_mid_alloc = NULL;
   s_inner = virt_range_alloc(&s_r, s_inner_size, s_inner_align);
}

static void test_race(void)
{
   uint32_t a;
   /* in the first word: the outer one goes on past it */
   virt_range_init(&s_r, BASE, SIZE, PAGE);
   s_inner_size = PAGE;
   s_inner_align = 1;
   virt_range_test_mid_alloc = alloc_inside;
   a = virt_range_alloc(&s_r, 3 * PAGE, 1);
   virt_range_test_mid_alloc = NULL;
   CHECK(s_inner == AT(0));
   CHECK(a == AT(1));
   CHECK(virt_range_free(&s_r, a));
   CHECK(virt_range_alloc(&s_r, 3 * PAGE, 1) == AT(1));
   /* in the second word: the pages taken in the first are given back */
   virt_range_init(&s_r, BASE, SIZE, PAGE);
   CHECK(virt_range_alloc(&s_r, 30 * PAGE, 1) == AT(0));
   s_inner_size = PAGE;
   s_inner_align = 32 * PAGE;
   virt_range_test_mid_alloc = alloc_inside;
   a = virt_range_alloc(&s_r, 5 * PAGE, 1);
   virt_range_test_mid_alloc = NULL;
   CHECK(s_inner == AT(32));
   CHECK(a == AT(33));
   CHECK(virt_range_alloc(&s_r, 2 * PAGE, 1) == AT(30));
}

/* threads allocating and freeing at once: no page held twice */
#define THREADS 4
#define ROUNDS 20000
static retro_atomic_int_t s_owner[VIRT_RANGE_MAX_PAGES];
static retro_atomic_int_t s_overlaps;
static retro_atomic_int_t s_go;

static void worker(void *ud)
{
   int id = (int)(size_t)ud;
   unsigned seed = 1234u + (unsigned)id * 7919u;
   uint32_t held[8];
   uint32_t held_n[8];
   int n = 0, i;
   while (!retro_atomic_load_acquire_int(&s_go))
      sthread_yield();
   for (i = 0; i < ROUNDS; i++)
   {
      seed = seed * 1103515245u + 12345u;
      if (n < 8 && (n == 0 || (seed >> 16) & 1))
      {
         uint32_t pages = 1 + ((seed >> 20) % 8), a, p;
         a = virt_range_alloc(&s_r, pages * PAGE, 1);
         if (!a)
            continue;
         for (p = (a - BASE) / PAGE; p < (a - BASE) / PAGE + pages; p++)
            if (!retro_atomic_cas_int(&s_owner[p], 0, id))
               retro_atomic_fetch_add_int(&s_overlaps, 1);
         held[n] = a;
         held_n[n++] = pages;
      }
      else
      {
         uint32_t p, a = held[--n];
         for (p = (a - BASE) / PAGE; p < (a - BASE) / PAGE + held_n[n]; p++)
            if (!retro_atomic_cas_int(&s_owner[p], id, 0))
               retro_atomic_fetch_add_int(&s_overlaps, 1);
         if (!virt_range_free(&s_r, a))
            retro_atomic_fetch_add_int(&s_overlaps, 1);
      }
   }
   while (n > 0)
   {
      uint32_t p, a = held[--n];
      for (p = (a - BASE) / PAGE; p < (a - BASE) / PAGE + held_n[n]; p++)
         retro_atomic_cas_int(&s_owner[p], id, 0);
      virt_range_free(&s_r, a);
   }
}

static void test_threads(void)
{
   sthread_t *t[THREADS];
   int i;
   /* 64 pages: the threads keep meeting */
   virt_range_init(&s_r, BASE, 64 * PAGE, PAGE);
   for (i = 0; i < VIRT_RANGE_MAX_PAGES; i++)
      retro_atomic_int_init(&s_owner[i], 0);
   retro_atomic_int_init(&s_overlaps, 0);
   retro_atomic_int_init(&s_go, 0);
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)(i + 1));
   retro_atomic_store_release_int(&s_go, 1);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   CHECK(retro_atomic_load_acquire_int(&s_overlaps) == 0);
   /* all given back */
   CHECK(virt_range_alloc(&s_r, 64 * PAGE, 1) == BASE);
}

static void test_fsa_clients(void)
{
   fsa_clients_reset();
   CHECK(!fsa_clients_add(0));
   CHECK(!fsa_clients_add(0x1080));                     /* index 128 */
   CHECK(fsa_clients_add(0x1005));
   CHECK(fsa_clients_has(0x1005));
   CHECK(!fsa_clients_has(0x2005));                     /* same index, not this one */
   CHECK(!fsa_clients_has(0));
   CHECK(!fsa_clients_remove(0x2005));
   CHECK(fsa_clients_has(0x1005));
   CHECK(fsa_clients_remove(0x1005));
   CHECK(!fsa_clients_remove(0x1005));                  /* once */
   CHECK(!fsa_clients_has(0x1005));
   /* a closed handle's index given out again: only the new one is a client */
   CHECK(fsa_clients_add(0x1007));
   CHECK(fsa_clients_add(0x3007));
   CHECK(!fsa_clients_has(0x1007));
   CHECK(fsa_clients_has(0x3007));
   CHECK(!fsa_clients_remove(0x1007));
   CHECK(fsa_clients_has(0x3007));
   CHECK(fsa_clients_add(0x105F));
   fsa_clients_reset();
   CHECK(!fsa_clients_has(0x3007) && !fsa_clients_has(0x105F));
}

/* two threads deleting the same client: one takes it out */
static retro_atomic_int_t s_removed;
static uint32_t s_handle;

static void remover(void *ud)
{
   (void)ud;
   while (!retro_atomic_load_acquire_int(&s_go))
      sthread_yield();
   if (fsa_clients_remove(s_handle))
      retro_atomic_fetch_add_int(&s_removed, 1);
}

static void test_fsa_threads(void)
{
   int round;
   retro_atomic_int_init(&s_removed, 0);
   fsa_clients_reset();
   for (round = 0; round < 300; round++)
   {
      sthread_t *a, *b;
      s_handle = ((uint32_t)(round + 1) << 12) | (uint32_t)(round % 96);
      CHECK(fsa_clients_add(s_handle));
      retro_atomic_store_release_int(&s_go, 0);
      a = sthread_create(remover, NULL);
      b = sthread_create(remover, NULL);
      retro_atomic_store_release_int(&s_go, 1);
      sthread_join(a);
      sthread_join(b);
      CHECK(!fsa_clients_has(s_handle));
   }
   CHECK(retro_atomic_load_acquire_int(&s_removed) == 300);
}

int main(void)
{
   test_basic();
   test_words();
   test_race();
   test_threads();
   test_fsa_clients();
   test_fsa_threads();
   if (failures)
   {
      fprintf(stderr, "os_tables_test: %d failed\n", failures);
      return 1;
   }
   printf("os_tables_test: all passed\n");
   return 0;
}
