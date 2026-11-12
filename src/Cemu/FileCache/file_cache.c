/* A cache file (file_cache.h) */

#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <streams/file_stream.h>

#include "util/helpers/mapped_file.h"
#include "util/helpers/zlib_stream.h"
#include "file_cache.h"

#define MAGIC_V1 0x8371b694UL /* before Cemu 1.7.4, caches up to 4 GiB: not read */
#define MAGIC_V2 0x8371b695UL /* large caches */
#define MAGIC_V3 0x8371b696UL /* Cemu 1.16.0: zlib compression */
#define HEADER_RESV 128 /* octets reserved for the header */
#define TABLE_NAME1 0xEFEFEFEFEFEFEFEFULL
#define TABLE_NAME2 0xFEFEFEFEFEFEFEFEULL
#define FLAG_COMPRESSED 1

/* readers are shown entries by table index, in chunks that never move */
#define SLOT_CHUNK 4096
#define SLOT_CHUNKS 4096

#ifdef FILECACHE_COUNT_PROBES
static retro_atomic_size_t s_probes;
#define PROBE() retro_atomic_fetch_add_size(&s_probes, 1)
size_t file_cache_probes(void)
{
   return retro_atomic_load_acquire_size(&s_probes);
}
void file_cache_probes_reset(void)
{
   retro_atomic_store_release_size(&s_probes, 0);
}
#else
#define PROBE() ((void)0)
#endif

#ifdef FILECACHE_TEST_HOOKS
void (*file_cache_read_hook)(uint64_t name1, uint64_t name2) = NULL;
#endif

/* an entry of the file table, as the file has it */
typedef struct table_entry
{
   uint64_t name1;
   uint64_t name2;
   uint64_t offset;
   uint32_t size;
   uint8_t flags;
   uint8_t reserved[3];
} table_entry_t;

/* What readers see of an entry: never changed. The writer puts a new one
 * in its place and keeps the old one until the cache goes, as a reader may
 * still be reading it. The data it points at is not written over before
 * the file is next opened: readers read the file itself. */
typedef struct record
{
   uint64_t name1;
   uint64_t name2;
   uint64_t offset;
   uint32_t size;
   int compressed;
   int64_t seq;                  /* the write it is from */
   struct record *retired_next;  /* the writer's */
} record_t;

/* The table index by name, for readers: a name's slot is written once and
 * its index published after it, so a reader finds a name or an empty slot.
 * Grown by publishing a copy twice the size; the old ones are kept until
 * the cache goes. */
typedef struct lookup_slot
{
   uint64_t name1;
   uint64_t name2;
   retro_atomic_int_t index; /* -1 for empty */
} lookup_slot_t;

typedef struct lookup_table
{
   size_t mask;
   size_t used;
   struct lookup_table *retired_next;
   lookup_slot_t slots[1];
} lookup_table_t;

/* the file's octets; a new mapping when a record is past the end of this
 * one, or newer than its copy where the file could not be mapped */
typedef struct mapping
{
   mapped_file_t file;
   int64_t seq; /* every write up to it is in it */
   struct mapping *retired_next;
} mapping_t;

typedef struct gap
{
   uint64_t offset;
   uint64_t size;
} gap_t;

struct file_cache
{
   char *path;
   RFILE *stream;
   uint64_t data_offset;
   uint32_t extra_version;
   retro_atomic_int_t compression; /* read by the writer as it writes */
   table_entry_t *table;
   int32_t table_count;
   uint64_t table_offset;
   uint32_t table_size;

   /* the writer's: the entries by name (open addressing, -1 empty), where
    * the scan for a free entry starts, and the space between entries */
   int32_t *index;
   size_t index_size;
   size_t index_used;
   int32_t free_hint;
   gap_t *gaps;   /* sorted by offset, none touching */
   size_t gap_count;
   size_t gap_cap;
   uint64_t data_end;
   record_t *retired_records;
   lookup_table_t *retired_lookups;

   /* the readers' */
   retro_atomic_ptr_t slots[SLOT_CHUNKS]; /* each retro_atomic_ptr_t[SLOT_CHUNK] of records */
   retro_atomic_int_t published_count;
   retro_atomic_int_t live_count;
   retro_atomic_ptr_t lookup;
   retro_atomic_ptr_t mapping;
   retro_atomic_ptr_t retired_mappings; /* a stack any reader pushes on */
   retro_atomic_64_t write_seq;
   retro_atomic_int_t pending_writes;
};

/* --- the writer thread ---------------------------------------------------- */

typedef struct job
{
   file_cache_t *fc;
   uint64_t name1;
   uint64_t name2;
   int is_delete;
   size_t size;
   struct job *next;
   /* then size octets */
} job_t;

static struct
{
   int ready;
   retro_atomic_ptr_t jobs;   /* pushed by any thread, newest first */
   retro_atomic_int_t queued;
   retro_atomic_int_t running;
   retro_atomic_int_t started;
   retro_eventcount_t wake;    /* the writer's sleep */
   retro_eventcount_t written; /* a cache's writes done; not the cache's own,
                                  as the writer notifies after the last thing
                                  it does with a cache that may then go */
   sthread_t *thread;
} s_writer;

static int writer_has_work(void)
{
   return retro_atomic_load_acquire_int(&s_writer.queued) != 0
      || !retro_atomic_load_acquire_int(&s_writer.running);
}

static void add_internal(file_cache_t *fc, uint64_t name1, uint64_t name2,
      const uint8_t *data, size_t size, int no_compression);
static void delete_internal(file_cache_t *fc, uint64_t name1, uint64_t name2);

