/*
 * ESP-DJ3000 — per-deck gain / 3-band EQ / DJ filter / crossfader law.
 *
 * The crossfader is a *control signal*: the controller broadcasts its
 * position and each deck computes and applies its own gain before summing
 * onto the shared analog AUDIO bus (design doc §0 #1, §5). Constant-power
 * by default, with a curve setting toward a sharp scratch cut.
 *
 * EQ: RBJ biquads — low shelf @ 200 Hz, peaking @ 1.2 kHz, high shelf
 * @ 6 kHz. Filter: one knob sweeping resonant HP (right of center) or LP
 * (left of center), the classic DJ filter. All portable C99 float DSP.
 */
#ifndef ESPDJ_MIX_H
#define ESPDJ_MIX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float b0, b1, b2, a1, a2; /* normalized (a0 = 1) */
    float z1l, z2l, z1r, z2r; /* transposed direct form II state, L and R */
} espdj_biquad_t;

void espdj_biquad_bypass(espdj_biquad_t *q);
void espdj_biquad_lowshelf(espdj_biquad_t *q, float fs, float f0, float gain_db);
void espdj_biquad_highshelf(espdj_biquad_t *q, float fs, float f0, float gain_db);
void espdj_biquad_peaking(espdj_biquad_t *q, float fs, float f0, float gain_db, float bw_q);
void espdj_biquad_lowpass(espdj_biquad_t *q, float fs, float f0, float res_q);
void espdj_biquad_highpass(espdj_biquad_t *q, float fs, float f0, float res_q);

/* Process interleaved stereo int16 in place. */
void espdj_biquad_process_i16(espdj_biquad_t *q, int16_t *buf, size_t frames);

typedef struct {
    float fs;
    float trim;            /* linear channel gain (deck volume) */
    float xfader_gain;     /* from espdj_xfader_gains() */
    float low_db, mid_db, high_db;
    float filter_pos;      /* -1..+1, 0 = off */
    espdj_biquad_t eq_low, eq_mid, eq_high, filter;
    bool filter_active;
} espdj_channel_t;

void espdj_channel_init(espdj_channel_t *ch, float fs);
void espdj_channel_set_eq(espdj_channel_t *ch, float low_db, float mid_db, float high_db);
/* pos in [-1, 1]: negative sweeps a resonant LP down from 20 kHz,
 * positive sweeps a resonant HP up from 20 Hz, ~0 (|pos|<0.03) bypasses. */
void espdj_channel_set_filter(espdj_channel_t *ch, float pos);
void espdj_channel_process_i16(espdj_channel_t *ch, int16_t *buf, size_t frames);

/* Crossfader law. position 0..1 (0 = full A), curve 0..1
 * (0 = constant power blend, 1 = sharp scratch cut). */
void espdj_xfader_gains(float position, float curve, float *gain_a, float *gain_b);

#ifdef __cplusplus
}
#endif

#endif /* ESPDJ_MIX_H */
