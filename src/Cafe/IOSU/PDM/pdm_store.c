/* The play statistics and play diary (pdm_store.h) */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <retro_atomic.h>
#include <file/file_path.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <streams/file_stream.h>

#include "Common/host_path.h"
#include "pdm_store.h"

#define STATS_FILE_SIZE (4 + PDM_PLAY_STATS_ENTRIES * PDM_PLAY_STATS_ENTRY_SIZE)
#define DIARY_HEADER_SIZE 8
/* 1970-01-01 to 2000-01-01 */
#define DAYS_TO_2000 10957
#define PATH_SIZE 4096

static struct
{
   RFILE *file;
   uint32_t count;
   uint8_t entry[PDM_PLAY_STATS_ENTRIES * PDM_PLAY_STATS_ENTRY_SIZE];
   char path[PATH_SIZE];
} s_stats;

static struct
{
   RFILE *file;
   uint32_t read_index;
   uint32_t write_index;
   uint8_t entry[PDM_PLAY_DIARY_ENTRIES * PDM_PLAY_DIARY_ENTRY_SIZE];
   char path[PATH_SIZE];
} s_diary;

static struct
{
   sthread_t *thread;
   retro_eventcount_t wake;
   bool wake_ready;
   retro_atomic_int_t stop;
   retro_atomic_int_t write_failures;
   int entry;
   int64_t start_us;
   int64_t wake_us;
   uint32_t counted;
} s_track;

static int64_t real_now_us(void) { return (int64_t)cpu_features_get_time_usec(); }
static int64_t (*s_now_us)(void) = real_now_us;
static uint16_t (*s_today)(void) = NULL;

static uint32_t get_be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t get_be16(const uint8_t *p)
{
   return (uint16_t)((p[0] << 8) | p[1]);
}

static void put_be32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static void put_be16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

/* an entry's fields */
#define ST_ID_HIGH   0
#define ST_ID_LOW    4
#define ST_MINUTES   8
#define ST_LAUNCHES 12
#define ST_FIRST    14
#define ST_LAST     16
#define ST_UKN12    18

static uint8_t *stats_entry(int i)
{
   return s_stats.entry + (size_t)i * PDM_PLAY_STATS_ENTRY_SIZE;
}

uint16_t pdm_today(void)
{
   time_t t;
   if (s_today)
      return s_today();
   t = time(NULL);
   return (uint16_t)((long)(t / 86400) - DAYS_TO_2000);
}

void pdm_set_clocks(int64_t (*now_us)(void), uint16_t (*today)(void))
{
   s_now_us = now_us ? now_us : real_now_us;
   s_today = today;
}

/* The file, opened to read and write: an existing one (NULL for none), or
 * one made anew, empty. The path matched without regard to case, as every
 * host file is. Neither mode opens a directory. */