static void writer_thread(void *arg)
{
   (void)arg;
   for (;;)
   {
      job_t *list, *next, *fifo = NULL;
      int any = 0;
      /* asleep until there is a job, or a stop: a stop still writes out
       * everything queued before it */
      while (!writer_has_work())
      {
         int key = retro_eventcount_prepare_wait(&s_writer.wake);
         if (writer_has_work())
         {
            retro_eventcount_cancel_wait(&s_writer.wake);
            break;
         }
         retro_eventcount_commit_wait(&s_writer.wake, key);
      }
      /* all of them at once, in the order they were asked for */
      for (list = (job_t*)retro_atomic_exchange_ptr(&s_writer.jobs, NULL); list; list = next)
      {
         next = list->next;
         list->next = fifo;
         fifo = list;
      }
      for (; fifo; fifo = next)
      {
         file_cache_t *fc = fifo->fc;
         next = fifo->next;
         any = 1;
         retro_atomic_fetch_sub_int(&s_writer.queued, 1);
         if (fifo->is_delete)
            delete_internal(fc, fifo->name1, fifo->name2);
         else
            add_internal(fc, fifo->name1, fifo->name2, (const uint8_t*)(fifo + 1), fifo->size, 0);
         free(fifo);
         /* the last this thread does with the cache */
         retro_atomic_fetch_sub_int(&fc->pending_writes, 1);
         retro_eventcount_notify(&s_writer.written);
      }
      if (!any && !retro_atomic_load_acquire_int(&s_writer.running)
            && retro_atomic_load_acquire_int(&s_writer.queued) == 0)
         return;
   }
}

bool file_cache_global_init(void)
{
   if (s_writer.ready)
      return true;
   retro_atomic_ptr_init(&s_writer.jobs, NULL);
   retro_atomic_int_init(&s_writer.queued, 0);
   retro_atomic_int_init(&s_writer.running, 0);
   retro_atomic_int_init(&s_writer.started, 0);
   if (!retro_eventcount_init(&s_writer.wake))
      return false;
   if (!retro_eventcount_init(&s_writer.written))
   {
      retro_eventcount_free(&s_writer.wake);
      return false;
   }
   s_writer.ready = 1;
   return true;
}

void file_cache_stop_writer(void)
{
   if (!s_writer.ready)
      return;
   retro_atomic_store_release_int(&s_writer.running, 0);
   retro_eventcount_notify(&s_writer.wake);
   if (s_writer.thread)
   {
      sthread_join(s_writer.thread);
      s_writer.thread = NULL;
   }
   retro_atomic_store_release_int(&s_writer.started, 0);
}

static void writer_add(file_cache_t *fc, uint64_t name1, uint64_t name2,
      const void *data, size_t size, int is_delete)
{
   job_t *job = (job_t*)malloc(sizeof(*job) + size);
   if (!job)
      return;
   job->fc = fc;
   job->name1 = name1;
   job->name2 = name2;
   job->is_delete = is_delete;
   job->size = size;
   if (size)
      memcpy(job + 1, data, size);
   retro_atomic_fetch_add_int(&fc->pending_writes, 1);
   retro_atomic_fetch_add_int(&s_writer.queued, 1);
   for (;;)
   {
      job_t *head = (job_t*)retro_atomic_load_acquire_ptr(&s_writer.jobs);
      job->next = head;
      if (retro_atomic_cas_ptr(&s_writer.jobs, head, job))
         break;
   }
   /* the first job since the last stop starts the thread */
   if (!retro_atomic_exchange_int(&s_writer.started, 1))
   {
      retro_atomic_store_release_int(&s_writer.running, 1);
      s_writer.thread = sthread_create(writer_thread, NULL);
   }
   retro_eventcount_notify(&s_writer.wake);
}

/* --- the file table and the space in the file (the writer's) ------------- */

static int is_free_name(uint64_t name1, uint64_t name2)
{
   return name1 == 0 && name2 == 0;
}

static int is_table_name(uint64_t name1, uint64_t name2)
{
   return name1 == TABLE_NAME1 && name2 == TABLE_NAME2;
}

static uint64_t name_hash(uint64_t name1, uint64_t name2)
{
   /* the names are hashes already, but of paths, and some callers number
    * them instead: mixed so that neither ends up in a run of slots */
   uint64_t h = name1 * 0x9E3779B97F4A7C15ULL
      ^ (name2 + 0x632BE59BD9B4E019ULL) * 0xC2B2AE3D27D4EB4FULL;
   return h ^ (h >> 29);
}

static int32_t find_entry(const file_cache_t *fc, uint64_t name1, uint64_t name2)
{
   size_t mask, slot;
   if (is_free_name(name1, name2) || !fc->index_size)
      return -1;
   mask = fc->index_size - 1;
   for (slot = (size_t)name_hash(name1, name2) & mask;; slot = (slot + 1) & mask)
   {
      int32_t i = fc->index[slot];
      PROBE();
      if (i < 0)
         return -1;
      if (fc->table[i].name1 == name1 && fc->table[i].name2 == name2)
         return i;
   }
}

/* counted when a write looks for its slot, not when the index regrows */
static void index_put(int32_t *index, size_t size, const table_entry_t *e,
      int32_t i, int counted)
{
   size_t mask = size - 1;
   size_t slot = (size_t)name_hash(e->name1, e->name2) & mask;
   while (index[slot] >= 0)
   {
      if (counted)
         PROBE();
      slot = (slot + 1) & mask;
   }
   index[slot] = i;
}

