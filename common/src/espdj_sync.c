#include "espdj_sync.h"

#include <math.h>

void espdj_sync_init(espdj_sync_t *s)
{
    s->valid = false;
    s->ref_time_us = 0;
    s->ref_beat = 0.0;
    s->master_bps = 2.0; /* 120 BPM placeholder until first report */
    s->trim = 0.0f;
    /* Tuned for ~critical damping with a 100 Hz update loop at ~120 BPM:
     * closed loop err'' = -bps*kp*err' - bps*ki*rate*err. The ±0.02%
     * authority makes phase pull-in inherently gentle (that's the point —
     * inaudible); big offsets are handled by re-anchoring, not the loop. */
    s->kp = 0.004f;   /* saturates authority beyond ~0.05 beat of error */
    s->ki = 1.3e-7f;  /* integral removes steady clock-rate offset */
    s->integ = 0.0f;
}

void espdj_sync_on_report(espdj_sync_t *s, uint64_t local_now_us,
                          uint32_t beat_number, uint16_t phase_q16,
                          uint16_t bpm_q8, uint32_t latency_us)
{
    if (bpm_q8 == 0) {
        return;
    }
    double bpm = (double)bpm_q8 / 256.0;
    double beat = (double)beat_number + (double)phase_q16 / 65536.0;
    /* The report describes the master `latency_us` ago in local terms. */
    s->ref_time_us = local_now_us - latency_us;
    s->ref_beat = beat;
    s->master_bps = bpm / 60.0;
    s->valid = true;
}

double espdj_sync_master_beat_at(const espdj_sync_t *s, uint64_t t_us)
{
    if (!s->valid) {
        return 0.0;
    }
    double dt = ((double)(int64_t)(t_us - s->ref_time_us)) / 1e6;
    return s->ref_beat + dt * s->master_bps;
}

double espdj_sync_phase_error(const espdj_sync_t *s, uint64_t t_us,
                              double local_beat)
{
    if (!s->valid) {
        return 0.0;
    }
    double err = espdj_sync_master_beat_at(s, t_us) - local_beat;
    err -= floor(err);        /* wrap to [0, 1) */
    if (err > 0.5) err -= 1.0; /* -> (-0.5, 0.5] */
    return err;
}

float espdj_sync_update(espdj_sync_t *s, uint64_t t_us, double local_beat)
{
    if (!s->valid) {
        s->trim = 0.0f;
        return 0.0f;
    }
    double err = espdj_sync_phase_error(s, t_us, local_beat);

    s->integ += (float)err;
    /* anti-windup: keep the integral's contribution inside authority */
    float imax = ESPDJ_SYNC_MAX_TRIM / (s->ki > 0.0f ? s->ki : 1.0f);
    if (s->integ > imax) s->integ = imax;
    if (s->integ < -imax) s->integ = -imax;

    float trim = s->kp * (float)err + s->ki * s->integ;
    if (trim > ESPDJ_SYNC_MAX_TRIM) trim = ESPDJ_SYNC_MAX_TRIM;
    if (trim < -ESPDJ_SYNC_MAX_TRIM) trim = -ESPDJ_SYNC_MAX_TRIM;
    s->trim = trim;
    return trim;
}
