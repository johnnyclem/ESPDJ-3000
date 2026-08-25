/*
 * ESP-DJ3000 — Link-style beat sync (design doc §2 "Beat sync").
 *
 * The sync master broadcasts a timestamped beat phase at 10 Hz. Slaves
 * project the master's phase forward to "now" using the message timestamp
 * (transports carry their own latency; the timestamp removes it), compare
 * against their local playhead phase, and trim their resample ratio by at
 * most ±0.02% to converge — clock discipline, not hard snapping.
 *
 * A large error (> half a beat, e.g. right after pressing SYNC) is handled
 * by phase-jumping the *reference* rather than the audio: the caller is
 * told the beat offset so it can beat-jump or simply re-anchor.
 */
#ifndef ESPDJ_SYNC_H
#define ESPDJ_SYNC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDJ_SYNC_MAX_TRIM 0.0002f /* ±0.02% resample-ratio authority */

typedef struct {
    /* last master report */
    bool     valid;
    uint64_t ref_time_us;   /* local clock when the report was accepted */
    double   ref_beat;      /* master beat (integer + phase) at ref_time */
    double   master_bps;    /* master beats per second */
    /* discipline state */
    float    trim;          /* current ratio trim, clamped to MAX_TRIM */
    float    kp;            /* proportional gain, trim per beat of error */
    float    ki;            /* integral gain */
    float    integ;
} espdj_sync_t;

void espdj_sync_init(espdj_sync_t *s);

/* Feed a master beat-phase report. `local_now_us` is the receiver's clock
 * at message arrival; `latency_us` is the transport's known one-way latency
 * (ESP-NOW ~2-5 ms, wired bus ~0). */
void espdj_sync_on_report(espdj_sync_t *s, uint64_t local_now_us,
                          uint32_t beat_number, uint16_t phase_q16,
                          uint16_t bpm_q8, uint32_t latency_us);

/* Master beat (fractional) projected to local time t. */
double espdj_sync_master_beat_at(const espdj_sync_t *s, uint64_t t_us);

/* Update the discipline loop with the local playhead's fractional beat at
 * local time t. Returns the resample-ratio trim to add to the deck's
 * nominal ratio (1.0 + pitch). Phase is compared modulo 1 beat so decks
 * lock to the *beat*, not to absolute track position. */
float espdj_sync_update(espdj_sync_t *s, uint64_t t_us, double local_beat);

/* Current phase error in beats, wrapped to (-0.5, 0.5]; 0 if no report. */
double espdj_sync_phase_error(const espdj_sync_t *s, uint64_t t_us,
                              double local_beat);

#ifdef __cplusplus
}
#endif

#endif /* ESPDJ_SYNC_H */
