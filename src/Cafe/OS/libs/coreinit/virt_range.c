/* The console's virtual address range, by page, with no lock (virt_range.h) */

#include "virt_range.h"

#ifdef VIRT_RANGE_TEST_HOOKS
/* in an allocation, between finding free pages and taking them */
void (*virt_range_test_mid_alloc)(void);
#define MID_ALLOC() do { if (virt_range_test_mid_alloc) virt_range_test_mid_alloc(); } while (0)
#else
#define MID_ALLOC() do { } while (0)
#endif

void virt_range_init(virt_range_t *r, uint32_t base, uint32_t size, uint32_t page_size)
{
   uint32_t i;
   r->base = base;
   r->page_size = page_size;
   r->pages = size / page_size;
   if (r->pages > VIRT_RANGE_MAX_PAGES)
      r->pages = VIRT_RANGE_MAX_PAGES;
   for (i = 0; i < VIRT_RANGE_MAX_PAGES / 32; i++)
      retro_atomic_int_init(&r->used[i], 0);
   for (i = 0; i < VIRT_RANGE_MAX_PAGES; i++)
      retro_atomic_int_init(&r->run[i], 0);
}

/* the bits of pages [first, end) in word w */
static uint32_t word_mask(uint32_t first, uint32_t end, uint32_t w)
{
   uint32_t lo = w * 32;
   uint32_t a  = first > lo ? first - lo : 0;
   uint32_t b  = end < lo + 32 ? end - lo : 32;
   if (a >= b)
      return 0;
   if (b - a == 32)
      return 0xFFFFFFFFu;
   return (((uint32_t)1 << (b - a)) - 1) << a;
}

static bool page_used(virt_range_t *r, uint32_t p)
{
   return (((uint32_t)retro_atomic_load_acquire_int(&r->used[p >> 5]) >> (p & 31)) & 1) != 0;
}

static void clear_pages(virt_range_t *r, uint32_t first, uint32_t end)
{
   uint32_t w;
   for (w = first >> 5; w <= (end - 1) >> 5; w++)
      retro_atomic_fetch_and_int(&r->used[w], (int)~word_mask(first, end, w));
}

/* all of [first, end) taken, or none of it and false */
static bool claim(virt_range_t *r, uint32_t first, uint32_t end)
{
   uint32_t w;
   for (w = first >> 5; w <= (end - 1) >> 5; w++)
   {
      const uint32_t m = word_mask(first, end, w);
      for (;;)
      {
         int v = retro_atomic_load_acquire_int(&r->used[w]);
         if ((uint32_t)v & m)
         {
            /* another took some meanwhile: give back the words before */
            if (w > (first >> 5))
               clear_pages(r, first, w << 5);
            return false;
         }
         if (retro_atomic_cas_int(&r->used[w], v, (int)((uint32_t)v | m)))
            break;
      }
   }
   return true;
}

uint32_t virt_range_alloc(virt_range_t *r, uint32_t size, uint32_t align)
{
   uint32_t n    = (uint32_t)(((uint64_t)size + r->page_size - 1) / r->page_size);
   uint32_t step = align > r->page_size ? align / r->page_size : 1;
   uint32_t s    = 0;
   if (!n)
      n = 1;
   if (n > r->pages)
      return 0;
   for (;;)
   {
      uint32_t p;
      bool busy = false;
      s = (s + step - 1) / step * step;
      if (s > r->pages - n)
         return 0;
      for (p = s; p < s + n; p++)
      {
         /* a full word: past it at once */
         if (!(p & 31) && retro_atomic_load_acquire_int(&r->used[p >> 5]) == -1)
         {
            p += 31;
            busy = true;
            break;
         }
         if (page_used(r, p))
         {
            busy = true;
            break;
         }
      }
      if (busy)
      {
         s = p + 1;
         continue;
      }
      MID_ALLOC();
      if (!claim(r, s, s + n))
         continue; /* look again from here */
      retro_atomic_store_release_int(&r->run[s], (int)n);
      return r->base + s * r->page_size;
   }
}

bool virt_range_free(virt_range_t *r, uint32_t addr)
{
   uint32_t s;
   int n;
   if (addr < r->base || (addr - r->base) % r->page_size)
      return false;
   s = (addr - r->base) / r->page_size;
   if (s >= r->pages)
      return false;
   n = retro_atomic_load_acquire_int(&r->run[s]);
   if (n <= 0 || !retro_atomic_cas_int(&r->run[s], n, 0))
      return false;
   clear_pages(r, s, s + (uint32_t)n);
   return true;
}
