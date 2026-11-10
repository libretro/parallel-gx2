/* The core's lock-free cell (src/util/helpers/rcu_cell.c), under RcuCell.h,
 * in C89 as it is:
 *  - a value read is the value in place; a write puts the next in place and
 *    gives the old one back to be freed;
 *  - four readers reading while a writer replaces the value thousands of
 *    times never see one freed or half built (each value is poisoned as it
 *    is freed; AddressSanitizer by CI);
 *  - a reader that has taken its side and is not yet counted in when a
 *    writer moves on counts itself in on the new side: a second writer then
 *    waits for it, and the value it holds is not freed under it (a hook in
 *    the cell holds the reader at that moment);
 *  - writers take turns: two of them building each value from the one in
 *    place lose no write;
 *  - a reader inside a write's turn reads the value in place, as RcuCell's
 *    writers build the next from it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "util/helpers/rcu_cell.h"

static int failures = 0;
#define CHECK(cond) \
   do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define CANARY 0x600DF00DUL

struct value
{
   unsigned long canary;
   unsigned long n;
   unsigned long items[16];
};

static struct value *value_new(unsigned long n)
{
   struct value *v = (struct value*)malloc(sizeof(*v));
   int i;
   v->canary = CANARY;
   v->n = n;
   for (i = 0; i < 16; i++)
      v->items[i] = n;
   return v;
}

static void value_free(void *p)
{
   if (p)
      memset(p, 0xDD, sizeof(struct value)); /* poisoned: a late reader sees it */
   free(p);
}

static bool value_whole(const struct value *v)
{
   int i;
   if (v->canary != CANARY)
      return false;
   for (i = 0; i < 16; i++)
      if (v->items[i] != v->n)
         return false;
   return true;
}

static rcu_cell_t cell;

/* a while, given to the other threads */
static void pause_a_while(int yields)
{
   while (yields-- > 0)
      sthread_yield();
}

/* --- many readers, one writer -------------------------------------------- */

static retro_atomic_int_t stop;
static retro_atomic_int_t bad;

static void reader_loop(void *ud)
{
   (void)ud;
   while (!retro_atomic_load_acquire_int(&stop))
   {
      rcu_reader_t r;
      const struct value *v = (const struct value*)rcu_read_begin(&cell, &r);
      if (!value_whole(v))
         retro_atomic_fetch_add_int(&bad, 1);
      rcu_read_end(&r);
   }
}

static void test_readers(void)
{
   sthread_t *threads[4];
   unsigned long i;
   int t;
   retro_atomic_int_init(&stop, 0);
   retro_atomic_int_init(&bad, 0);
   for (t = 0; t < 4; t++)
      threads[t] = sthread_create(reader_loop, NULL);
   for (i = 1; i <= 5000; i++)
   {
      rcu_write_begin(&cell);
      value_free(rcu_write_publish(&cell, value_new(i)));
   }
   retro_atomic_store_release_int(&stop, 1);
   for (t = 0; t < 4; t++)
      sthread_join(threads[t]);
   CHECK(retro_atomic_load_acquire_int(&bad) == 0);
}

/* --- a reader held between taking its side and counting in -------------- */

static retro_atomic_int_t hold_armed;   /* the next reader to take its side waits */
static retro_atomic_int_t holding;      /* it is waiting there */
static retro_atomic_int_t let_go;       /* and may go on */
static retro_atomic_int_t has_value;    /* it read a value */
static retro_atomic_int_t second_done;  /* the second write is through */

void rcu_cell_test_side_taken(void)
{
   if (!retro_atomic_exchange_int(&hold_armed, 0))
      return;
   retro_atomic_store_release_int(&holding, 1);
   while (!retro_atomic_load_acquire_int(&let_go))
      sthread_yield();
}

static const struct value *held_value;

