/* coreinit's small claims with no lock (os_claims.h) */

#include "os_claims.h"

int os_claim_first(retro_atomic_int_t *flags, int count)
{
   int i;
   for (i = 0; i < count; i++)
   {
      if (retro_atomic_load_acquire_int(&flags[i]) == 0
            && retro_atomic_cas_int(&flags[i], 0, 1))
         return i;
   }
   return -1;
}

bool os_claim_release(retro_atomic_int_t *flags, int count, int i)
{
   if (i < 0 || i >= count)
      return false;
   return retro_atomic_cas_int(&flags[i], 1, 0);
}

uint32_t os_bump_alloc(retro_atomic_int_t *offset, uint32_t size, uint32_t align, uint32_t limit)
{
   const uint64_t len = ((uint64_t)size + 3) & ~(uint64_t)3;
   if (align < 2)
      align = 1;
   for (;;)
   {
      const int cur    = retro_atomic_load_acquire_int(offset);
      uint64_t start   = (uint64_t)(uint32_t)cur + align - 1;
      uint64_t end;
      start -= start % align;
      end    = start + len;
      if (end >= limit)
         return 0xFFFFFFFFu;
      if (retro_atomic_cas_int(offset, cur, (int)(uint32_t)end))
         return (uint32_t)start;
   }
}