/* false for no memory */
static int index_insert(file_cache_t *fc, int32_t entry)
{
   if ((fc->index_used + 1) * 2 > fc->index_size)
   {
      /* twice the slots, the entries put in again */
      size_t size = fc->index_size ? fc->index_size * 2 : 64, i;
      int32_t *grown = (int32_t*)malloc(size * sizeof(*grown));
      if (!grown)
         return 0;
      for (i = 0; i < size; i++)
         grown[i] = -1;
      for (i = 0; i < fc->index_size; i++)
         if (fc->index[i] >= 0)
            index_put(grown, size, &fc->table[fc->index[i]], fc->index[i], 0);
      free(fc->index);
      fc->index = grown;
      fc->index_size = size;
   }
   index_put(fc->index, fc->index_size, &fc->table[entry], entry, 1);
   fc->index_used++;
   return 1;
}

static void index_erase(file_cache_t *fc, uint64_t name1, uint64_t name2)
{
   size_t mask, slot, hole, next;
   if (!fc->index_size)
      return;
   mask = fc->index_size - 1;
   for (slot = (size_t)name_hash(name1, name2) & mask;; slot = (slot + 1) & mask)
   {
      int32_t i = fc->index[slot];
      if (i < 0)
         return;
      if (fc->table[i].name1 == name1 && fc->table[i].name2 == name2)
         break;
   }
   /* moves back the slots after it that would no longer be found (no
    * tombstones to pile up) */
   hole = slot;
   for (next = (hole + 1) & mask; fc->index[next] >= 0; next = (next + 1) & mask)
   {
      const table_entry_t *e = &fc->table[fc->index[next]];
      size_t home = (size_t)name_hash(e->name1, e->name2) & mask;
      /* it stays if its home is cyclically within (hole, next] */
      int stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
      if (stays)
         continue;
      fc->index[hole] = fc->index[next];
      hole = next;
   }
   fc->index[hole] = -1;
   fc->index_used--;
}

static int32_t take_free_entry(file_cache_t *fc)
{
   int32_t i;
   for (i = fc->free_hint; i < fc->table_count; i++)
   {
      PROBE();
      if (is_free_name(fc->table[i].name1, fc->table[i].name2))
      {
         fc->free_hint = i + 1;
         return i;
      }
   }
   fc->free_hint = fc->table_count;
   return -1;
}

static int gap_insert_at(file_cache_t *fc, size_t at, uint64_t offset, uint64_t size)
{
   if (fc->gap_count == fc->gap_cap)
   {
      size_t cap = fc->gap_cap ? fc->gap_cap * 2 : 16;
      gap_t *g = (gap_t*)realloc(fc->gaps, cap * sizeof(*g));
      if (!g)
         return 0;
      fc->gaps = g;
      fc->gap_cap = cap;
   }
   memmove(fc->gaps + at + 1, fc->gaps + at, (fc->gap_count - at) * sizeof(*fc->gaps));
   fc->gaps[at].offset = offset;
   fc->gaps[at].size = size;
   fc->gap_count++;
   return 1;
}

static void gap_erase_at(file_cache_t *fc, size_t at)
{
   memmove(fc->gaps + at, fc->gaps + at + 1, (fc->gap_count - at - 1) * sizeof(*fc->gaps));
   fc->gap_count--;
}

static uint64_t allocate_space(file_cache_t *fc, uint64_t size)
{
   uint64_t offset;
   size_t g;
   if (!size)
      return fc->data_end;
   /* the first gap it fits in, else the end */
   for (g = 0; g < fc->gap_count; g++)
   {
      PROBE();
      if (fc->gaps[g].size < size)
         continue;
      offset = fc->gaps[g].offset;
      fc->gaps[g].offset += size;
      fc->gaps[g].size -= size;
      if (!fc->gaps[g].size)
         gap_erase_at(fc, g);
      return offset;
   }
   offset = fc->data_end;
   fc->data_end += size;
   return offset;
}

static void release_space(file_cache_t *fc, uint64_t offset, uint64_t size)
{
   size_t lo = 0, hi = fc->gap_count, at;
   if (!size)
      return;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      if (fc->gaps[mid].offset < offset)
         lo = mid + 1;
      else
         hi = mid;
   }
   if (!gap_insert_at(fc, lo, offset, size))
      return; /* the space is lost until the file is next opened */
   at = lo;
   /* joined with the gaps it touches */
   if (at + 1 < fc->gap_count && fc->gaps[at].offset + fc->gaps[at].size == fc->gaps[at + 1].offset)
   {
      fc->gaps[at].size += fc->gaps[at + 1].size;
      gap_erase_at(fc, at + 1);
   }
   if (at > 0 && fc->gaps[at - 1].offset + fc->gaps[at - 1].size == fc->gaps[at].offset)
   {
      fc->gaps[at - 1].size += fc->gaps[at].size;
      gap_erase_at(fc, at);
      at--;
   }
   /* a gap the data ends with is no gap */
   if (fc->gaps[at].offset + fc->gaps[at].size == fc->data_end)
   {
      fc->data_end = fc->gaps[at].offset;
      gap_erase_at(fc, at);
   }
}

