#include "espdj_mix.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void espdj_biquad_bypass(espdj_biquad_t *q)
{
    q->b0 = 1.0f; q->b1 = q->b2 = q->a1 = q->a2 = 0.0f;
    q->z1l = q->z2l = q->z1r = q->z2r = 0.0f;
}

static void normalize(espdj_biquad_t *q, float b0, float b1, float b2,
                      float a0, float a1, float a2)
{
    q->b0 = b0 / a0;
    q->b1 = b1 / a0;
    q->b2 = b2 / a0;
    q->a1 = a1 / a0;
    q->a2 = a2 / a0;
}

/* RBJ audio EQ cookbook forms. */

void espdj_biquad_lowshelf(espdj_biquad_t *q, float fs, float f0, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w = 2.0f * (float)M_PI * f0 / fs;
    float cw = cosf(w), sw = sinf(w);
    float S = 1.0f; /* shelf slope */
    float alpha = sw / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    float twoRootAalpha = 2.0f * sqrtf(A) * alpha;
    normalize(q,
        A * ((A + 1) - (A - 1) * cw + twoRootAalpha),
        2 * A * ((A - 1) - (A + 1) * cw),
        A * ((A + 1) - (A - 1) * cw - twoRootAalpha),
        (A + 1) + (A - 1) * cw + twoRootAalpha,
        -2 * ((A - 1) + (A + 1) * cw),
        (A + 1) + (A - 1) * cw - twoRootAalpha);
}

void espdj_biquad_highshelf(espdj_biquad_t *q, float fs, float f0, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w = 2.0f * (float)M_PI * f0 / fs;
    float cw = cosf(w), sw = sinf(w);
    float S = 1.0f;
    float alpha = sw / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    float twoRootAalpha = 2.0f * sqrtf(A) * alpha;
    normalize(q,
        A * ((A + 1) + (A - 1) * cw + twoRootAalpha),
        -2 * A * ((A - 1) + (A + 1) * cw),
        A * ((A + 1) + (A - 1) * cw - twoRootAalpha),
        (A + 1) - (A - 1) * cw + twoRootAalpha,
        2 * ((A - 1) - (A + 1) * cw),
        (A + 1) - (A - 1) * cw - twoRootAalpha);
}

void espdj_biquad_peaking(espdj_biquad_t *q, float fs, float f0, float gain_db, float bw_q)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w = 2.0f * (float)M_PI * f0 / fs;
    float alpha = sinf(w) / (2.0f * bw_q);
    normalize(q,
        1 + alpha * A, -2 * cosf(w), 1 - alpha * A,
        1 + alpha / A, -2 * cosf(w), 1 - alpha / A);
}

void espdj_biquad_lowpass(espdj_biquad_t *q, float fs, float f0, float res_q)
{
    float w = 2.0f * (float)M_PI * f0 / fs;
    float cw = cosf(w);
    float alpha = sinf(w) / (2.0f * res_q);
    normalize(q,
        (1 - cw) / 2, 1 - cw, (1 - cw) / 2,
        1 + alpha, -2 * cw, 1 - alpha);
}

void espdj_biquad_highpass(espdj_biquad_t *q, float fs, float f0, float res_q)
{
    float w = 2.0f * (float)M_PI * f0 / fs;
    float cw = cosf(w);
    float alpha = sinf(w) / (2.0f * res_q);
    normalize(q,
        (1 + cw) / 2, -(1 + cw), (1 + cw) / 2,
        1 + alpha, -2 * cw, 1 - alpha);
}

void espdj_biquad_process_i16(espdj_biquad_t *q, int16_t *buf, size_t frames)
{
    float b0 = q->b0, b1 = q->b1, b2 = q->b2, a1 = q->a1, a2 = q->a2;
    float z1l = q->z1l, z2l = q->z2l, z1r = q->z1r, z2r = q->z2r;
    for (size_t i = 0; i < frames; i++) {
        float xl = (float)buf[i * 2];
        float yl = b0 * xl + z1l;
        z1l = b1 * xl - a1 * yl + z2l;
        z2l = b2 * xl - a2 * yl;
        float xr = (float)buf[i * 2 + 1];
        float yr = b0 * xr + z1r;
        z1r = b1 * xr - a1 * yr + z2r;
        z2r = b2 * xr - a2 * yr;
        if (yl > 32767.0f) yl = 32767.0f; else if (yl < -32768.0f) yl = -32768.0f;
        if (yr > 32767.0f) yr = 32767.0f; else if (yr < -32768.0f) yr = -32768.0f;
        buf[i * 2]     = (int16_t)lrintf(yl);
        buf[i * 2 + 1] = (int16_t)lrintf(yr);
    }
    q->z1l = z1l; q->z2l = z2l; q->z1r = z1r; q->z2r = z2r;
}

