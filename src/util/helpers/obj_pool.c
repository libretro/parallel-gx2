/* A lock-free pool of same-size blocks (obj_pool.h) */

#include <stdlib.h>
#include <stdint.h>

#include "obj_pool.h"

/* a block's header: its index, or LOOSE for one malloc'd on its own */
#define HEADER 16u
#define LOOSE 0xffffffffu

static unsigned char *block_at(const obj_pool_t *p, unsigned char *chunk, unsigned i)
{
   return chunk + (size_t)i * p->stride;
}

void obj_pool_init(obj_pool_t *p, void *ring, unsigned size, unsigned per_chunk)
{
   int i;
   lf_ring_init(&p->free, ring, per_chunk * OBJ_POOL_MAX_CHUNKS, 4);
   for (i = 0; i < OBJ_POOL_MAX_CHUNKS; i++)
      retro_atomic_ptr_init(&p->chunks[i], NULL);
   retro_atomic_int_init(&p->claimed, 0);
   retro_atomic_int_init(&p->loose, 0);
   retro_atomic_int_init(&p->out, 0);
   p->size = size;
   p->stride = (unsigned)(HEADER + ((size + 15u) & ~15u));
   p->per_chunk = per_chunk;
}

/* a new chunk: its first block for the caller, the rest free; NULL when
 * there is no room for another or no memory */
static unsigned char *grow(obj_pool_t *p)
{
   const int c = retro_atomic_fetch_add_int(&p->claimed, 1);
   unsigned char *chunk;
   unsigned i;
   if (c >= OBJ_POOL_MAX_CHUNKS)
   {
      retro_atomic_fetch_add_int(&p->claimed, -1);
      return NULL;
   }
   chunk = (unsigned char*)malloc((size_t)p->per_chunk * p->stride);
   if (!chunk)
   {
      /* the slot stays claimed: it is not tried again */
      return NULL;
   }
   for (i = 0; i < p->per_chunk; i++)
      *(uint32_t*)block_at(p, chunk, i) = (uint32_t)c * p->per_chunk + i;
   retro_atomic_store_release_ptr(&p->chunks[c], chunk);
   for (i = 1; i < p->per_chunk; i++)
   {
      uint32_t index = (uint32_t)c * p->per_chunk + i;
      lf_ring_push(&p->free, &index);
   }
   return block_at(p, chunk, 0);
}

bool obj_pool_reserve(obj_pool_t *p, unsigned chunks)
{
   while ((unsigned)retro_atomic_load_acquire_int(&p->claimed) < chunks)
   {
      unsigned char *b = grow(p);
      if (!b)
         return false;
      retro_atomic_fetch_add_int(&p->out, 1);
      obj_pool_put(p, b + HEADER);
   }
   return true;
}

void *obj_pool_get(obj_pool_t *p)
{
   uint32_t index;
   unsigned char *b;
   retro_atomic_fetch_add_int(&p->out, 1);
   if (lf_ring_pop(&p->free, &index))
   {
      unsigned char *chunk = (unsigned char*)retro_atomic_load_acquire_ptr(
            &p->chunks[index / p->per_chunk]);
      return block_at(p, chunk, index % p->per_chunk) + HEADER;
   }
   if ((b = grow(p)) != NULL)
      return b + HEADER;
   /* every chunk made: one on its own */
   if (!(b = (unsigned char*)malloc(p->stride)))
   {
      retro_atomic_fetch_add_int(&p->out, -1);
      return NULL;
   }
   *(uint32_t*)b = LOOSE;
   retro_atomic_fetch_add_int(&p->loose, 1);
   return b + HEADER;
}

void obj_pool_put(obj_pool_t *p, void *block)
{
   unsigned char *b;
   uint32_t index;
   if (!block)
      return;
   retro_atomic_fetch_add_int(&p->out, -1);
   b = (unsigned char*)block - HEADER;
   index = *(uint32_t*)b;
   if (index == LOOSE)
   {
      retro_atomic_fetch_add_int(&p->loose, -1);
      free(b);
      return;
   }
   /* room for every block there is: never full */
   lf_ring_push(&p->free, &index);
}

int obj_pool_out(obj_pool_t *p)
{
   return retro_atomic_load_acquire_int(&p->out);
}

bool obj_pool_free_all(obj_pool_t *p)
{
   int i;
   uint32_t index;
   if (obj_pool_out(p) != 0)
      return false;
   while (lf_ring_pop(&p->free, &index))
      ;
   for (i = 0; i < OBJ_POOL_MAX_CHUNKS; i++)
   {
      free(retro_atomic_load_acquire_ptr(&p->chunks[i]));
      retro_atomic_store_release_ptr(&p->chunks[i], NULL);
   }
   retro_atomic_store_release_int(&p->claimed, 0);
   return true;
}