static int cmp_gap(const void *a, const void *b)
{
   const gap_t *x = (const gap_t*)a, *y = (const gap_t*)b;
   return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* what the file table holds, kept beside it so that nothing has to scan
 * the table; false for no memory */
static int build_lookup(file_cache_t *fc)
{
   gap_t *used;
   size_t n = 0, i;
   int hint = 0;
   used = (gap_t*)malloc(((size_t)fc->table_count + 1) * sizeof(*used));
   if (!used)
      return 0;
   fc->free_hint = 0;
   fc->data_end = 0;
   /* the space in use: every entry's data, and the table itself, where the
    * header says it is (whatever entry 0 says) */
   used[n].offset = fc->table_offset;
   used[n++].size = fc->table_size;
   for (i = 0; i < (size_t)fc->table_count; i++)
   {
      const table_entry_t *e = &fc->table[i];
      if (is_free_name(e->name1, e->name2))
      {
         if (!hint)
            fc->free_hint = (int32_t)i;
         hint = 1;
         continue;
      }
      /* the first of two entries under one name is the one a scan found */
      if (find_entry(fc, e->name1, e->name2) < 0 && !index_insert(fc, (int32_t)i))
      {
         free(used);
         return 0;
      }
      used[n].offset = e->offset;
      used[n++].size = e->size;
   }
   if (!hint)
      fc->free_hint = fc->table_count;
   qsort(used, n, sizeof(*used), cmp_gap);
   for (i = 0; i < n; i++)
   {
      if (!used[i].size)
         continue;
      if (used[i].offset > fc->data_end
            && !gap_insert_at(fc, fc->gap_count, fc->data_end, used[i].offset - fc->data_end))
      {
         free(used);
         return 0;
      }
      if (used[i].offset + used[i].size > fc->data_end)
         fc->data_end = used[i].offset + used[i].size;
   }
   free(used);
   return 1;
}

/* --- what readers see (published by the writer, and the opening thread) -- */

static const record_t *slot_record(const file_cache_t *fc, int32_t entry)
{
   retro_atomic_ptr_t *chunk = (retro_atomic_ptr_t*)retro_atomic_load_acquire_ptr(
         (retro_atomic_ptr_t*)&fc->slots[(size_t)entry / SLOT_CHUNK]);
   return chunk ? (const record_t*)retro_atomic_load_acquire_ptr(&chunk[(size_t)entry % SLOT_CHUNK]) : NULL;
}

static const record_t *find_published(file_cache_t *fc, uint64_t name1, uint64_t name2)
{
   const lookup_table_t *t;
   size_t slot;
   if (is_free_name(name1, name2))
      return NULL;
   t = (const lookup_table_t*)retro_atomic_load_acquire_ptr(&fc->lookup);
   if (!t)
      return NULL;
   for (slot = (size_t)name_hash(name1, name2) & t->mask;; slot = (slot + 1) & t->mask)
   {
      const lookup_slot_t *s = &t->slots[slot];
      int32_t index = retro_atomic_load_acquire_int((retro_atomic_int_t*)&s->index);
      const record_t *r;
      PROBE();
      if (index < 0)
         return NULL;
      if (s->name1 != name1 || s->name2 != name2)
         continue;
      /* the entry the name was last written to: deleted since, or another
       * name's now, if its record says otherwise */
      r = slot_record(fc, index);
      return r && r->name1 == name1 && r->name2 == name2 ? r : NULL;
   }
}

static lookup_slot_t *lookup_find(lookup_table_t *t, uint64_t name1, uint64_t name2)
{
   size_t slot;
   for (slot = (size_t)name_hash(name1, name2) & t->mask;; slot = (slot + 1) & t->mask)
   {
      lookup_slot_t *s = &t->slots[slot];
      if (retro_atomic_load_relaxed_int(&s->index) < 0)
         return s;
      if (s->name1 == name1 && s->name2 == name2)
         return s;
   }
}

static lookup_table_t *lookup_new(size_t size)
{
   lookup_table_t *t = (lookup_table_t*)malloc(sizeof(*t) + (size - 1) * sizeof(lookup_slot_t));
   size_t i;
   if (!t)
      return NULL;
   t->mask = size - 1;
   t->used = 0;
   t->retired_next = NULL;
   for (i = 0; i < size; i++)
   {
      t->slots[i].name1 = 0;
      t->slots[i].name2 = 0;
      retro_atomic_int_init(&t->slots[i].index, -1);
   }
   return t;
}

/* a name's slot in the readers' lookup, made or pointed at the entry */
static void lookup_set(file_cache_t *fc, uint64_t name1, uint64_t name2, int32_t entry)
{
   lookup_table_t *t = (lookup_table_t*)retro_atomic_load_relaxed_ptr(&fc->lookup);
   lookup_slot_t *s;
   if (t)
   {
      s = lookup_find(t, name1, name2);
      if (retro_atomic_load_relaxed_int(&s->index) >= 0)
      {
         retro_atomic_store_release_int(&s->index, entry);
         return;
      }
   }
   if (!t || (t->used + 1) * 2 > t->mask + 1)
   {
      /* twice the slots, the names put in again, then published */
      lookup_table_t *grown = lookup_new(t ? (t->mask + 1) * 2 : 64);
      size_t i;
      if (!grown)
         return; /* not found by name until a later write grows it */
      if (t)
         for (i = 0; i <= t->mask; i++)
         {
            const lookup_slot_t *old = &t->slots[i];
            int32_t index = retro_atomic_load_relaxed_int((retro_atomic_int_t*)&old->index);
            if (index < 0)
               continue;
            s = lookup_find(grown, old->name1, old->name2);
            s->name1 = old->name1;
            s->name2 = old->name2;
            retro_atomic_store_relaxed_int(&s->index, index);
            grown->used++;
         }
      retro_atomic_store_release_ptr(&fc->lookup, grown);
      if (t)
      {
         t->retired_next = fc->retired_lookups;
         fc->retired_lookups = t;
      }
      t = grown;
   }
   s = lookup_find(t, name1, name2);
   s->name1 = name1;
   s->name2 = name2;
   retro_atomic_store_release_int(&s->index, entry);
   t->used++;
}

/* the table's size, chunks for it made before readers may look that far */
static void publish_count(file_cache_t *fc)
{
   size_t count = (size_t)fc->table_count, c;
   if (count > (size_t)SLOT_CHUNK * SLOT_CHUNKS)
      count = (size_t)SLOT_CHUNK * SLOT_CHUNKS;
   for (c = 0; c * SLOT_CHUNK < count; c++)
   {
      retro_atomic_ptr_t *chunk;
      size_t i;
      if (retro_atomic_load_relaxed_ptr(&fc->slots[c]))
         continue;
      chunk = (retro_atomic_ptr_t*)malloc(SLOT_CHUNK * sizeof(*chunk));
      if (!chunk)
      {
         count = c * SLOT_CHUNK;
         break;
      }
      for (i = 0; i < SLOT_CHUNK; i++)
         retro_atomic_ptr_init(&chunk[i], NULL);
      retro_atomic_store_release_ptr(&fc->slots[c], chunk);
   }
   retro_atomic_store_release_int(&fc->published_count, (int)count);
}

static void retire_record(file_cache_t *fc, const record_t *r)
{
   record_t *w = (record_t*)r;
   w->retired_next = fc->retired_records;
   fc->retired_records = w;
}

static void publish_entry(file_cache_t *fc, int32_t entry, int named)
{
   const table_entry_t *e = &fc->table[entry];
   retro_atomic_ptr_t *chunk;
   record_t *r;
   const record_t *old;
   if ((size_t)entry >= (size_t)SLOT_CHUNK * SLOT_CHUNKS)
      return; /* past what a reader can be shown: not found */
   if (is_free_name(e->name1, e->name2) || is_table_name(e->name1, e->name2))
      return;
   publish_count(fc);
   chunk = (retro_atomic_ptr_t*)retro_atomic_load_relaxed_ptr(&fc->slots[(size_t)entry / SLOT_CHUNK]);
   if (!chunk || !(r = (record_t*)malloc(sizeof(*r))))
      return;
   r->name1 = e->name1;
   r->name2 = e->name2;
   r->offset = e->offset;
   r->size = e->size;
   r->compressed = (e->flags & FLAG_COMPRESSED) != 0;
   r->seq = retro_atomic_fetch_add_64(&fc->write_seq, 1) + 1;
   r->retired_next = NULL;
   old = (const record_t*)retro_atomic_exchange_ptr(&chunk[(size_t)entry % SLOT_CHUNK], r);
   if (old)
      retire_record(fc, old);
   else
      retro_atomic_fetch_add_int(&fc->live_count, 1);
   /* the entry its name's lookup finds; of two entries under one name in a
    * file being opened, the first (named) */
   if (named)
      lookup_set(fc, e->name1, e->name2, entry);
}

static void unpublish_entry(file_cache_t *fc, int32_t entry)
{
   retro_atomic_ptr_t *chunk;
   const record_t *old;
   if ((size_t)entry >= (size_t)SLOT_CHUNK * SLOT_CHUNKS)
      return;
   chunk = (retro_atomic_ptr_t*)retro_atomic_load_relaxed_ptr(&fc->slots[(size_t)entry / SLOT_CHUNK]);
   if (!chunk)
      return;
   old = (const record_t*)retro_atomic_exchange_ptr(&chunk[(size_t)entry % SLOT_CHUNK], NULL);
   if (old)
   {
      retire_record(fc, old);
      retro_atomic_fetch_sub_int(&fc->live_count, 1);
   }
}

static void publish_init(file_cache_t *fc)
{
   int32_t i;
   publish_count(fc);
   for (i = 0; i < fc->table_count; i++)
      publish_entry(fc, i, find_entry(fc, fc->table[i].name1, fc->table[i].name2) == i);
}

/* --- writing (the writer's) ----------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
   memcpy(p, &v, 4); /* the file's numbers are the host's, as Cemu's are */
}

