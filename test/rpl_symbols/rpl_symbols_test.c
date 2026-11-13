/* The names of the console's addresses (src/Cafe/OS/RPL/rpl_symbols.c):
 * the newest symbol at an address, one library name for every case it is
 * given in, ranges removed by kind, the nearest symbol below, jump proxies,
 * names longer than a block, and - under TSan - threads storing, looking
 * up and removing at once with no lock, a symbol a reader holds valid until
 * everything is cleared, built a second time with 4 buckets so that every
 * thread adds to the same lists. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Cafe/OS/RPL/rpl_symbols.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void test_basic(void)
{
   const rpl_symbol_t *a, *b, *c;
   rpl_symbols_clear();
   CHECK(rpl_symbols_at(0x02000000) == NULL);
   a = rpl_symbols_store("coreinit", "OSReport", 0x02000000, 0);
   CHECK(a && a->address == 0x02000000 && strcmp(a->name, "OSReport") == 0 && strcmp(a->lib, "coreinit") == 0);
   CHECK(rpl_symbols_at(0x02000000) == a);
   /* the newest first, the one under it back when it goes */
   b = rpl_symbols_store("CoreInit", "OSFatal", 0x02000000, RPL_SYMBOL_PATCH);
   CHECK(rpl_symbols_at(0x02000000) == b);
   /* one copy of a library's name, whatever its case */
   CHECK(b->lib == a->lib);
   rpl_symbols_remove_range(0x02000000, 4, RPL_SYMBOL_PATCH);
   CHECK(rpl_symbols_at(0x02000000) == a);
   /* a symbol of another kind stays */
   rpl_symbols_remove_range(0x02000000, 4, RPL_SYMBOL_PATCH);
   CHECK(rpl_symbols_at(0x02000000) == a);
   /* kind 0: every one */
   rpl_symbols_remove_range(0x02000000, 4, 0);
   CHECK(rpl_symbols_at(0x02000000) == NULL);

   /* a range, a word at a time; what is past its end stays */
   rpl_symbols_store("gp", "p0", 0x03000000, RPL_SYMBOL_PATCH);
   rpl_symbols_store("gp", "p1", 0x03000004, RPL_SYMBOL_PATCH);
   c = rpl_symbols_store("gp", "p2", 0x03000008, RPL_SYMBOL_PATCH);
   rpl_symbols_remove_range(0x03000000, 8, RPL_SYMBOL_PATCH);
   CHECK(!rpl_symbols_at(0x03000000) && !rpl_symbols_at(0x03000004) && rpl_symbols_at(0x03000008) == c);
   rpl_symbols_remove_range(0x03000008, 0, 0);
   CHECK(rpl_symbols_at(0x03000008) == c);
   /* removed, it still reads as it was (nothing is freed until clear) */
   rpl_symbols_remove_range(0x03000008, 1, 0);
   CHECK(!rpl_symbols_at(0x03000008) && strcmp(c->name, "p2") == 0);
}

static void test_closest(void)
{
   const rpl_symbol_t *f;
   rpl_symbols_clear();
   f = rpl_symbols_store("rpx", "main", 0x02100000, 0);
   CHECK(rpl_symbols_closest(0x02100000) == f);
   CHECK(rpl_symbols_closest(0x02100040) == f);
   /* 4095 words up is the farthest still found */
   CHECK(rpl_symbols_closest(0x02100000 + 4095 * 4) == f);
   CHECK(rpl_symbols_closest(0x02100000 + 4096 * 4) == NULL);
   CHECK(rpl_symbols_closest(0x020FFFFC) == NULL);
   /* a removed one is passed over for the next below */
   rpl_symbols_store("rpx", "inner", 0x02100020, RPL_SYMBOL_PATCH);
   CHECK(strcmp(rpl_symbols_closest(0x02100024)->name, "inner") == 0);
   rpl_symbols_remove_range(0x02100020, 4, RPL_SYMBOL_PATCH);
   CHECK(rpl_symbols_closest(0x02100024) == f);
   /* jump proxies take the name of where they lead */
   rpl_symbols_jump_proxy(0x01800000, 0x02100000);
   CHECK(rpl_symbols_at(0x01800000) && strcmp(rpl_symbols_at(0x01800000)->name, "main") == 0);
   rpl_symbols_jump_proxy(0x01800004, 0x02100004);
   CHECK(rpl_symbols_at(0x01800004) == NULL);
}

