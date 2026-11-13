#ifndef CAFE_OS_RPL_RPL_SYMBOLS_H
#define CAFE_OS_RPL_RPL_SYMBOLS_H

/* The names of the console's addresses: each RPL's exports and imports as
 * it is loaded, a trampoline under the name of where it jumps, and a
 * graphic pack patch's symbols, for logs and crash reports. C89.
 *
 * No lock. Symbols at an address are a stack, newest first; adding one, or
 * an address, is a compare-and-swap onto the head of a list, and removing
 * one marks it, so a reader walking a list never meets one that is gone.
 * Nothing is freed until rpl_symbols_clear, which runs when nothing else
 * does (all RPLs unloaded): a symbol a reader holds stays valid until then.
 * Names are kept in blocks taken whole (128 KiB), a bump each. */

#include <stddef.h>
#include <stdint.h>

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* rpl_symbol_t.flags */
#define RPL_SYMBOL_MAP   1
#define RPL_SYMBOL_PATCH 2

typedef struct rpl_symbol
{
   uint32_t address;
   uint32_t flags;
   const char *lib;  /* one copy for each library, whatever its case */
   const char *name;
} rpl_symbol_t;

/* name at address, from lib; NULL only for no memory */
const rpl_symbol_t *rpl_symbols_store(const char *lib, const char *name,
      uint32_t address, uint32_t flags);
/* the newest symbol still at address; NULL for none */
const rpl_symbol_t *rpl_symbols_at(uint32_t address);
/* the newest at address or the nearest word below, up to 4096 words down */
const rpl_symbol_t *rpl_symbols_closest(uint32_t address);
/* every symbol in [address, address + length), a word at a time, whose
 * flags include all of flags (0: every one) */
void rpl_symbols_remove_range(uint32_t address, int32_t length, uint32_t flags);
/* jump given the name of what is at dest, if anything is */
void rpl_symbols_jump_proxy(uint32_t jump, uint32_t dest);
/* everything forgotten and freed: only while nothing else calls these */
void rpl_symbols_clear(void);

#ifdef RPL_SYMBOLS_TEST_HOOKS
/* the symbols still at address */
size_t rpl_symbols_count(uint32_t address);
#endif

RETRO_END_DECLS

#endif