void espdj_channel_init(espdj_channel_t *ch, float fs)
{
    ch->fs = fs;
    ch->trim = 1.0f;
    ch->xfader_gain = 1.0f;
    ch->low_db = ch->mid_db = ch->high_db = 0.0f;
    ch->filter_pos = 0.0f;
    ch->filter_active = false;
    espdj_biquad_bypass(&ch->eq_low);
    espdj_biquad_bypass(&ch->eq_mid);
    espdj_biquad_bypass(&ch->eq_high);
    espdj_biquad_bypass(&ch->filter);
}

void espdj_channel_set_eq(espdj_channel_t *ch, float low_db, float mid_db, float high_db)
{
    ch->low_db = low_db;
    ch->mid_db = mid_db;
    ch->high_db = high_db;
    espdj_biquad_lowshelf(&ch->eq_low, ch->fs, 200.0f, low_db);
    espdj_biquad_peaking(&ch->eq_mid, ch->fs, 1200.0f, mid_db, 0.9f);
    espdj_biquad_highshelf(&ch->eq_high, ch->fs, 6000.0f, high_db);
}

void espdj_channel_set_filter(espdj_channel_t *ch, float pos)
{
    if (pos > 1.0f) pos = 1.0f;
    if (pos < -1.0f) pos = -1.0f;
    ch->filter_pos = pos;
    if (fabsf(pos) < 0.03f) {
        ch->filter_active = false;
        espdj_biquad_bypass(&ch->filter);
        return;
    }
    ch->filter_active = true;
    float res = 1.2f; /* gentle resonance, DJ-filter flavor */
    if (pos < 0.0f) {
        /* LP sweeps 20 kHz down to ~80 Hz, log taper */
        float f = 20000.0f * powf(80.0f / 20000.0f, -pos);
        espdj_biquad_lowpass(&ch->filter, ch->fs, f, res);
    } else {
        /* HP sweeps 20 Hz up to ~8 kHz, log taper */
        float f = 20.0f * powf(8000.0f / 20.0f, pos);
        espdj_biquad_highpass(&ch->filter, ch->fs, f, res);
    }
}

void espdj_channel_process_i16(espdj_channel_t *ch, int16_t *buf, size_t frames)
{
    if (ch->low_db != 0.0f)  espdj_biquad_process_i16(&ch->eq_low, buf, frames);
    if (ch->mid_db != 0.0f)  espdj_biquad_process_i16(&ch->eq_mid, buf, frames);
    if (ch->high_db != 0.0f) espdj_biquad_process_i16(&ch->eq_high, buf, frames);
    if (ch->filter_active)   espdj_biquad_process_i16(&ch->filter, buf, frames);

    float g = ch->trim * ch->xfader_gain;
    if (g != 1.0f) {
        for (size_t i = 0; i < frames * 2; i++) {
            float v = (float)buf[i] * g;
            if (v > 32767.0f) v = 32767.0f; else if (v < -32768.0f) v = -32768.0f;
            buf[i] = (int16_t)lrintf(v);
        }
    }
}

void espdj_xfader_gains(float position, float curve, float *gain_a, float *gain_b)
{
    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;
    if (curve < 0.0f) curve = 0.0f;
    if (curve > 1.0f) curve = 1.0f;

    /* Constant-power base law. */
    float ga = cosf(position * (float)M_PI * 0.5f);
    float gb = sinf(position * (float)M_PI * 0.5f);

    /* Curve setting morphs toward a scratch cut: full gain except in a
     * narrow cut zone at each extreme. */
    if (curve > 0.0f) {
        float zone = 0.5f - 0.45f * curve; /* cut zone shrinks with curve */
        float sa = (position >= 1.0f - zone)
                       ? cosf(((position - (1.0f - zone)) / zone) * (float)M_PI * 0.5f)
                       : 1.0f;
        float sb = (position <= zone)
                       ? sinf((position / zone) * (float)M_PI * 0.5f)
                       : 1.0f;
        ga = ga + (sa - ga) * curve;
        gb = gb + (sb - gb) * curve;
    }
    *gain_a = ga;
    *gain_b = gb;
}
