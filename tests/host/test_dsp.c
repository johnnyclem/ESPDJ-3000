#include "test_util.h"
#include "espdj_resampler.h"
#include "espdj_ringbuf.h"
#include "espdj_mix.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS 44100.0

/* Fill interleaved stereo with a sine at `freq` on both channels. */
static void gen_sine(int16_t *buf, size_t frames, double freq, double fs)
{
    for (size_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(20000.0 * sin(2.0 * M_PI * freq * (double)i / fs));
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }
}

/* Count zero crossings on the left channel to estimate frequency. */
static double est_freq(const int16_t *buf, size_t frames, double fs)
{
    int crossings = 0;
    for (size_t i = 1; i < frames; i++) {
        if ((buf[(i - 1) * 2] < 0) != (buf[i * 2] < 0)) crossings++;
    }
    return (double)crossings * fs / (2.0 * (double)frames);
}

static void test_resampler_unity(void)
{
    enum { N = 4096 };
    static int16_t src[N * 2], dst[N * 2];
    gen_sine(src, N, 1000.0, FS);

    espdj_resampler_t r;
    espdj_resampler_init(&r);
    size_t consumed = 0;
    size_t got = espdj_resample_stereo_i16(&r, src, N, dst, N - 8, &consumed);
    CHECK(got == N - 8);
    CHECK(consumed > 0);
    /* At ratio 1.0 the output is (nearly) a delayed copy. */
    int maxerr = 0;
    for (size_t i = 4; i < got - 4; i++) {
        int e = abs((int)dst[i * 2] - (int)src[(i + 1) * 2]);
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr <= 2); /* cubic through the sample points + rounding */
}

static void test_resampler_pitch(void)
{
    enum { N = 32768 };
    static int16_t src[N * 2], dst[N * 2];
    gen_sine(src, N, 1000.0, FS);

    /* +8% varispeed should shift a 1 kHz tone to ~1.08 kHz. */
    espdj_resampler_t r;
    espdj_resampler_init(&r);
    espdj_resampler_set_ratio(&r, 1.08f, true);
    size_t got = espdj_resample_stereo_i16(&r, src, N, dst, N, NULL);
    CHECK(got > 20000);
    CHECK_NEAR(est_freq(dst, got, FS), 1080.0, 15.0);

    /* Half speed: one octave down. */
    espdj_resampler_init(&r);
    espdj_resampler_set_ratio(&r, 0.5f, true);
    got = espdj_resample_stereo_i16(&r, src, N, dst, N, NULL);
    CHECK(got == N); /* plenty of source for N outputs at half rate */
    CHECK_NEAR(est_freq(dst, got, FS), 500.0, 10.0);
}

static void test_resampler_reverse(void)
{
    enum { N = 8192 };
    static int16_t src[N * 2], dst[N * 2];
    gen_sine(src, N, 500.0, FS);

    espdj_resampler_t r;
    espdj_resampler_init(&r);
    r.position = (double)N - 4.0; /* start near the end */
    espdj_resampler_set_ratio(&r, -1.0f, true); /* scratch backwards */
    size_t consumed = 123;
    size_t got = espdj_resample_stereo_i16(&r, src, N, dst, N, &consumed);
    CHECK(got > N / 2);              /* played back through the window */
    CHECK(consumed == 0);            /* window held while reversing */
    CHECK_NEAR(est_freq(dst, got, FS), 500.0, 10.0); /* same |rate| */
}

static void test_resampler_slew(void)
{
    /* Non-immediate rate change must glide, not jump. */
    enum { N = 4096 };
    static int16_t src[N * 2], dst[N * 2];
    gen_sine(src, N, 1000.0, FS);
    espdj_resampler_t r;
    espdj_resampler_init(&r);
    espdj_resampler_set_ratio(&r, 1.5f, false);
    espdj_resample_stereo_i16(&r, src, N, dst, 100, NULL);
    CHECK(r.ratio > 1.0f && r.ratio < 1.02f); /* 100 frames * 5e-5 = 0.005 */
}

