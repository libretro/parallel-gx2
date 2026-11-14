/* The recompiler's bookkeeping, with no lock (rec_store.h) */

#include "rec_store.h"

#ifdef REC_STORE_TEST_HOOKS
/* in a look, on the first range it passes */
void (*rec_store_test_mid_find)(void);
#define MID_FIND() do { if (rec_store_test_mid_find) rec_store_test_mid_find(); } while (0)
#else
#define MID_FIND() do { } while (0)
#endif

void rec_store_init(rec_store_t *s, retro_atomic_ptr_t *heads, size_t count, uint32_t granularity)
{
   size_t i;
   s->heads       = heads;
   s->count       = count;
   s->granularity = granularity;
   s->retired     = NULL;
   for (i = 0; i < count; i++)
      retro_atomic_ptr_init(&heads[i], NULL);
   retro_atomic_int_init(&s->readers, 0);
}

static void span(const rec_store_t *s, uint32_t start, uint32_t end, size_t *first, size_t *n)
{
   const uint32_t last = (end > start ? end - 1 : start) / s->granularity;
   *first = start / s->granularity;
   *n     = (size_t)(last - (uint32_t)*first) + 1;
   if (*n > s->count)
      *n = s->count;
}

size_t rec_store_nodes(const rec_store_t *s, uint32_t start, uint32_t end)
{
   size_t first, n;
   span(s, start, end, &first, &n);
   return n;
}

void rec_store_insert(rec_store_t *s, rec_node_t *nodes, void *obj, uint32_t start, uint32_t end)
{
   size_t first, n, i;
   span(s, start, end, &first, &n);
   for (i = 0; i < n; i++)
   {
      rec_node_t *node = &nodes[i];
      const size_t b   = (first + i) % s->count;
      node->obj        = obj;
      node->start      = start;
      node->end        = end;
      node->bucket     = b;
      node->retired    = NULL;
      node->group      = 0;
      retro_atomic_ptr_init(&node->next, retro_atomic_load_acquire_ptr(&s->heads[b]));
      /* whole before it is reachable */
      retro_atomic_store_release_ptr(&s->heads[b], node);
   }
   nodes[0].group = n;
   /* In before what the caller reads next: a look starts with the same
    * count, so of the two the later one sees the other's side */
   retro_atomic_fetch_add_seq_cst_int(&s->readers, 0);
}

void rec_store_remove(rec_store_t *s, rec_node_t *nodes, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
   {
      rec_node_t *node        = &nodes[i];
      retro_atomic_ptr_t *at  = &s->heads[node->bucket];
      for (;;)
      {
         rec_node_t *cur = (rec_node_t*)retro_atomic_load_acquire_ptr(at);
         if (!cur)
            break; /* not in it */
         if (cur == node)
         {
            /* a look on it goes on to what followed it */
            retro_atomic_store_release_ptr(at, retro_atomic_load_acquire_ptr(&node->next));
            break;
         }
         at = &cur->next;
      }
   }
   nodes[0].group   = n;
   nodes[0].retired = s->retired;
   s->retired       = &nodes[0];
}

void rec_store_collect(rec_store_t *s, void (*done)(rec_node_t *first, void *ud), void *ud)
{
   rec_node_t *g;
   if (!s->retired)
      return;
   /* the unlinks before, the count after (both on the count a look starts
    * with): a look not counted yet starts from the lists as they are now */
   if (retro_atomic_fetch_add_seq_cst_int(&s->readers, 0) != 0)
      return;
   g          = s->retired;
   s->retired = NULL;
   while (g)
   {
      rec_node_t *next = g->retired;
      done(g, ud);
      g = next;
   }
}

void rec_store_find(rec_store_t *s, uint32_t start, uint32_t end,
      void (*f)(void *obj, uint32_t start, uint32_t end, void *ud), void *ud)
{
   size_t first, n, i;
   bool hooked = false;
   span(s, start, end, &first, &n);
   retro_atomic_fetch_add_seq_cst_int(&s->readers, 1);
   for (i = 0; i < n; i++)
   {
      rec_node_t *node = (rec_node_t*)retro_atomic_load_acquire_ptr(&s->heads[(first + i) % s->count]);
      while (node)
      {
         if (start < node->end && end > node->start)
            f(node->obj, node->start, node->end, ud);
         if (!hooked)
         {
            hooked = true;
            MID_FIND();
         }
         node = (rec_node_t*)retro_atomic_load_acquire_ptr(&node->next);
      }
   }
   retro_atomic_fetch_add_int(&s->readers, -1);
}

void rec_inval_init(rec_inval_t *v)
{
   int i;
   retro_atomic_int_init(&v->next, 0);
   for (i = 0; i < REC_INVAL_SLOTS; i++)
   {
      retro_atomic_int_init(&v->slots[i].seq, 0);
      retro_atomic_int_init(&v->slots[i].start, 0);
      retro_atomic_int_init(&v->slots[i].end, 0);
   }
}

void rec_inval_add(rec_inval_t *v, uint32_t start, uint32_t end)
{
   const int seq          = retro_atomic_fetch_add_seq_cst_int(&v->next, 1);
   rec_inval_slot_t *slot = &v->slots[(unsigned)seq % REC_INVAL_SLOTS];
   /* marked being written before the range changes (a swap: nothing after
    * it moves before it) */
   retro_atomic_exchange_int(&slot->seq, 0);
   retro_atomic_store_release_int(&slot->start, (int)start);
   retro_atomic_store_release_int(&slot->end, (int)end);
   retro_atomic_store_release_int(&slot->seq, (int)((unsigned)seq + 1u));
   /* a reader that sees the sequence taken but the slot not yet written
    * counts it as touching everything */
}

int rec_inval_now(rec_inval_t *v)
{
   return retro_atomic_load_seq_cst_int(&v->next);
}

bool rec_inval_since(rec_inval_t *v, int from, uint32_t start, uint32_t end)
{
   unsigned s;
   const int now = retro_atomic_load_seq_cst_int(&v->next);
   if ((unsigned)now - (unsigned)from > REC_INVAL_SLOTS)
      return true; /* gone round: no telling */
   for (s = (unsigned)from; s != (unsigned)now; s++)
   {
      rec_inval_slot_t *slot = &v->slots[s % REC_INVAL_SLOTS];
      uint32_t rs, re;
      if ((unsigned)retro_atomic_load_acquire_int(&slot->seq) != s + 1u)
         return true; /* being written, or written over */
      rs = (uint32_t)retro_atomic_load_acquire_int(&slot->start);
      re = (uint32_t)retro_atomic_load_acquire_int(&slot->end);
      /* written over meanwhile: a read that writes, so it is not answered
       * from before the range was read */
      if ((unsigned)retro_atomic_fetch_add_int(&slot->seq, 0) != s + 1u)
         return true;
      if (start < re && end > rs)
         return true;
   }
   return false;
}
