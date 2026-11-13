/* The renderer's object pool (src/util/helpers/obj_pool.c): blocks handed
 * out once, 16-aligned and whole, taken back and handed out again rather
 * than made anew, chunks made as needed up to the most there may be and
 * single blocks past them, nothing freed while one is out - and, under TSan
 * and ASan, threads taking and giving back at once, with no lock. C89, as
 * the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/obj_pool.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define PER 4
#define SIZE 40
#define ALL (PER * OBJ_POOL_MAX_CHUNKS)
static unsigned char s_ring[OBJ_POOL_RING_SIZE(PER)];

static void test_single(void)
{
   obj_pool_t p;
   void *b[ALL + 8];
   int i, j;
   memset(s_ring, 0, sizeof(s_ring));
   obj_pool_init(&p, s_ring, SIZE, PER);
   CHECK(obj_pool_out(&p) == 0);
   CHECK(obj_pool_reserve(&p, 1));
   CHECK(retro_atomic_load_acquire_int(&p.claimed) == 1 && obj_pool_out(&p) == 0);
   /* a chunk's worth: no new chunk */
   for (i = 0; i < PER; i++)
      CHECK((b[i] = obj_pool_get(&p)) != NULL);
   CHECK(retro_atomic_load_acquire_int(&p.claimed) == 1);
   /* given back, handed out again: still none */
   for (i = 0; i < PER; i++)
      obj_pool_put(&p, b[i]);
   CHECK(obj_pool_out(&p) == 0);
   for (i = 0; i < PER; i++)
      CHECK((b[i] = obj_pool_get(&p)) != NULL);
   CHECK(retro_atomic_load_acquire_int(&p.claimed) == 1);
   /* every chunk, then single blocks */
   for (i = PER; i < ALL + 8; i++)
      CHECK((b[i] = obj_pool_get(&p)) != NULL);
   CHECK(retro_atomic_load_acquire_int(&p.claimed) == OBJ_POOL_MAX_CHUNKS);
   CHECK(retro_atomic_load_acquire_int(&p.loose) == 8);
   CHECK(obj_pool_out(&p) == ALL + 8);
   /* each its own, 16-aligned, whole */
   for (i = 0; i < ALL + 8; i++)
   {
      CHECK(((uintptr_t)b[i] & 15) == 0);
      memset(b[i], i & 0xff, SIZE);
   }
   for (i = 0; i < ALL + 8; i++)
   {
      unsigned char *c = (unsigned char*)b[i];
      for (j = 0; j < SIZE; j++)
         if (c[j] != (unsigned char)(i & 0xff))
         {
            failures++;
            break;
         }
   }
   /* nothing freed while one is out */
   CHECK(!obj_pool_free_all(&p));
   for (i = 0; i < ALL + 8; i++)
      obj_pool_put(&p, b[i]);
   CHECK(retro_atomic_load_acquire_int(&p.loose) == 0 && obj_pool_out(&p) == 0);
   /* all handed out again from the chunks, none on its own */
   for (i = 0; i < ALL; i++)
      CHECK((b[i] = obj_pool_get(&p)) != NULL);
   CHECK(retro_atomic_load_acquire_int(&p.loose) == 0);
   for (i = 0; i < ALL; i++)
      obj_pool_put(&p, b[i]);
   obj_pool_put(&p, NULL);
   CHECK(obj_pool_free_all(&p));
   /* and usable again after */
   CHECK((b[0] = obj_pool_get(&p)) != NULL);
   obj_pool_put(&p, b[0]);
   CHECK(obj_pool_free_all(&p));
}

/* threads at once: no block held by two */
#define THREADS 4
#define ROUNDS 20000
static unsigned char s_ring2[OBJ_POOL_RING_SIZE(16)];
static obj_pool_t s_pool;
static retro_atomic_int_t s_bad;

static void worker(void *ud)
{
   const unsigned id = (unsigned)(size_t)ud;
   void *held[8];
   int r, i;
   for (r = 0; r < ROUNDS; r++)
   {
      const int n = 1 + (r % 8);
      for (i = 0; i < n; i++)
      {
         held[i] = obj_pool_get(&s_pool);
         memset(held[i], (int)id, 64);
      }
      for (i = 0; i < n; i++)
      {
         const unsigned char *c = (const unsigned char*)held[i];
         if (c[0] != id || c[63] != id)
            retro_atomic_fetch_add_int(&s_bad, 1);
         obj_pool_put(&s_pool, held[i]);
      }
   }
}

static void test_threads(void)
{
   sthread_t *t[THREADS];
   int i;
   memset(s_ring2, 0, sizeof(s_ring2));
   obj_pool_init(&s_pool, s_ring2, 64, 16);
   retro_atomic_int_init(&s_bad, 0);
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)(i + 1));
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   CHECK(retro_atomic_load_acquire_int(&s_bad) == 0);
   CHECK(obj_pool_out(&s_pool) == 0);
   /* 32 held at most at once: never past the chunks */
   CHECK(retro_atomic_load_acquire_int(&s_pool.loose) == 0);
   CHECK(obj_pool_free_all(&s_pool));
}

int main(void)
{
   test_single();
   test_threads();
   if (failures)
   {
      fprintf(stderr, "obj_pool_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("obj_pool_test: all passed\n");
   return 0;
}