static void test_ringbuf(void)
{
    enum { CAP = 256 };
    static int16_t storage[CAP * 2];
    espdj_ringbuf_t rb;
    CHECK(espdj_ringbuf_init(&rb, storage, CAP));
    CHECK(!espdj_ringbuf_init(&rb, storage, 100)); /* not a power of two */
    CHECK(espdj_ringbuf_init(&rb, storage, CAP));

    int16_t in[64 * 2], out[64 * 2];
    for (int i = 0; i < 64; i++) { in[i * 2] = (int16_t)i; in[i * 2 + 1] = (int16_t)-i; }

    /* Fill/drain across the wrap point several times. */
    for (int round = 0; round < 20; round++) {
        CHECK(espdj_ringbuf_write(&rb, in, 64) == 64);
        CHECK(espdj_ringbuf_used_frames(&rb) == 64);
        CHECK(espdj_ringbuf_read(&rb, out, 64) == 64);
        CHECK(memcmp(in, out, sizeof(in)) == 0);
    }

    /* Overfill clips at capacity. */
    size_t total = 0;
    for (int i = 0; i < 10; i++) total += espdj_ringbuf_write(&rb, in, 64);
    CHECK(total == CAP);
    CHECK(espdj_ringbuf_free_frames(&rb) == 0);
    CHECK(espdj_ringbuf_skip(&rb, 1000) == CAP);
    CHECK(espdj_ringbuf_used_frames(&rb) == 0);
}

static double rms(const int16_t *buf, size_t frames)
{
    double acc = 0;
    for (size_t i = 0; i < frames; i++) {
        double v = buf[i * 2];
        acc += v * v;
    }
    return sqrt(acc / (double)frames);
}

static void test_eq_and_filter(void)
{
    enum { N = 16384 };
    static int16_t low[N * 2], high[N * 2], work[N * 2];
    gen_sine(low, N, 100.0, FS);
    gen_sine(high, N, 8000.0, FS);

    espdj_channel_t ch;
    espdj_channel_init(&ch, (float)FS);

    /* Kill the lows: 100 Hz drops hard, 8 kHz unaffected. */
    espdj_channel_set_eq(&ch, -24.0f, 0.0f, 0.0f);
    memcpy(work, low, sizeof(work));
    espdj_channel_process_i16(&ch, work, N);
    double att = 20.0 * log10(rms(work + 2048, N - 4096) / rms(low, N));
    CHECK(att < -18.0);

    espdj_channel_init(&ch, (float)FS);
    espdj_channel_set_eq(&ch, -24.0f, 0.0f, 0.0f);
    memcpy(work, high, sizeof(work));
    espdj_channel_process_i16(&ch, work, N);
    att = 20.0 * log10(rms(work + 2048, N - 4096) / rms(high, N));
    CHECK(att > -1.5 && att < 1.5);

    /* LP filter swept low kills 8 kHz. */
    espdj_channel_init(&ch, (float)FS);
    espdj_channel_set_filter(&ch, -0.9f);
    memcpy(work, high, sizeof(work));
    espdj_channel_process_i16(&ch, work, N);
    att = 20.0 * log10((rms(work + 2048, N - 4096) + 1.0) / rms(high, N));
    CHECK(att < -30.0);

    /* Filter knob near center bypasses. */
    espdj_channel_init(&ch, (float)FS);
    espdj_channel_set_filter(&ch, 0.01f);
    CHECK(!ch.filter_active);
}

static void test_xfader_law(void)
{
    float a, b;
    espdj_xfader_gains(0.0f, 0.0f, &a, &b);
    CHECK_NEAR(a, 1.0, 1e-6); CHECK_NEAR(b, 0.0, 1e-6);
    espdj_xfader_gains(1.0f, 0.0f, &a, &b);
    CHECK_NEAR(a, 0.0, 1e-6); CHECK_NEAR(b, 1.0, 1e-6);
    /* Constant power at center: -3 dB each, a^2 + b^2 = 1. */
    espdj_xfader_gains(0.5f, 0.0f, &a, &b);
    CHECK_NEAR(a, sqrt(0.5), 1e-3);
    CHECK_NEAR(a * a + b * b, 1.0, 1e-3);
    /* Sharp curve: both decks at (nearly) full through the middle. */
    espdj_xfader_gains(0.5f, 1.0f, &a, &b);
    CHECK(a > 0.95f && b > 0.95f);
    /* ...but still fully cut at the extremes. */
    espdj_xfader_gains(0.0f, 1.0f, &a, &b);
    CHECK_NEAR(b, 0.0, 1e-6);
    espdj_xfader_gains(1.0f, 1.0f, &a, &b);
    CHECK_NEAR(a, 0.0, 1e-6);
}

int main(void)
{
    test_resampler_unity();
    test_resampler_pitch();
    test_resampler_reverse();
    test_resampler_slew();
    test_ringbuf();
    test_eq_and_filter();
    test_xfader_law();
    TEST_MAIN_END();
}