static RFILE *open_rw(const char *path, unsigned mode)
{
   char *p = host_path_resolve(path);
   RFILE *f = filestream_open(p ? p : path, mode, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   free(p);
   return f;
}

static RFILE *open_existing(const char *path)
{
   return open_rw(path, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING);
}

static RFILE *create(const char *path)
{
   return open_rw(path, RETRO_VFS_FILE_ACCESS_READ_WRITE);
}

static bool write_at(RFILE *f, int64_t pos, const void *data, int64_t size)
{
   if (filestream_seek(f, pos, RETRO_VFS_SEEK_POSITION_START) != 0
         || filestream_write(f, data, size) != size)
   {
      retro_atomic_fetch_add_int(&s_track.write_failures, 1);
      return false;
   }
   return true;
}

static unsigned stats_open(void)
{
   uint8_t count[4];
   s_stats.count = 0;
   memset(s_stats.entry, 0, sizeof(s_stats.entry));
   s_stats.file = open_existing(s_stats.path);
   if (!s_stats.file)
   {
      s_stats.file = create(s_stats.path);
      if (!s_stats.file)
         return PDM_STATS_UNAVAILABLE;
      memset(count, 0, sizeof(count));
      write_at(s_stats.file, 0, count, 4);
      write_at(s_stats.file, 4, s_stats.entry, sizeof(s_stats.entry));
      return PDM_STATS_CREATED;
   }
   if (filestream_get_size(s_stats.file) != STATS_FILE_SIZE)
   {
      /* kept as it is, in case it can be salvaged; nothing tracked */
      filestream_close(s_stats.file);
      s_stats.file = NULL;
      return PDM_STATS_MALFORMED;
   }
   if (filestream_read(s_stats.file, count, 4) == 4)
      s_stats.count = get_be32(count);
   if (s_stats.count > PDM_PLAY_STATS_ENTRIES)
      s_stats.count = PDM_PLAY_STATS_ENTRIES;
   filestream_read(s_stats.file, s_stats.entry, sizeof(s_stats.entry));
   return 0;
}

static unsigned diary_create(void)
{
   uint8_t header[DIARY_HEADER_SIZE];
   s_diary.read_index = 0;
   s_diary.write_index = 0;
   s_diary.file = create(s_diary.path);
   if (!s_diary.file)
      return PDM_DIARY_UNAVAILABLE;
   memset(header, 0, sizeof(header));
   write_at(s_diary.file, 0, header, sizeof(header));
   return PDM_DIARY_CREATED;
}

static unsigned diary_open(void)
{
   uint8_t header[DIARY_HEADER_SIZE];
   unsigned r = 0;
   s_diary.read_index = 0;
   s_diary.write_index = 0;
   memset(s_diary.entry, 0, sizeof(s_diary.entry));
   s_diary.file = open_existing(s_diary.path);
   if (!s_diary.file)
      return diary_create();
   if (filestream_read(s_diary.file, header, sizeof(header)) != sizeof(header))
   {
      filestream_close(s_diary.file);
      s_diary.file = NULL;
      return PDM_DIARY_BAD_HEADER | diary_create();
   }
   s_diary.read_index = get_be32(header);
   s_diary.write_index = get_be32(header + 4);
   /* an index of PDM_PLAY_DIARY_ENTRIES is one past the end too */
   if (s_diary.read_index >= PDM_PLAY_DIARY_ENTRIES
         || s_diary.write_index >= PDM_PLAY_DIARY_ENTRIES)
   {
      s_diary.read_index %= PDM_PLAY_DIARY_ENTRIES;
      s_diary.write_index %= PDM_PLAY_DIARY_ENTRIES;
      r |= PDM_DIARY_BAD_INDEX;
   }
   /* what the file does not have stays zero */
   filestream_read(s_diary.file, s_diary.entry, sizeof(s_diary.entry));
   return r;
}

unsigned pdm_load(const char *dir)
{
   path_mkdir(dir);
   fill_pathname_join_special(s_stats.path, dir, "PlayStats.dat", sizeof(s_stats.path));
   fill_pathname_join_special(s_diary.path, dir, "PlayDiary.dat", sizeof(s_diary.path));
   pdm_close();
   return stats_open() | diary_open();
}

void pdm_close(void)
{
   if (s_stats.file)
      filestream_close(s_stats.file);
   s_stats.file = NULL;
   if (s_diary.file)
      filestream_close(s_diary.file);
   s_diary.file = NULL;
}

void pdm_unload(void)
{
   pdm_close();
   s_stats.count = 0;
   memset(s_stats.entry, 0, sizeof(s_stats.entry));
   s_diary.read_index = 0;
   s_diary.write_index = 0;
   memset(s_diary.entry, 0, sizeof(s_diary.entry));
}

static int stats_find(uint64_t title_id)
{
   const uint32_t high = (uint32_t)(title_id >> 32);
   const uint32_t low = (uint32_t)title_id;
   uint32_t i;
   for (i = 0; i < s_stats.count; i++)
   {
      const uint8_t *e = stats_entry((int)i);
      if (get_be32(e + ST_ID_HIGH) == high && get_be32(e + ST_ID_LOW) == low)
         return (int)i;
   }
   return -1;
}

static void stats_write(int i, bool with_count)
{
   uint8_t count[4];
   if (!s_stats.file)
      return;
   if (!write_at(s_stats.file, 4 + (int64_t)i * PDM_PLAY_STATS_ENTRY_SIZE,
            stats_entry(i), PDM_PLAY_STATS_ENTRY_SIZE))
      return;
   if (with_count)
   {
      put_be32(count, s_stats.count);
      write_at(s_stats.file, 0, count, 4);
   }
   /* on disk as it is counted, not when the title stops */
   filestream_flush(s_stats.file);
}

static int stats_begin(uint64_t title_id)
{
   const uint16_t today = pdm_today();
   bool count_changed = false;
   uint8_t *e;
   int i = stats_find(title_id);
   if (i >= 0)
   {
      e = stats_entry(i);
      put_be16(e + ST_LAST, today);
      put_be16(e + ST_LAUNCHES, (uint16_t)(get_be16(e + ST_LAUNCHES) + 1));
      stats_write(i, false);
      return i;
   }
   if (s_stats.count < PDM_PLAY_STATS_ENTRIES)
   {
      i = (int)s_stats.count++;
      count_changed = true;
   }
   else
   {
      /* all taken: the title played least gives way */
      int j;
      i = 0;
      for (j = 1; j < PDM_PLAY_STATS_ENTRIES; j++)
         if (get_be32(stats_entry(j) + ST_MINUTES) < get_be32(stats_entry(i) + ST_MINUTES))
            i = j;
   }
   e = stats_entry(i);
   put_be32(e + ST_ID_HIGH, (uint32_t)(title_id >> 32));
   put_be32(e + ST_ID_LOW, (uint32_t)title_id);
   put_be32(e + ST_MINUTES, 0);
   put_be16(e + ST_LAUNCHES, 1);
   put_be16(e + ST_FIRST, today);
   put_be16(e + ST_LAST, today);
   put_be16(e + ST_UKN12, 0);
   stats_write(i, count_changed);
   return i;
}

/* the whole minutes since the start not yet added */
static void count_minutes(void)
{
   uint8_t *e;
   const int64_t elapsed_us = s_now_us() - s_track.start_us;
   const uint32_t minutes = elapsed_us > 0 ? (uint32_t)(elapsed_us / 60000000) : 0;
   if (minutes <= s_track.counted || s_track.entry < 0)
      return;
   e = stats_entry(s_track.entry);
   put_be32(e + ST_MINUTES, get_be32(e + ST_MINUTES) + (minutes - s_track.counted));
   put_be16(e + ST_LAST, pdm_today());
   s_track.counted = minutes;
   stats_write(s_track.entry, false);
}

static void tracking_thread(void *unused)
{
   (void)unused;
   for (;;)
   {
      int key = retro_eventcount_prepare_wait(&s_track.wake);
      if (retro_atomic_load_acquire_int(&s_track.stop))
      {
         retro_eventcount_cancel_wait(&s_track.wake);
         break;
      }
      retro_eventcount_commit_wait_timeout(&s_track.wake, key, s_track.wake_us);
      if (retro_atomic_load_acquire_int(&s_track.stop))
         break;
      count_minutes();
   }
}

bool pdm_tracking_start(uint64_t title_id, int64_t wake_us)
{
   if (s_track.thread)
      return false;
   if (!s_track.wake_ready)
   {
      if (!retro_eventcount_init(&s_track.wake))
         return false;
      s_track.wake_ready = true;
   }
   s_track.entry = stats_begin(title_id);
   s_track.start_us = s_now_us();
   s_track.wake_us = wake_us;
   s_track.counted = 0;
   retro_atomic_store_release_int(&s_track.stop, 0);
   s_track.thread = sthread_create(tracking_thread, NULL);
   return s_track.thread != NULL;
}

unsigned pdm_tracking_stop(void)
{
   if (s_track.thread)
   {
      retro_atomic_store_release_int(&s_track.stop, 1);
      retro_eventcount_notify(&s_track.wake);
      sthread_join(s_track.thread);
      s_track.thread = NULL;
      /* the minutes since its last wake */
      count_minutes();
   }
   s_track.entry = -1;
   return (unsigned)retro_atomic_exchange_int(&s_track.write_failures, 0);
}

uint32_t pdm_diary_get(void *out, uint32_t max)
{
   uint8_t *o = (uint8_t*)out;
   uint32_t n = 0;
   uint32_t i = s_diary.read_index;
   while (i != s_diary.write_index && n < max)
   {
      memcpy(o, s_diary.entry + (size_t)i * PDM_PLAY_DIARY_ENTRY_SIZE, PDM_PLAY_DIARY_ENTRY_SIZE);
      o += PDM_PLAY_DIARY_ENTRY_SIZE;
      n++;
      i = (i + 1) % PDM_PLAY_DIARY_ENTRIES;
   }
   return n;
}

bool pdm_stats_get(uint64_t title_id, uint32_t *minutes, uint16_t *launches,
      uint16_t *first_day, uint16_t *last_day)
{
   const uint8_t *e;
   const int i = stats_find(title_id);
   if (i < 0)
      return false;
   e = stats_entry(i);
   if (minutes)
      *minutes = get_be32(e + ST_MINUTES);
   if (launches)
      *launches = get_be16(e + ST_LAUNCHES);
   if (first_day)
      *first_day = get_be16(e + ST_FIRST);
   if (last_day)
      *last_day = get_be16(e + ST_LAST);
   return true;
}

uint32_t pdm_stats_count(void)
{
   return s_stats.count;
}