static void put_u64(uint8_t *p, uint64_t v)
{
   memcpy(p, &v, 8);
}

static void write_at(file_cache_t *fc, uint64_t at, const void *data, size_t size)
{
   filestream_seek(fc->stream, (int64_t)at, RETRO_VFS_SEEK_POSITION_START);
   if (size)
      filestream_write(fc->stream, data, (int64_t)size);
}

static void write_header(file_cache_t *fc)
{
   uint8_t h[28];
   put_u32(h, MAGIC_V3);
   put_u32(h + 4, fc->extra_version);
   put_u64(h + 8, fc->data_offset);
   put_u64(h + 16, fc->table_offset);
   put_u32(h + 24, fc->table_size);
   write_at(fc, 0, h, sizeof(h));
}

/* the table made extra entries bigger; false for no memory */
static int grow_table(file_cache_t *fc, int32_t extra)
{
   int32_t count = fc->table_count + extra;
   table_entry_t *t = (table_entry_t*)realloc(fc->table, (size_t)count * sizeof(*t));
   if (!t)
      return 0;
   memset(t + fc->table_count, 0, (size_t)extra * sizeof(*t));
   fc->table = t;
   fc->table_count = count;
   /* Entry 0 keeps the table's name: the write below finds it by that
    * name, puts the new table where nothing is (the old one is still in use
    * until the header points past it) and only then gives the old table's
    * space up */
   add_internal(fc, TABLE_NAME1, TABLE_NAME2, (const uint8_t*)fc->table,
         (size_t)count * sizeof(*t), 1);
   /* what was written is the table itself, its entry 0 already saying
    * where it now is */
   fc->table_offset = fc->table[0].offset;
   fc->table_size = fc->table[0].size;
   write_header(fc);
   filestream_flush(fc->stream);
   publish_count(fc);
   return 1;
}

