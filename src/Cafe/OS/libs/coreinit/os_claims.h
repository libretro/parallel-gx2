#ifndef COREINIT_OS_CLAIMS_H
#define COREINIT_OS_CLAIMS_H

/* Small claims coreinit makes from any thread with no lock: the first free
 * of a row of flags (GHS's file locks), and space bumped off a fixed area
 * (the system area). C89. */

#include <stdint.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* the lowest flag of count that was 0, now 1; -1 when none is */
int os_claim_first(retro_atomic_int_t *flags, int count);
/* flag i back to 0; false if it was not taken */
bool os_claim_release(retro_atomic_int_t *flags, int count, int i);

/* size (rounded up to 4) at the next multiple of align (0 or 1 for none)
 * from *offset, when it ends before limit: its offset, and *offset moved
 * past it. Otherwise 0xFFFFFFFF, and *offset unchanged. */
uint32_t os_bump_alloc(retro_atomic_int_t *offset, uint32_t size, uint32_t align, uint32_t limit);

RETRO_END_DECLS

#endif
