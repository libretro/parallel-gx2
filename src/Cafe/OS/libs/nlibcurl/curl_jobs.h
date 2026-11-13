#ifndef CAFE_OS_LIBS_NLIBCURL_CURL_JOBS_H
#define CAFE_OS_LIBS_NLIBCURL_CURL_JOBS_H

/* The host threads nlibcurl's transfers run on. C89.
 *
 * A title's curl_easy_perform blocks its thread while the transfer runs on
 * one of CURL_JOBS_WORKERS host threads, which hands each of the title's
 * callbacks (a header, data written, data to send) back to that thread and
 * waits for the answer. The workers live as long as the core; a title
 * stopping retires its jobs, after which a worker hands nothing more to
 * it and a callback it waits on is answered 0.
 *
 * No lock and nothing allocated: a job is one of CURL_JOBS_SLOTS fixed
 * slots, held by its caller and by the worker until each lets go (a
 * retired caller lets go of itself), queued by index in a bounded queue
 * (D. Vyukov's); a worker waits for the queue and for an answer on one
 * eventcount. */

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define CURL_JOBS_WORKERS 8
#define CURL_JOBS_SLOTS 32

/* run(slot) on a worker; the slot is let go of by the worker after it */
bool curl_jobs_start(void (*run)(int slot));
/* the core going: every job retired, unblock() called to end the
 * transfers the workers are in, the workers joined */
void curl_jobs_stop(void (*unblock)(void));

/* a slot for the caller to fill, or -1 when none is free */
int curl_jobs_claim(void);
/* to a worker; false when there are none running or the queue is full,
 * and the caller runs it itself - the slot is the caller's alone then */
bool curl_jobs_queue(int slot);
/* the caller done with it */
void curl_jobs_release(int slot);

/* every job there is now retired: for a title stopping, while none of its
 * threads run */
void curl_jobs_retire(void);
/* not retired: the caller is still there */
bool curl_jobs_live(int slot);

/* the caller's answer to the callback the job's worker waits on */
void curl_jobs_answer(int slot, uint32_t answer);
/* the worker's side: waits for it; 0 once the job is retired */
uint32_t curl_jobs_await(int slot);

RETRO_END_DECLS

#endif