/* as a zlib stream after its size, big-endian; NULL where it cannot be */
static uint8_t *compress_entry(const uint8_t *data, size_t size, size_t *out_size)
{
   size_t len = zlib_compress_room(size);
   uint8_t *out = (uint8_t*)malloc(4 + len);
   /* level 4: good compression for the time it takes */
   if (!out || size > 0xFFFFFFFFUL || !zlib_compress(out + 4, &len, data, size, 4))
   {
      free(out);
      return NULL;
   }
   out[0] = (uint8_t)(size >> 24);
   out[1] = (uint8_t)(size >> 16);
   out[2] = (uint8_t)(size >> 8);
   out[3] = (uint8_t)size;
   *out_size = 4 + len;
   return out;
}

static void add_internal(file_cache_t *fc, uint64_t name1, uint64_t name2,
      const uint8_t *data, size_t size, int no_compression)
{
   const uint8_t *raw = data;
   uint8_t *compressed = NULL;
   size_t raw_size = size;
   uint64_t replaced_offset, replaced_size, offset;
   int32_t entry;
   int replacing;
   table_entry_t *e;
   if (size > 0xFFFFFFFFUL)
      return;
   if (retro_atomic_load_acquire_int(&fc->compression) && !no_compression
         && (compressed = compress_entry(data, size, &raw_size)))
      raw = compressed;
   else
      raw_size = size;
   /* the entry already under this name, else a free one */
   entry = find_entry(fc, name1, name2);
   replacing = entry >= 0;
   if (!replacing)
   {
      entry = take_free_entry(fc);
      /* no free entry: a table twice as big (64 entries more at a time made
       * the writes of a large cache quadratic, and left a trail of old
       * tables behind in the file) */
      if (entry < 0 && !is_table_name(name1, name2)
            && grow_table(fc, fc->table_count > 64 ? fc->table_count : 64))
         entry = take_free_entry(fc);
      if (entry < 0)
      {
         free(compressed);
         return;
      }
   }
   /* the data goes where nothing is: what it replaces stays in use until
    * the entry points away from it */
   e = &fc->table[entry];
   replaced_offset = e->offset;
   replaced_size = replacing ? e->size : 0;
   offset = allocate_space(fc, raw_size);
   e->name1 = name1;
   e->name2 = name2;
   e->offset = offset;
   e->size = (uint32_t)raw_size;
   e->flags = compressed ? FLAG_COMPRESSED : 0;
   memset(e->reserved, 0, sizeof(e->reserved));
   if (!replacing && !index_insert(fc, entry))
   {
      /* not found again until the file is next opened */
   }
   write_at(fc, fc->data_offset + offset, raw, raw_size);
   write_at(fc, fc->data_offset + fc->table_offset + (uint64_t)entry * sizeof(*e), e, sizeof(*e));
   /* in the file before a reader can find it: readers read the file */
   filestream_flush(fc->stream);
   if (is_table_name(name1, name2))
      release_space(fc, replaced_offset, replaced_size); /* no reader reads the table */
   else
      publish_entry(fc, entry, 1); /* and what it replaces stays as it is */
   free(compressed);
}

static void delete_internal(file_cache_t *fc, uint64_t name1, uint64_t name2)
{
   int32_t entry = find_entry(fc, name1, name2);
   table_entry_t *e;
   if (entry < 0)
      return;
   index_erase(fc, name1, name2);
   e = &fc->table[entry];
   e->name1 = 0;
   e->name2 = 0;
   e->offset = 0;
   e->size = 0;
   write_at(fc, fc->data_offset + fc->table_offset + (uint64_t)entry * sizeof(*e), e, sizeof(*e));
   filestream_flush(fc->stream);
   /* its data stays as it is (record) */
   unpublish_entry(fc, entry);
   if (entry < fc->free_hint)
      fc->free_hint = entry;
}

/* --- opening and closing -------------------------------------------------- */

static file_cache_t *cache_new(const char *path, RFILE *stream)
{
   file_cache_t *fc = (file_cache_t*)calloc(1, sizeof(*fc));
   size_t i, len = strlen(path);
   if (!fc || !(fc->path = (char*)malloc(len + 1)))
   {
      free(fc);
      return NULL;
   }
   memcpy(fc->path, path, len + 1);
   fc->stream = stream;
   retro_atomic_int_init(&fc->compression, 1);
   for (i = 0; i < SLOT_CHUNKS; i++)
      retro_atomic_ptr_init(&fc->slots[i], NULL);
   retro_atomic_int_init(&fc->published_count, 0);
   retro_atomic_int_init(&fc->live_count, 0);
   retro_atomic_ptr_init(&fc->lookup, NULL);
   retro_atomic_ptr_init(&fc->mapping, NULL);
   retro_atomic_ptr_init(&fc->retired_mappings, NULL);
   retro_atomic_64_init(&fc->write_seq, 0);
   retro_atomic_int_init(&fc->pending_writes, 0);
   return fc;
}

