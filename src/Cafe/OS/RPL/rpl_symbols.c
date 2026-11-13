/* The names of the console's addresses (rpl_symbols.h) */

#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>

#include "rpl_symbols.h"

#ifndef RPL_SYMBOLS_BUCKETS
#define RPL_SYMBOLS_BUCKETS 16384 /* a power of two; tests make it small */
#endif
#define BUCKETS RPL_SYMBOLS_BUCKETS
#define BLOCK_SIZE (128 * 1024)
#define ALIGN 8
#define CLOSEST_WORDS 4096

typedef struct sym_node
{
   rpl_symbol_t pub;            /* first: what callers are handed */
   struct sym_node *previous;   /* set before it is published, then fixed */
   retro_atomic_int_t removed;
} sym_node_t;

typedef struct addr_node
{
   uint32_t address;
   struct addr_node *next;      /* fixed once published */
   retro_atomic_ptr_t head;     /* sym_node_t*, newest first */
} addr_node_t;

typedef struct lib_node
{
   const char *name;
   struct lib_node *next;       /* fixed once published */
} lib_node_t;

typedef struct block
{
   struct block *next;          /* the block before it: every block, chained */
   size_t size;
   retro_atomic_size_t used;
} block_t;

static retro_atomic_ptr_t s_buckets[BUCKETS];
static retro_atomic_ptr_t s_libs;
static retro_atomic_ptr_t s_block;