static void held_reader(void *ud)
{
   rcu_reader_t r;
   (void)ud;
   held_value = (const struct value*)rcu_read_begin(&cell, &r);
   retro_atomic_store_release_int(&has_value, 1);
   /* holds it while the second writer tries to free it */
   pause_a_while(20000);
   if (!value_whole(held_value) || held_value->n != 1001)
      retro_atomic_fetch_add_int(&bad, 1);
   if (retro_atomic_load_acquire_int(&second_done))
      retro_atomic_fetch_add_int(&bad, 1); /* freed while held */
   rcu_read_end(&r);
}

static void second_writer(void *ud)
{
   (void)ud;
   rcu_write_begin(&cell);
   value_free(rcu_write_publish(&cell, value_new(1002)));
   retro_atomic_store_release_int(&second_done, 1);
}

static void test_side_taken(void)
{
   sthread_t *reader, *writer;
   retro_atomic_int_init(&hold_armed, 1);
   retro_atomic_int_init(&holding, 0);
   retro_atomic_int_init(&let_go, 0);
   retro_atomic_int_init(&has_value, 0);
   retro_atomic_int_init(&second_done, 0);
   retro_atomic_int_init(&bad, 0);
   reader = sthread_create(held_reader, NULL);
   while (!retro_atomic_load_acquire_int(&holding))
      sthread_yield();
   /* the first writer moves on while the reader has its side but is not
    * counted in: nothing to wait for */
   rcu_write_begin(&cell);
   value_free(rcu_write_publish(&cell, value_new(1001)));
   retro_atomic_store_release_int(&let_go, 1);
   while (!retro_atomic_load_acquire_int(&has_value))
      sthread_yield();
   /* the second waits for the reader holding 1001 */
   writer = sthread_create(second_writer, NULL);
   sthread_join(reader);
   sthread_join(writer);
   CHECK(retro_atomic_load_acquire_int(&bad) == 0);
   CHECK(retro_atomic_load_acquire_int(&second_done) == 1);
}

/* --- writers take turns --------------------------------------------------- */

static void counting_writer(void *ud)
{
   int i;
   (void)ud;
   for (i = 0; i < 2000; i++)
   {
      const struct value *cur;
      rcu_write_begin(&cell);
      cur = (const struct value*)rcu_write_current(&cell);
      value_free(rcu_write_publish(&cell, value_new(cur->n + 1)));
   }
}

static void test_writers(void)
{
   sthread_t *a, *b;
   rcu_reader_t r;
   unsigned long start;
   rcu_read_begin(&cell, &r);
   start = ((const struct value*)r.ptr)->n;
   rcu_read_end(&r);
   a = sthread_create(counting_writer, NULL);
   b = sthread_create(counting_writer, NULL);
   sthread_join(a);
   sthread_join(b);
   rcu_read_begin(&cell, &r);
   CHECK(((const struct value*)r.ptr)->n == start + 4000);
   rcu_read_end(&r);
}

int main(void)
{
   rcu_reader_t r;
   const struct value *v;
   struct value *first = value_new(7);
   retro_atomic_int_init(&hold_armed, 0);
   CHECK(rcu_cell_init(&cell, first));
   v = (const struct value*)rcu_read_begin(&cell, &r);
   CHECK(v == first && v->n == 7);
   rcu_read_end(&r);
   rcu_write_begin(&cell);
   CHECK(rcu_write_current(&cell) == first);
   /* a reader inside the turn reads the value in place */
   v = (const struct value*)rcu_read_begin(&cell, &r);
   CHECK(v == first);
   rcu_read_end(&r);
   CHECK(rcu_write_publish(&cell, value_new(8)) == first);
   value_free(first);
   v = (const struct value*)rcu_read_begin(&cell, &r);
   CHECK(v->n == 8);
   rcu_read_end(&r);

   test_readers();
   test_side_taken();
   test_writers();

   value_free(rcu_cell_deinit(&cell));
   if (failures)
   {
      fprintf(stderr, "rcu_cell: %d check(s) failed\n", failures);
      return 1;
   }
   printf("rcu_cell: all checks passed\n");
   return 0;
}