static void cache_free(file_cache_t *fc)
{
   size_t c, i;
   mapping_t *m;
   lookup_table_t *t;
   record_t *r;
   for (c = 0; c < SLOT_CHUNKS; c++)
   {
      retro_atomic_ptr_t *chunk = (retro_atomic_ptr_t*)retro_atomic_load_relaxed_ptr(&fc->slots[c]);
      if (!chunk)
         continue;
      for (i = 0; i < SLOT_CHUNK; i++)
         free(retro_atomic_load_relaxed_ptr(&chunk[i]));
      free(chunk);
   }
   while ((r = fc->retired_records))
   {
      fc->retired_records = r->retired_next;
      free(r);
   }
   free(retro_atomic_load_relaxed_ptr(&fc->lookup));
   while ((t = fc->retired_lookups))
   {
      fc->retired_lookups = t->retired_next;
      free(t);
   }
   if ((m = (mapping_t*)retro_atomic_load_relaxed_ptr(&fc->mapping)))
   {
      mapped_file_close(&m->file);
      free(m);
   }
   for (m = (mapping_t*)retro_atomic_load_relaxed_ptr(&fc->retired_mappings); m; )
   {
      mapping_t *next = m->retired_next;
      mapped_file_close(&m->file);
      free(m);
      m = next;
   }
   if (fc->stream)
      filestream_close(fc->stream);
   free(fc->table);
   free(fc->index);
   free(fc->gaps);
   free(fc->path);
   free(fc);
}

/* the lookup and the readers' view of a file just opened or made */
static file_cache_t *cache_ready(file_cache_t *fc)
{
   if (!build_lookup(fc))
   {
      cache_free(fc);
      return NULL;
   }
   publish_init(fc);
   return fc;
}

file_cache_t *file_cache_create(const char *path, uint32_t extra_version)
{
   RFILE *stream = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   file_cache_t *fc;
   if (!stream)
      return NULL;
   if (!(fc = cache_new(path, stream)))
   {
      filestream_close(stream);
      return NULL;
   }
   fc->extra_version = extra_version;
   fc->data_offset = HEADER_RESV;
   fc->table_count = 32;
   fc->table_offset = 0;
   fc->table_size = (uint32_t)(32 * sizeof(table_entry_t));
   if (!(fc->table = (table_entry_t*)calloc(32, sizeof(table_entry_t))))
   {
      cache_free(fc);
      return NULL;
   }
   /* the file table says where it is itself */
   fc->table[0].name1 = TABLE_NAME1;
   fc->table[0].name2 = TABLE_NAME2;
   fc->table[0].offset = fc->table_offset;
   fc->table[0].size = fc->table_size;
   write_header(fc);
   write_at(fc, fc->data_offset + fc->table_offset, fc->table, fc->table_size);
   filestream_flush(stream);
   return cache_ready(fc);
}

static file_cache_t *open_existing(const char *path, int check_version,
      uint32_t extra_version, bool *corrupt)
{
   uint8_t h[28];
   uint32_t magic, version, table_size, count, i;
   uint64_t data_offset, table_offset;
   int64_t got;
   file_cache_t *fc;
   RFILE *stream = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!stream)
      return NULL;
   got = filestream_read(stream, h, sizeof(h));
   if (got < 8)
   {
      if (got >= 4)
      {
         memcpy(&magic, h, 4);
         if (corrupt && (magic == MAGIC_V2 || magic == MAGIC_V3))
            *corrupt = true;
      }
      filestream_close(stream);
      return NULL;
   }
   memcpy(&magic, h, 4);
   memcpy(&version, h + 4, 4);
   /* V1 was dropped with V3 */
   if ((magic != MAGIC_V2 && magic != MAGIC_V3)
         || (check_version && version != extra_version))
   {
      filestream_close(stream);
      return NULL;
   }
   memcpy(&data_offset, h + 8, 8);
   memcpy(&table_offset, h + 16, 8);
   memcpy(&table_size, h + 24, 4);
   if (got < (int64_t)sizeof(h) || table_size % sizeof(table_entry_t))
   {
      if (corrupt)
         *corrupt = true;
      filestream_close(stream);
      return NULL;
   }
   count = table_size / (uint32_t)sizeof(table_entry_t);
   if (!(fc = cache_new(path, stream)))
   {
      filestream_close(stream);
      return NULL;
   }
   fc->extra_version = version;
   fc->data_offset = data_offset;
   fc->table_count = (int32_t)count;
   fc->table_offset = table_offset;
   fc->table_size = table_size;
   fc->table = (table_entry_t*)calloc(count ? count : 1, sizeof(table_entry_t));
   if (!fc->table)
   {
      cache_free(fc);
      return NULL;
   }
   filestream_seek(stream, (int64_t)(data_offset + table_offset), RETRO_VFS_SEEK_POSITION_START);
   if (table_size && filestream_read(stream, fc->table, table_size) != (int64_t)table_size)
   {
      if (corrupt)
         *corrupt = true;
      cache_free(fc);
      return NULL;
   }
   /* in V2 the flags were not guaranteed to be anything */
   if (magic == MAGIC_V2)
      for (i = 0; i < count; i++)
      {
         fc->table[i].flags = 0;
         memset(fc->table[i].reserved, 0, sizeof(fc->table[i].reserved));
      }
   return cache_ready(fc);
}

file_cache_t *file_cache_open(const char *path, bool allow_create,
      uint32_t extra_version, bool *corrupt)
{
   file_cache_t *fc;
   if (corrupt)
      *corrupt = false;
   if ((fc = open_existing(path, 1, extra_version, corrupt)))
      return fc;
   return allow_create ? file_cache_create(path, extra_version) : NULL;
}

