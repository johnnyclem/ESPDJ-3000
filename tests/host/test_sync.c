#include "test_util.h"
#include "espdj_sync.h"

/* Simulate a slave deck whose clock runs at a slight rate offset from the
 * master, disciplined by 10 Hz beat-phase reports. */
static void test_convergence(void)
{
    espdj_sync_t s;
    espdj_sync_init(&s);

    const double master_bpm = 128.0;
    const double master_bps = master_bpm / 60.0;
    const uint16_t bpm_q8 = (uint16_t)(master_bpm * 256.0);

    /* Slave starts ~1 ms out of phase (post-anchor residue) and its clock
     * runs +50 ppm fast — crystal-tolerance class of error, inside the
     * ±200 ppm (±0.02%) trim authority. */
    double local_beat = -0.002;
    const double rate_err = 5e-5;

    uint64_t t = 0;
    const uint64_t step_us = 10000; /* 100 Hz discipline loop */
    double master_beat = 0.0;
    float trim = 0.0f;
    double final_err = 1.0;

    for (int i = 0; i < 1200 * 100; i++) { /* 20 simulated minutes */
        t += step_us;
        master_beat = master_bps * (double)t / 1e6;
        /* Slave advances at nominal(1+rate_err) plus applied trim. */
        local_beat += master_bps * (1.0 + rate_err + (double)trim) * (double)step_us / 1e6;

        if (i % 10 == 0) { /* 10 Hz reports, 3 ms transport latency */
            uint64_t arrival = t + 3000;
            uint32_t bn = (uint32_t)master_beat;
            uint16_t ph = (uint16_t)((master_beat - (double)bn) * 65536.0);
            espdj_sync_on_report(&s, arrival, bn, ph, bpm_q8, 3000);
        }
        trim = espdj_sync_update(&s, t, local_beat);
        final_err = espdj_sync_phase_error(&s, t, local_beat);
    }

    /* Phase-locked to well under a millisecond of beat error. */
    CHECK(fabs(final_err) < 0.001);
    /* The steady-state trim must cancel the clock-rate error. */
    CHECK_NEAR(trim, -rate_err, 1e-5);
    /* And it never exceeded its authority. */
    CHECK(fabs(trim) <= ESPDJ_SYNC_MAX_TRIM + 1e-9);
}

static void test_wrap_and_clamp(void)
{
    espdj_sync_t s;
    espdj_sync_init(&s);
    CHECK(espdj_sync_update(&s, 1000, 5.0) == 0.0f); /* no report yet */

    espdj_sync_on_report(&s, 1000000, 100, 0, 120 * 256, 0);
    /* Phase error wraps modulo one beat: being 3.5 beats "behind" reads
     * as +0.5, not +3.5 — decks lock to the beat, not track position. */
    double e = espdj_sync_phase_error(&s, 1000000, 96.5);
    CHECK_NEAR(e, 0.5, 1e-9);
    e = espdj_sync_phase_error(&s, 1000000, 100.25);
    CHECK_NEAR(e, -0.25, 1e-9);

    /* Huge error saturates at the ±0.02% authority. */
    float trim = espdj_sync_update(&s, 1000000, 100.45);
    CHECK_NEAR(trim, -ESPDJ_SYNC_MAX_TRIM, 1e-9);
}

int main(void)
{
    test_convergence();
    test_wrap_and_clamp();
    TEST_MAIN_END();
}
