/* The play statistics and diary (src/Cafe/IOSU/PDM/pdm_store.c): the files
 * made where they are missing, a title's entry begun, counted and written
 * where the console has it, the least played title giving way once all 256
 * are taken, a malformed file left alone, the diary read as the ring it is,
 * an index one past the end wrapped, and the counting thread stopped at once
 * rather than at its next wake. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <retro_atomic.h>
#include <file/file_path.h>
#include <features/features_cpu.h>
#include <streams/file_stream.h>

#include "Common/host_path.h"
#include "Cafe/IOSU/PDM/pdm_store.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define DIR_ "pdm_test_dir"
#define STATS DIR_ "/PlayStats.dat"
#define DIARY DIR_ "/PlayDiary.dat"
#define STATS_SIZE (4 + 256 * 20)
#define TITLE 0x0005000010101D00ULL
#define MIN_US 60000000

static retro_atomic_64_t s_fake_us;
static retro_atomic_int_t s_fake_day;

static int64_t fake_now(void) { return (int64_t)retro_atomic_load_acquire_64(&s_fake_us); }
static uint16_t fake_today(void) { return (uint16_t)retro_atomic_load_acquire_int(&s_fake_day); }

static void set_clock(int64_t us, int day)
{
   retro_atomic_store_release_64(&s_fake_us, (uint64_t)us);
   retro_atomic_store_release_int(&s_fake_day, day);
}

static uint32_t be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
   return (uint16_t)((p[0] << 8) | p[1]);
}

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* the whole file into buf; its size, -1 for none */
static int64_t read_file(const char *path, uint8_t *buf, int64_t cap)
{
   int64_t n;
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!f)
      return -1;
   n = filestream_read(f, buf, cap);
   filestream_close(f);
   return n;
}

static void write_file(const char *path, const uint8_t *data, int64_t size)
{
   CHECK(filestream_write_file(path, data, size));
}

static uint8_t s_buf[8 + 18250 * 16 + 64];

/* the minutes of the entry at index i in the file */
static uint32_t file_minutes(int i)
{
   if (read_file(STATS, s_buf, sizeof(s_buf)) != STATS_SIZE)
      return 0xFFFFFFFF;
   return be32(s_buf + 4 + i * 20 + 8);
}

static void test_created(void)
{
   uint32_t i;
   bool zero = true;
   host_remove_all(DIR_);
   /* the folder too: the statistics were not made the first time, when
    * only the diary made it */
   CHECK(pdm_load(DIR_) == (PDM_STATS_CREATED | PDM_DIARY_CREATED));
   CHECK(path_is_directory(DIR_));
   pdm_close();
   CHECK(read_file(STATS, s_buf, sizeof(s_buf)) == STATS_SIZE);
   for (i = 0; i < STATS_SIZE; i++)
      if (s_buf[i])
         zero = false;
   CHECK(zero);
   CHECK(read_file(DIARY, s_buf, sizeof(s_buf)) == 8);
   CHECK(be32(s_buf) == 0 && be32(s_buf + 4) == 0);
   /* made once: well-formed now */
   CHECK(pdm_load(DIR_) == 0);
   pdm_unload();
}

static void wait_minutes(int i, uint32_t want)
{
   const retro_time_t deadline = cpu_features_get_time_usec() + 10000000;
   while (file_minutes(i) != want && cpu_features_get_time_usec() < deadline)
      ;
   CHECK(file_minutes(i) == want);
}

static void test_tracking(void)
{
   uint32_t minutes = 0;
   uint16_t launches = 0, first = 0, last = 0;
   const uint8_t *e;
   set_clock(1000000, 1000);
   pdm_unload();
   CHECK(pdm_load(DIR_) == 0);
   CHECK(pdm_tracking_start(TITLE, 1000));
   /* one counter at a time */
   CHECK(!pdm_tracking_start(TITLE, 1000));
   /* the entry and the count written as soon as it begins */
   CHECK(read_file(STATS, s_buf, sizeof(s_buf)) == STATS_SIZE);
   CHECK(be32(s_buf) == 1);
   e = s_buf + 4;
   CHECK(be32(e) == 0x00050000 && be32(e + 4) == 0x10101D00);
   CHECK(be32(e + 8) == 0 && be16(e + 12) == 1 && be16(e + 14) == 1000 && be16(e + 16) == 1000);
   /* counted while it runs */
   set_clock(1000000 + 2 * (int64_t)MIN_US + 5, 1000);
   wait_minutes(0, 2);
   /* and what is left when it stops, on a new day */
   set_clock(1000000 + 3 * (int64_t)MIN_US + MIN_US / 2, 1001);
   CHECK(pdm_tracking_stop() == 0);
   CHECK(file_minutes(0) == 3);
   CHECK(pdm_stats_get(TITLE, &minutes, &launches, &first, &last));
   CHECK(minutes == 3 && launches == 1 && first == 1000 && last == 1001);
   pdm_close();

   /* again: one more launch, the minutes kept, no new entry */
   set_clock(50, 1005);
   pdm_unload();
   CHECK(pdm_load(DIR_) == 0);
   CHECK(pdm_tracking_start(TITLE, 1000));
   CHECK(pdm_tracking_stop() == 0);
   pdm_close();
   CHECK(read_file(STATS, s_buf, sizeof(s_buf)) == STATS_SIZE);
   CHECK(be32(s_buf) == 1);
   CHECK(be32(s_buf + 4 + 8) == 3 && be16(s_buf + 4 + 12) == 2 && be16(s_buf + 4 + 14) == 1000 && be16(s_buf + 4 + 16) == 1005);
   CHECK(pdm_stats_count() == 1);
}