file_cache_t *file_cache_open_any(const char *path, bool *corrupt)
{
   if (corrupt)
      *corrupt = false;
   return open_existing(path, 0, 0, corrupt);
}

void file_cache_close(file_cache_t *fc)
{
   if (!fc)
      return;
   file_cache_wait(fc);
   cache_free(fc);
}

void file_cache_use_compression(file_cache_t *fc, bool enable)
{
   retro_atomic_store_release_int(&fc->compression, enable ? 1 : 0);
}

/* --- the callers' side ---------------------------------------------------- */

void file_cache_add(file_cache_t *fc, uint64_t name1, uint64_t name2,
      const void *data, size_t size)
{
   writer_add(fc, name1, name2, data, size, 0);
}

static int writes_done(file_cache_t *fc)
{
   return retro_atomic_load_acquire_int(&fc->pending_writes) == 0;
}

void file_cache_wait(file_cache_t *fc)
{
   while (!writes_done(fc))
   {
      int key = retro_eventcount_prepare_wait(&s_writer.written);
      if (writes_done(fc))
      {
         retro_eventcount_cancel_wait(&s_writer.written);
         return;
      }
      retro_eventcount_commit_wait(&s_writer.written, key);
   }
}

bool file_cache_delete(file_cache_t *fc, uint64_t name1, uint64_t name2)
{
   if (is_table_name(name1, name2))
      return false; /* the file table is not to be deleted */
   /* as of every write asked for before it */
   file_cache_wait(fc);
   if (!find_published(fc, name1, name2))
      return false;
   writer_add(fc, name1, name2, NULL, 0, 1);
   file_cache_wait(fc);
   return true;
}

/* the mapping that holds the record's data, made anew when the one there
 * does not; the one that loses a race to publish its own takes the
 * winner's */
static mapping_t *mapping_for(file_cache_t *fc, const record_t *r, uint64_t end)
{
   mapping_t *m = (mapping_t*)retro_atomic_load_acquire_ptr(&fc->mapping);
   int tries;
   for (tries = 0; tries < 2; tries++)
   {
      mapping_t *fresh;
      if (m && m->file.size >= end && (m->file.mapped || m->seq >= r->seq))
         break;
      if (!(fresh = (mapping_t*)calloc(1, sizeof(*fresh))))
         break;
      fresh->seq = retro_atomic_load_acquire_64(&fc->write_seq);
      mapped_file_open(&fresh->file, fc->path);
      if (retro_atomic_cas_ptr(&fc->mapping, m, fresh))
      {
         if (m)
            for (;;)
            {
               mapping_t *head = (mapping_t*)retro_atomic_load_acquire_ptr(&fc->retired_mappings);
               m->retired_next = head;
               if (retro_atomic_cas_ptr(&fc->retired_mappings, head, m))
                  break;
            }
         m = fresh;
      }
      else
      {
         mapped_file_close(&fresh->file);
         free(fresh);
         m = (mapping_t*)retro_atomic_load_acquire_ptr(&fc->mapping);
      }
   }
   return m && m->file.size >= end ? m : NULL;
}

static bool read_record(file_cache_t *fc, const record_t *r,
      file_cache_alloc_t alloc, void *ctx)
{
   const uint64_t end = fc->data_offset + r->offset + r->size;
   const mapping_t *m;
   const uint8_t *data;
   uint8_t *out;
   uint32_t size;
   size_t out_len, in_len;
#ifdef FILECACHE_TEST_HOOKS
   if (file_cache_read_hook)
      file_cache_read_hook(r->name1, r->name2);
#endif
   if (!(m = mapping_for(fc, r, end)))
      return false;
   /* straight from the file's pages into the caller's buffer */
   data = m->file.data + fc->data_offset + r->offset;
   if (!r->compressed)
   {
      if (!(out = (uint8_t*)alloc(ctx, r->size)) && r->size)
         return false;
      if (r->size)
         memcpy(out, data, r->size);
      return true;
   }
   /* the size it was, big-endian, then the zlib stream */
   if (r->size < 4)
      return false;
   size = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
      | ((uint32_t)data[2] << 8) | data[3];
   if (!(out = (uint8_t*)alloc(ctx, size)) && size)
      return false;
   out_len = size;
   in_len = r->size - 4;
   return zlib_uncompress(out, &out_len, data + 4, &in_len, 15) == ZLIB_OK
      && out_len == size && in_len == r->size - 4;
}

bool file_cache_get(file_cache_t *fc, uint64_t name1, uint64_t name2,
      file_cache_alloc_t alloc, void *ctx)
{
   const record_t *r = find_published(fc, name1, name2);
   return r && read_record(fc, r, alloc, ctx);
}

bool file_cache_get_index(file_cache_t *fc, int32_t index, uint64_t *name1,
      uint64_t *name2, file_cache_alloc_t alloc, void *ctx)
{
   const record_t *r;
   if (index < 0 || index >= retro_atomic_load_acquire_int(&fc->published_count))
      return false;
   /* none for a free entry or the file table's own */
   if (!(r = slot_record(fc, index)))
      return false;
   if (name1)
      *name1 = r->name1;
   if (name2)
      *name2 = r->name2;
   return read_record(fc, r, alloc, ctx);
}

bool file_cache_has(file_cache_t *fc, uint64_t name1, uint64_t name2)
{
   return find_published(fc, name1, name2) != NULL;
}

int32_t file_cache_count(file_cache_t *fc)
{
   return retro_atomic_load_acquire_int(&fc->live_count);
}

int32_t file_cache_max_index(file_cache_t *fc)
{
   return retro_atomic_load_acquire_int(&fc->published_count);
}