static size_t block_header(void)
{
   return (sizeof(block_t) + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

/* size octets, aligned, from the current block or a new one */
static void *arena_alloc(size_t size)
{
   size = (size + ALIGN - 1) & ~(size_t)(ALIGN - 1);
   for (;;)
   {
      block_t *b = (block_t*)retro_atomic_load_acquire_ptr(&s_block);
      block_t *nb;
      size_t cap;
      if (b)
      {
         const size_t off = retro_atomic_fetch_add_size(&b->used, size);
         if (off <= b->size && size <= b->size - off)
            return (char*)b + block_header() + off;
      }
      /* full (or none yet): a new one, which only one thread puts in */
      cap = size > BLOCK_SIZE ? size : BLOCK_SIZE;
      nb = (block_t*)malloc(block_header() + cap);
      if (!nb)
         return NULL;
      nb->next = b;
      nb->size = cap;
      retro_atomic_size_init(&nb->used, size);
      if (retro_atomic_cas_ptr(&s_block, b, nb))
         return (char*)nb + block_header();
      free(nb);
   }
}

static char *arena_strdup(const char *s)
{
   const size_t len = strlen(s) + 1;
   char *d = (char*)arena_alloc(len);
   if (d)
      memcpy(d, s, len);
   return d;
}

static int ascii_lower(int c)
{
   return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int iequals(const char *a, const char *b)
{
   while (*a && ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b))
   {
      a++;
      b++;
   }
   return ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b);
}

static const char *find_lib(lib_node_t *from, lib_node_t *to, const char *lib)
{
   for (; from && from != to; from = from->next)
      if (iequals(from->name, lib))
         return from->name;
   return NULL;
}

/* one copy of each library's name, matched without regard to case */
static const char *intern_lib(const char *lib)
{
   lib_node_t *head = (lib_node_t*)retro_atomic_load_acquire_ptr(&s_libs);
   lib_node_t *node;
   const char *found = find_lib(head, NULL, lib);
   if (found)
      return found;
   node = (lib_node_t*)arena_alloc(sizeof(*node));
   if (!node || !(node->name = arena_strdup(lib)))
      return NULL;
   for (;;)
   {
      node->next = head;
      if (retro_atomic_cas_ptr(&s_libs, head, node))
         return node->name;
      /* someone else added libraries: one may be this one */
      {
         lib_node_t *now = (lib_node_t*)retro_atomic_load_acquire_ptr(&s_libs);
         found = find_lib(now, head, lib);
         if (found)
            return found; /* the copy stays in the arena, unused */
         head = now;
      }
   }
}

static retro_atomic_ptr_t *bucket(uint32_t address)
{
   /* words: the low two bits say nothing */
   uint32_t h = (address >> 2) * 2654435761u;
   return &s_buckets[(h >> 18) % BUCKETS];
}

static addr_node_t *find_addr(addr_node_t *from, addr_node_t *to, uint32_t address)
{
   for (; from && from != to; from = from->next)
      if (from->address == address)
         return from;
   return NULL;
}

static addr_node_t *lookup(uint32_t address)
{
   return find_addr((addr_node_t*)retro_atomic_load_acquire_ptr(bucket(address)), NULL, address);
}

static addr_node_t *lookup_or_add(uint32_t address)
{
   retro_atomic_ptr_t *b = bucket(address);
   addr_node_t *head = (addr_node_t*)retro_atomic_load_acquire_ptr(b);
   addr_node_t *node = find_addr(head, NULL, address);
   if (node)
      return node;
   node = (addr_node_t*)arena_alloc(sizeof(*node));
   if (!node)
      return NULL;
   node->address = address;
   retro_atomic_ptr_init(&node->head, NULL);
   for (;;)
   {
      addr_node_t *found;
      node->next = head;
      if (retro_atomic_cas_ptr(b, head, node))
         return node;
      {
         addr_node_t *now = (addr_node_t*)retro_atomic_load_acquire_ptr(b);
         found = find_addr(now, head, address);
         if (found)
            return found;
         head = now;
      }
   }
}

const rpl_symbol_t *rpl_symbols_store(const char *lib, const char *name,
      uint32_t address, uint32_t flags)
{
   addr_node_t *a = lookup_or_add(address);
   sym_node_t *s = (sym_node_t*)arena_alloc(sizeof(*s));
   const char *l = intern_lib(lib);
   char *n = arena_strdup(name);
   void *head;
   if (!a || !s || !l || !n)
      return NULL;
   s->pub.address = address;
   s->pub.flags = flags;
   s->pub.lib = l;
   s->pub.name = n;
   retro_atomic_int_init(&s->removed, 0);
   head = retro_atomic_load_acquire_ptr(&a->head);
   for (;;)
   {
      s->previous = (sym_node_t*)head;
      if (retro_atomic_cas_ptr(&a->head, head, s))
         return &s->pub;
      head = retro_atomic_load_acquire_ptr(&a->head);
   }
}

static const rpl_symbol_t *live(const addr_node_t *a)
{
   const sym_node_t *s;
   if (!a)
      return NULL;
   for (s = (const sym_node_t*)retro_atomic_load_acquire_ptr(
            (retro_atomic_ptr_t*)&a->head); s; s = s->previous)
      if (!retro_atomic_load_acquire_int((retro_atomic_int_t*)&s->removed))
         return &s->pub;
   return NULL;
}

const rpl_symbol_t *rpl_symbols_at(uint32_t address)
{
   return live(lookup(address));
}

const rpl_symbol_t *rpl_symbols_closest(uint32_t address)
{
   unsigned i;
   for (i = 0; i < CLOSEST_WORDS; i++, address -= 4)
   {
      const rpl_symbol_t *s = rpl_symbols_at(address);
      if (s)
         return s;
   }
   return NULL;
}

void rpl_symbols_remove_range(uint32_t address, int32_t length, uint32_t flags)
{
   for (; length > 0; length -= 4, address += 4)
   {
      addr_node_t *a = lookup(address);
      sym_node_t *s;
      if (!a)
         continue;
      for (s = (sym_node_t*)retro_atomic_load_acquire_ptr(&a->head); s; s = s->previous)
         if ((s->pub.flags & flags) == flags)
            retro_atomic_store_release_int(&s->removed, 1);
   }
}

void rpl_symbols_jump_proxy(uint32_t jump, uint32_t dest)
{
   const rpl_symbol_t *s = rpl_symbols_at(dest);
   if (s)
      rpl_symbols_store(s->lib, s->name, jump, 0);
}

void rpl_symbols_clear(void)
{
   block_t *b = (block_t*)retro_atomic_load_acquire_ptr(&s_block);
   size_t i;
   while (b)
   {
      block_t *next = b->next;
      free(b);
      b = next;
   }
   retro_atomic_store_release_ptr(&s_block, NULL);
   retro_atomic_store_release_ptr(&s_libs, NULL);
   for (i = 0; i < BUCKETS; i++)
      retro_atomic_store_relaxed_ptr(&s_buckets[i], NULL);
}

#ifdef RPL_SYMBOLS_TEST_HOOKS
size_t rpl_symbols_count(uint32_t address)
{
   const addr_node_t *a = lookup(address);
   const sym_node_t *s;
   size_t n = 0;
   if (!a)
      return 0;
   for (s = (const sym_node_t*)retro_atomic_load_acquire_ptr(
            (retro_atomic_ptr_t*)&a->head); s; s = s->previous)
      if (!retro_atomic_load_acquire_int((retro_atomic_int_t*)&s->removed))
         n++;
   return n;
}
#endif