static void test_long_names(void)
{
   /* longer than a block, and many that fill several */
   static char big[300 * 1024];
   char name[32];
   unsigned i;
   rpl_symbols_clear();
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = 0;
   CHECK(rpl_symbols_store("lib", big, 0x10, 0) != NULL);
   for (i = 0; i < 20000; i++)
   {
      sprintf(name, "sym_%u", i);
      CHECK(rpl_symbols_store("lib", name, 0x20000000u + i * 4, 0) != NULL);
   }
   CHECK(strlen(rpl_symbols_at(0x10)->name) == sizeof(big) - 1);
   for (i = 0; i < 20000; i += 997)
   {
      sprintf(name, "sym_%u", i);
      CHECK(strcmp(rpl_symbols_at(0x20000000u + i * 4)->name, name) == 0);
   }
   rpl_symbols_clear();
   CHECK(rpl_symbols_at(0x10) == NULL && rpl_symbols_at(0x20000000) == NULL);
}

#define THREADS 6
#define PER 4000

static retro_atomic_int_t s_go;
static retro_atomic_int_t s_bad;

static void worker(void *ud)
{
   const unsigned id = (unsigned)(size_t)ud;
   char name[32], lib[16];
   unsigned i;
   while (!retro_atomic_load_acquire_int(&s_go))
      ;
   for (i = 0; i < PER; i++)
   {
      const uint32_t own = 0x30000000u + (id * PER + i) * 4;
      /* every thread at the same few addresses too */
      const uint32_t shared = 0x40000000u + (i % 64) * 4;
      const rpl_symbol_t *s;
      sprintf(name, "t%u_%u", id, i);
      sprintf(lib, (i & 1) ? "LIB%u" : "lib%u", i % 8);
      s = rpl_symbols_store(lib, name, own, (i % 3) ? 0 : RPL_SYMBOL_PATCH);
      rpl_symbols_store(lib, name, shared, RPL_SYMBOL_PATCH);
      /* and at ones nothing removes: every one of them kept */
      rpl_symbols_store(lib, name, 0x50000000u + (i % 16) * 4, 0);
      if (!s || rpl_symbols_at(own) != s)
         retro_atomic_fetch_add_int(&s_bad, 1);
      /* someone else's, maybe removed meanwhile: whatever comes back is whole */
      s = rpl_symbols_closest(shared + 8);
      if (s && (strncmp(s->name, "t", 1) != 0 || s->address > shared + 8))
         retro_atomic_fetch_add_int(&s_bad, 1);
      if ((i % 50) == 0)
         rpl_symbols_remove_range(0x40000000u, 64 * 4, RPL_SYMBOL_PATCH);
   }
}

static void test_threads(void)
{
   sthread_t *t[THREADS];
   unsigned i, id, patch_left = 0, found = 0;
   const char *libs[8];
   rpl_symbols_clear();
   retro_atomic_int_init(&s_go, 0);
   retro_atomic_int_init(&s_bad, 0);
   for (i = 0; i < THREADS; i++)
      t[i] = sthread_create(worker, (void*)(size_t)i);
   retro_atomic_store_release_int(&s_go, 1);
   for (i = 0; i < THREADS; i++)
      sthread_join(t[i]);
   CHECK(retro_atomic_load_acquire_int(&s_bad) == 0);
   /* every thread's own symbols there, and one copy of each library */
   memset((void*)libs, 0, sizeof(libs));
   for (id = 0; id < THREADS; id++)
      for (i = 0; i < PER; i++)
      {
         const rpl_symbol_t *s = rpl_symbols_at(0x30000000u + (id * PER + i) * 4);
         char name[32];
         sprintf(name, "t%u_%u", id, i);
         if (s && strcmp(s->name, name) == 0)
            found++;
         if (s)
         {
            if (!libs[i % 8])
               libs[i % 8] = s->lib;
            else if (libs[i % 8] != s->lib)
               failures++;
         }
      }
   CHECK(found == THREADS * PER);
   rpl_symbols_remove_range(0x40000000u, 64 * 4, RPL_SYMBOL_PATCH);
   for (i = 0; i < 64; i++)
      if (rpl_symbols_at(0x40000000u + i * 4))
         patch_left++;
   CHECK(patch_left == 0);
   for (i = 0, found = 0; i < 16; i++)
      found += (unsigned)rpl_symbols_count(0x50000000u + i * 4);
   CHECK(found == THREADS * PER);
   rpl_symbols_clear();
}

int main(void)
{
   test_basic();
   test_closest();
   test_long_names();
   test_threads();
   rpl_symbols_clear();
   if (failures)
   {
      fprintf(stderr, "rpl_symbols_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("rpl_symbols_test: all passed\n");
   return 0;
}