static void test_stop_at_once(void)
{
   retro_time_t t;
   pdm_set_clocks(NULL, NULL);
   pdm_unload();
   CHECK(pdm_load(DIR_) == 0);
   /* a wake a minute away does not hold the stop up */
   CHECK(pdm_tracking_start(TITLE, 60 * (int64_t)1000000));
   /* long enough for it to be waiting */
   t = cpu_features_get_time_usec() + 20000;
   while (cpu_features_get_time_usec() < t)
      ;
   t = cpu_features_get_time_usec();
   CHECK(pdm_tracking_stop() == 0);
   CHECK(cpu_features_get_time_usec() - t < 5000000);
   pdm_close();
   pdm_set_clocks(fake_now, fake_today);
}

static void test_full(void)
{
   int i;
   uint8_t *e;
   memset(s_buf, 0, STATS_SIZE);
   /* more than there can be: 256 read */
   put32(s_buf, 300);
   for (i = 0; i < 256; i++)
   {
      e = s_buf + 4 + i * 20;
      put32(e, 0x00050000);
      put32(e + 4, (uint32_t)(0x100 + i));
      put32(e + 8, i == 7 ? 1 : (uint32_t)(10 + i));
      put16(e + 12, 1);
   }
   write_file(STATS, s_buf, STATS_SIZE);
   set_clock(0, 2000);
   pdm_unload();
   CHECK(pdm_load(DIR_) == 0);
   CHECK(pdm_stats_count() == 256);
   CHECK(pdm_tracking_start(TITLE, 1000));
   CHECK(pdm_tracking_stop() == 0);
   pdm_close();
   CHECK(read_file(STATS, s_buf, sizeof(s_buf)) == STATS_SIZE);
   /* the count as the file had it: not rewritten */
   CHECK(be32(s_buf) == 300);
   e = s_buf + 4 + 7 * 20;
   CHECK(be32(e + 4) == 0x10101D00 && be32(e + 8) == 0 && be16(e + 14) == 2000);
   CHECK(be32(s_buf + 4 + 6 * 20 + 4) == 0x106 && be32(s_buf + 4 + 8 * 20 + 4) == 0x108);
   CHECK(pdm_stats_count() == 256);
}

static void test_malformed(void)
{
   static uint8_t odd[100];
   memset(odd, 0x5A, sizeof(odd));
   write_file(STATS, odd, sizeof(odd));
   pdm_unload();
   CHECK(pdm_load(DIR_) == PDM_STATS_MALFORMED);
   /* counted in memory, nothing written */
   CHECK(pdm_tracking_start(TITLE, 1000));
   CHECK(pdm_tracking_stop() == 0);
   CHECK(pdm_stats_get(TITLE, NULL, NULL, NULL, NULL));
   pdm_close();
   CHECK(read_file(STATS, s_buf, sizeof(s_buf)) == (int64_t)sizeof(odd));
   CHECK(memcmp(s_buf, odd, sizeof(odd)) == 0);
   CHECK(filestream_delete(STATS) == 0);
}

static void diary_file(uint32_t read_index, uint32_t write_index, uint32_t entries)
{
   uint32_t i;
   memset(s_buf, 0, sizeof(s_buf));
   put32(s_buf, read_index);
   put32(s_buf + 4, write_index);
   for (i = 0; i < entries; i++)
   {
      put32(s_buf + 8 + i * 16, 0x00050000);
      put32(s_buf + 8 + i * 16 + 4, i);
      put16(s_buf + 8 + i * 16 + 12, (uint16_t)(i + 1));
   }
   write_file(DIARY, s_buf, 8 + (int64_t)entries * 16);
}

static uint8_t s_out[18250 * 16];

