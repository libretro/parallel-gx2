#ifndef LIBRETRO_MIC_H
#define LIBRETRO_MIC_H

/* The GamePad microphone from the frontend's (the libretro microphone
 * interface). retro_run reads what the frontend captured each frame,
 * resamples it to the GamePad's 32 kHz and pushes it into a ring; the AX
 * thread takes 96 samples (3 ms) a frame from it for the title. No lock:
 * one writer, one reader. The frontend's microphone is opened the first
 * time a title opens the GamePad's, and is on only while one has it open.
 * C89. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <libretro.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define LIBRETRO_MIC_RATE 32000

/* At content load, on retro_run's thread. iface NULL or without read_mic:
 * no microphone, and nothing else here does anything. */
bool libretro_mic_init(const struct retro_microphone_interface *iface);
/* At unload, on retro_run's thread: the microphone closed, the ring freed */
void libretro_mic_deinit(void);

/* From the title's threads: whether it has the microphone open */
void libretro_mic_want(bool want);

/* From retro_run, once a frame, at fps frames a second: the microphone
 * turned on or off as the title wants it, and what it captured pushed */
void libretro_mic_run(double fps);

/* From the AX thread: whether captured samples come from the frontend, and
 * up to n of them (oldest first, behind by no more than about 70 ms);
 * returns how many it gave */
bool libretro_mic_live(void);
size_t libretro_mic_take(int16_t *out, size_t n);

RETRO_END_DECLS

#endif
