/*
 * ESP-DJ3000 — cubic (Catmull-Rom) varispeed resampler.
 *
 * This is the heart of the deck's RT path:
 *   SD -> PSRAM ring -> cubic resampler -> gain/EQ/filter -> I2S DMA
 *
 * The ratio is a *signed* playback rate: +1.0 is normal speed, 0.5 is half
 * speed, negative values play backwards (scratching pulls the ratio straight
 * from jog-ring velocity). Varispeed = Vinyl Mode: pitch and tempo move
 * together, no timestretch in v1 by design.
 *
 * Portable C99, fixed-point free on purpose: the S3 has an FPU and a block
 * of 128 stereo frames costs well under 1% of a core at 44.1 kHz.
 */
#ifndef ESPDJ_RESAMPLER_H
#define ESPDJ_RESAMPLER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double position;   /* fractional source frame index */
    float  ratio;      /* signed playback rate */
    float  target_ratio;
    float  slew_per_frame; /* max ratio change per output frame (declick) */
} espdj_resampler_t;

void espdj_resampler_init(espdj_resampler_t *r);

/* Set the playback rate. `immediate` jumps without slewing (scratch);
 * otherwise the rate glides at slew_per_frame per output frame (fader). */
void espdj_resampler_set_ratio(espdj_resampler_t *r, float ratio, bool immediate);

/* Resample stereo interleaved int16.
 *
 * src has src_frames frames available starting at source index 0; the
 * resampler's `position` indexes into it. Produces up to dst_frames output
 * frames into dst; stops early if it would read outside [1, src_frames-3]
 * (one frame of history and two of lookahead for the cubic kernel).
 *
 * Returns frames actually produced. `consumed_out` (may be NULL) receives
 * the integer number of source frames the caller can discard from the
 * front of its buffer; the resampler rebases `position` accordingly.
 * With a negative ratio nothing is consumed (caller keeps the window). */
size_t espdj_resample_stereo_i16(espdj_resampler_t *r,
                                 const int16_t *src, size_t src_frames,
                                 int16_t *dst, size_t dst_frames,
                                 size_t *consumed_out);

#ifdef __cplusplus
}
#endif

#endif /* ESPDJ_RESAMPLER_H */
