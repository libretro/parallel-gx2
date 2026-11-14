#ifndef COREINIT_VIRT_RANGE_H
#define COREINIT_VIRT_RANGE_H

/* The console's range of virtual addresses a title takes and gives back
 * (OSAllocVirtAddr, OSFreeVirtAddr), in pages, with no lock and nothing
 * allocated: a bit per page, claimed a word at a time with a
 * compare-and-swap, and the page count of each allocation kept at its
 * first page. The lowest run of free pages that fits is taken. C89. */

#include <stdint.h>

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define VIRT_RANGE_MAX_PAGES 8192

typedef struct virt_range
{
   retro_atomic_int_t used[VIRT_RANGE_MAX_PAGES / 32];
   retro_atomic_int_t run[VIRT_RANGE_MAX_PAGES]; /* pages from here, 0 if none start here */
   uint32_t base;
   uint32_t page_size;
   uint32_t pages;
} virt_range_t;

/* page_size a power of two; nothing taken. Not while others use it. */
void virt_range_init(virt_range_t *r, uint32_t base, uint32_t size, uint32_t page_size);
/* size rounded up to pages (at least one), aligned to align (a power of
 * two; under a page, a page); 0 when nothing fits */
uint32_t virt_range_alloc(virt_range_t *r, uint32_t size, uint32_t align);
/* false unless addr is where an allocation begins; freed once */
bool virt_range_free(virt_range_t *r, uint32_t addr);

RETRO_END_DECLS

#endif