static void test_diary(void)
{
   unsigned r;
   diary_file(5, 8, 20);
   pdm_unload();
   r = pdm_load(DIR_);
   CHECK((r & ~(unsigned)PDM_STATS_CREATED) == 0);
   CHECK(pdm_diary_get(s_out, 100) == 3);
   CHECK(be32(s_out + 4) == 5 && be32(s_out + 16 + 4) == 6 && be32(s_out + 32 + 4) == 7);
   CHECK(be16(s_out + 12) == 6);
   CHECK(pdm_diary_get(s_out, 2) == 2);
   CHECK(pdm_diary_get(s_out, 0) == 0);

   /* around the end of the ring; what the file lacks is zero */
   diary_file(18248, 2, 18250);
   pdm_unload();
   pdm_load(DIR_);
   CHECK(pdm_diary_get(s_out, 100) == 4);
   CHECK(be32(s_out + 4) == 18248 && be32(s_out + 16 + 4) == 18249);
   CHECK(be32(s_out + 32 + 4) == 0 && be32(s_out + 48 + 4) == 1);
   diary_file(0, 4, 2);
   pdm_unload();
   pdm_load(DIR_);
   CHECK(pdm_diary_get(s_out, 100) == 4);
   CHECK(be32(s_out + 16 + 4) == 1 && be32(s_out + 32) == 0 && be16(s_out + 48 + 12) == 0);

   /* an index one past the end, read past the entries before: wrapped */
   diary_file(18250, 3, 5);
   pdm_unload();
   r = pdm_load(DIR_);
   CHECK(r & PDM_DIARY_BAD_INDEX);
   CHECK(pdm_diary_get(s_out, 100) == 3);
   CHECK(be32(s_out + 4) == 0 && be16(s_out + 12) == 1);
   diary_file(18251, 18252, 5);
   pdm_unload();
   r = pdm_load(DIR_);
   CHECK(r & PDM_DIARY_BAD_INDEX);
   CHECK(pdm_diary_get(s_out, 100) == 1);
   CHECK(be32(s_out + 4) == 1);

   /* the write index one past the end: the ring up to its end */
   diary_file(3, 18250, 5);
   pdm_unload();
   r = pdm_load(DIR_);
   CHECK(r & PDM_DIARY_BAD_INDEX);
   CHECK(pdm_diary_get(s_out, 18250) == 18247);

   /* a header that cannot be read: made anew */
   write_file(DIARY, (const uint8_t*)"abc", 3);
   pdm_unload();
   r = pdm_load(DIR_);
   CHECK((r & (PDM_DIARY_BAD_HEADER | PDM_DIARY_CREATED)) == (PDM_DIARY_BAD_HEADER | PDM_DIARY_CREATED));
   CHECK(pdm_diary_get(s_out, 100) == 0);
   pdm_close();
   CHECK(read_file(DIARY, s_buf, sizeof(s_buf)) == 8);
   CHECK(be32(s_buf) == 0 && be32(s_buf + 4) == 0);

   /* forgotten on unload */
   diary_file(0, 2, 2);
   pdm_unload();
   pdm_load(DIR_);
   pdm_unload();
   CHECK(pdm_diary_get(s_out, 100) == 0);
   CHECK(pdm_stats_count() == 0);
}

static void test_unavailable(void)
{
   /* a folder where the files would be */
   host_remove_all(DIR_);
   CHECK(path_mkdir(STATS));
   CHECK(path_mkdir(DIARY));
   CHECK(pdm_load(DIR_) == (PDM_STATS_UNAVAILABLE | PDM_DIARY_UNAVAILABLE));
   CHECK(pdm_tracking_start(TITLE, 1000));
   CHECK(pdm_tracking_stop() == 0);
   pdm_unload();
}

static void test_case(void)
{
   /* the files as some other tool named them */
   host_remove_all(DIR_);
   CHECK(path_mkdir(DIR_));
   memset(s_buf, 0, STATS_SIZE);
   put32(s_buf, 1);
   put32(s_buf + 4 + 4, 0x42);
   write_file(DIR_ "/playstats.DAT", s_buf, STATS_SIZE);
   diary_file(0, 1, 1);
   CHECK(filestream_rename(DIARY, DIR_ "/PLAYDIARY.dat") == 0);
   CHECK(pdm_load(DIR_) == 0);
   CHECK(pdm_stats_count() == 1 && pdm_stats_get(0x42, NULL, NULL, NULL, NULL));
   CHECK(pdm_diary_get(s_out, 10) == 1);
   pdm_unload();
}

static void test_today(void)
{
   const long before = (long)(time(NULL) / 86400) - 10957;
   const uint16_t got = pdm_today();
   const long after = (long)(time(NULL) / 86400) - 10957;
   /* a day may turn between them */
   CHECK(got == (uint16_t)before || got == (uint16_t)after);
   /* 2026: day 9000 and more */
   CHECK(got > 9000);
}

int main(void)
{
   test_today();
   pdm_set_clocks(fake_now, fake_today);
   test_created();
   test_tracking();
   test_stop_at_once();
   test_full();
   test_malformed();
   test_diary();
   test_unavailable();
   test_case();
   pdm_set_clocks(NULL, NULL);
   host_remove_all(DIR_);
   if (failures)
   {
      fprintf(stderr, "pdm_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("pdm_test: all passed\n");
   return 0;
}
