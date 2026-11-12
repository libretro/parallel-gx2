#ifndef CAFE_IOSU_PDM_STORE_H
#define CAFE_IOSU_PDM_STORE_H

/* The play statistics and play diary the console keeps (PlayStats.dat,
 * PlayDiary.dat in the MLC's pdm folder) and the thread that counts the
 * minutes a title is played. C89.
 *
 * No locks: what changes the data runs at one time only. pdm_load and
 * pdm_unload run while no title does; pdm_tracking_start's own writes
 * happen before its thread starts, the thread is then the only writer, and
 * pdm_tracking_stop joins it. pdm_diary_get, which a title calls, only
 * reads, and nothing writes the diary while a title runs. */

#include <stdint.h>
#include <stddef.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define PDM_PLAY_STATS_ENTRIES 256
#define PDM_PLAY_DIARY_ENTRIES 18250

/* as the files and the console have them: big-endian */
#define PDM_PLAY_STATS_ENTRY_SIZE 20
#define PDM_PLAY_DIARY_ENTRY_SIZE 16

/* what pdm_load found, for the caller to log */
#define PDM_STATS_UNAVAILABLE  1 /* not opened or created: no tracking */
#define PDM_STATS_MALFORMED    2 /* not the size it is: left alone, no tracking */
#define PDM_STATS_CREATED      4
#define PDM_DIARY_UNAVAILABLE  8
#define PDM_DIARY_BAD_HEADER  16 /* unreadable: made anew */
#define PDM_DIARY_BAD_INDEX   32 /* a read or write index past the end: wrapped */
#define PDM_DIARY_CREATED     64

/* The files in dir (made if it is not there), read and left open, the
 * statistics where they are well-formed and created where they are
 * missing. A combination of the PDM_* flags above. */
unsigned pdm_load(const char *dir);
/* the files closed; what was read stays */
void pdm_close(void);
/* what was read forgotten; the files closed first */
void pdm_unload(void);

/* Counting begun for a title: its entry's launches and last day where it
 * has one, else a new one, the title least played replaced once all are
 * taken. Then a thread that every wake_us adds the minutes played since,
 * until pdm_tracking_stop. */
bool pdm_tracking_start(uint64_t title_id, int64_t wake_us);
/* the thread stopped, the whole minutes not yet counted counted. The
 * number of writes that failed since the last call. */
unsigned pdm_tracking_stop(void);

/* Up to max diary entries, oldest first, PDM_PLAY_DIARY_ENTRY_SIZE bytes
 * each into out; how many */
uint32_t pdm_diary_get(void *out, uint32_t max);

/* The statistics of a title, from what was read and counted, as host
 * numbers; false where it has no entry. Any pointer may be NULL. */
bool pdm_stats_get(uint64_t title_id, uint32_t *minutes, uint16_t *launches,
      uint16_t *first_day, uint16_t *last_day);
uint32_t pdm_stats_count(void);

/* Days since 2000-01-01 (UTC), as the files count them */
uint16_t pdm_today(void);

/* For tests: the clocks the counting reads, NULL for the real ones: a
 * monotonic one in microseconds and the day pdm_today gives. */
void pdm_set_clocks(int64_t (*now_us)(void), uint16_t (*today)(void));

RETRO_END_DECLS

#endif
