/* The fixed map behind the Vulkan sampler cache
 * (src/util/helpers/u64_map.c): against a plain list of what should be in
 * it, through 400000 random puts, gets and takes over keys that collide and
 * wrap round the table's end - every key found with its value, none found
 * that was taken, the count right - and full at three quarters, an
 * iteration that visits each entry once, a clear. C89, as the C89 lane
 * builds it. */

#include <stdio.h>
#include <string.h>

#include "util/helpers/u64_map.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define CAP 256
#define KEYS 400

static u64_map_slot_t s_slots[CAP];
static uint64_t s_keys[KEYS];
static char s_vals[KEYS];      /* a value's address: &s_vals[i] */
static int s_in[KEYS];         /* the oracle: which value i the key holds, -1 none */

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
   s_rng = s_rng * 1103515245u + 12345u;
   return s_rng >> 8;
}

static void check_all(const u64_map_t *m)
{
   int i, n = 0, bad = 0;
   for (i = 0; i < KEYS; i++)
   {
      void *v = u64_map_get(m, s_keys[i]);
      if (s_in[i] >= 0)
      {
         n++;
         if (v != &s_vals[s_in[i]])
            bad++;
      }
      else if (v)
         bad++;
   }
   CHECK(bad == 0);
   CHECK((int)m->count == n);
}

static void test_random(void)
{
   u64_map_t m;
   int i, op;
   memset(s_slots, 0, sizeof(s_slots));
   u64_map_init(&m, s_slots, CAP);
   /* keys of a few kinds: spread, sharing low bits, and adjacent */
   for (i = 0; i < KEYS; i++)
   {
      if (i % 3 == 0)
         s_keys[i] = ((uint64_t)rnd() << 32) ^ rnd();
      else if (i % 3 == 1)
         s_keys[i] = (uint64_t)i << 40;
      else
         s_keys[i] = (uint64_t)i;
      s_in[i] = -1;
   }
   s_keys[5] = 0; /* zero is a key like any other */
   for (op = 0; op < 400000; op++)
   {
      const int k = (int)(rnd() % KEYS);
      const uint32_t r = rnd() % 10;
      if (r < 5)
      {
         const int v = (int)(rnd() % KEYS);
         const bool ok = u64_map_put(&m, s_keys[k], &s_vals[v]);
         if (s_in[k] >= 0)
            CHECK(ok); /* in place: never refused */
         if (ok)
            s_in[k] = v;
         else
            CHECK((m.count + 1) * 4 > (size_t)CAP * 3);
      }
      else if (r < 8)
      {
         void *v = u64_map_take(&m, s_keys[k]);
         CHECK(v == (s_in[k] >= 0 ? &s_vals[s_in[k]] : NULL));
         s_in[k] = -1;
      }
      else
      {
         void *v = u64_map_get(&m, s_keys[k]);
         CHECK(v == (s_in[k] >= 0 ? &s_vals[s_in[k]] : NULL));
      }
      if (op % 4999 == 0)
         check_all(&m);
      if (failures)
         break;
   }
   check_all(&m);
}

static void test_full_iterate_clear(void)
{
   u64_map_t m;
   size_t cursor = 0;
   uint64_t key;
   int seen[KEYS], i, n = 0;
   memset(s_slots, 0, sizeof(s_slots));
   u64_map_init(&m, s_slots, CAP);
   for (i = 0; i < CAP * 3 / 4; i++)
      CHECK(u64_map_put(&m, (uint64_t)i * 7919, &s_vals[i % KEYS]));
   CHECK(!u64_map_put(&m, 999999999, &s_vals[0]));
   CHECK(u64_map_put(&m, 7919, &s_vals[3])); /* in place, full or not */
   CHECK(!u64_map_put(&m, 1, NULL));
   memset(seen, 0, sizeof(seen));
   while (u64_map_next(&m, &cursor, &key))
   {
      if (key % 7919 == 0 && key / 7919 < KEYS)
         seen[key / 7919]++;
      n++;
   }
   CHECK(n == CAP * 3 / 4);
   for (i = 0; i < CAP * 3 / 4; i++)
      if (seen[i] != 1)
         failures++;
   u64_map_clear(&m);
   CHECK(m.count == 0 && !u64_map_get(&m, 7919));
   cursor = 0;
   CHECK(!u64_map_next(&m, &cursor, &key));
   /* no NULL value: it would mark the slot free */
   CHECK(!u64_map_put(&m, 1, NULL) && m.count == 0 && !u64_map_get(&m, 1));
   CHECK(u64_map_put(&m, 5, &s_vals[1]) && u64_map_get(&m, 5) == &s_vals[1]);
}

int main(void)
{
   test_random();
   test_full_iterate_clear();
   if (failures)
   {
      fprintf(stderr, "u64_map_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("u64_map_test: all passed\n");
   return 0;
}
