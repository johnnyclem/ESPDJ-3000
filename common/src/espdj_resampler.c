#include "espdj_resampler.h"

#include <math.h>

void espdj_resampler_init(espdj_resampler_t *r)
{
    r->position = 1.0; /* leave one frame of history for the cubic kernel */
    r->ratio = 1.0f;
    r->target_ratio = 1.0f;
    r->slew_per_frame = 0.00005f; /* ~2.2 rate/s at 44.1k: click-free glide */
}

void espdj_resampler_set_ratio(espdj_resampler_t *r, float ratio, bool immediate)
{
    r->target_ratio = ratio;
    if (immediate) {
        r->ratio = ratio;
    }
}

static inline float catmull_rom(float ym1, float y0, float y1, float y2, float t)
{
    float a = -0.5f * ym1 + 1.5f * y0 - 1.5f * y1 + 0.5f * y2;
    float b = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c = 0.5f * (y1 - ym1);
    return ((a * t + b) * t + c) * t + y0;
}

static inline int16_t clamp_i16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

size_t espdj_resample_stereo_i16(espdj_resampler_t *r,
                                 const int16_t *src, size_t src_frames,
                                 int16_t *dst, size_t dst_frames,
                                 size_t *consumed_out)
{
    size_t produced = 0;
    if (src_frames < 4) {
        if (consumed_out) *consumed_out = 0;
        return 0;
    }
    const double lo = 1.0;
    const double hi = (double)src_frames - 3.0;

    while (produced < dst_frames) {
        /* Slew the rate toward its target once per output frame. */
        float d = r->target_ratio - r->ratio;
        if (d > r->slew_per_frame)       r->ratio += r->slew_per_frame;
        else if (d < -r->slew_per_frame) r->ratio -= r->slew_per_frame;
        else                             r->ratio = r->target_ratio;

        if (r->position < lo || r->position > hi) {
            break; /* out of window: caller refills (or we're at a stop) */
        }
        size_t i = (size_t)r->position;
        float t = (float)(r->position - (double)i);
        const int16_t *p = &src[(i - 1) * 2];
        dst[produced * 2] = clamp_i16(
            catmull_rom((float)p[0], (float)p[2], (float)p[4], (float)p[6], t));
        dst[produced * 2 + 1] = clamp_i16(
            catmull_rom((float)p[1], (float)p[3], (float)p[5], (float)p[7], t));
        produced++;
        r->position += (double)r->ratio;
    }

    /* Tell the caller how many leading source frames are dead weight.
     * Keep one frame of history behind the play position. Only meaningful
     * for forward playback; while scratching backwards we hold the window. */
    size_t consumed = 0;
    if (r->ratio >= 0.0f && r->position > 1.0) {
        consumed = (size_t)(r->position - 1.0);
        if (consumed > src_frames) consumed = src_frames;
        r->position -= (double)consumed;
    }
    if (consumed_out) *consumed_out = consumed;
    return produced;
}
