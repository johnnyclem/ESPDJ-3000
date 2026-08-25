/*
 * ESP-DJ3000 deck — core 1 realtime audio (design doc §3).
 *
 *   SD -> PSRAM cache -> cubic resampler (varispeed/scratch) ->
 *   gain/EQ/filter -> I2S DMA (PCM5102A), pre-fader tap -> CUE switch.
 *
 * Two tasks, both pinned to core 1:
 *   - sd_feeder (prio 5): keeps a sliding cache of file frames around the
 *     playhead in PSRAM. Sequential reads only; ~176 KB/s for one stereo
 *     stream, well inside the P0 SD bench budget.
 *   - audio_rt  (prio 22): 128-frame blocks. Never touches the SD card,
 *     never allocates, never blocks on anything but the I2S DMA queue.
 *
 * The PSRAM cache is a frame-indexed ring: file frame f lives at slot
 * f & CACHE_MASK. Validity is the contiguous range [cache_lo, cache_hi).
 * The RT task gathers the (tiny) span the resampler needs into a local
 * contiguous buffer each block, so scratching backwards is just reading
 * a different span — no rewind I/O in the hot path (SD only re-reads if
 * you scratch further back than the cache holds).
 */
#include "audio_task.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "deck_state.h"
#include "espdj_mix.h"
#include "espdj_resampler.h"
#include "pins.h"
#include "wav_reader.h"

static const char *TAG = "audio";

#define FS_HZ           44100
#define BLOCK_FRAMES    128
#define CACHE_FRAMES    (1u << 18) /* 262144 frames ≈ 5.9 s, 1 MB PSRAM */
#define CACHE_MASK      (CACHE_FRAMES - 1)
#define CACHE_BACK      (FS_HZ * 1)     /* keep 1 s behind the playhead */
#define CACHE_MIN_AHEAD (FS_HZ / 2)     /* refill urgency threshold */
#define FEED_CHUNK      4096            /* frames per SD read */

espdj_sync_t g_sync;
static portMUX_TYPE s_sync_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- shared cache state (feeder <-> RT) ---- */
static int16_t *s_cache;                   /* PSRAM, CACHE_FRAMES * 2 */
static _Atomic uint32_t s_cache_lo, s_cache_hi; /* valid file-frame range */
static _Atomic uint32_t s_generation;      /* bump on load/seek: cache void */
static _Atomic uint32_t s_want_frame;      /* RT tells feeder where it is */
static _Atomic bool     s_track_open;

/* Track handle: owned by the feeder; RT only reads total_frames after
 * s_track_open is set. */
static wav_reader_t s_wav;
static _Atomic uint32_t s_total_frames;

/* ---- RT playhead state (audio_rt only) ---- */
typedef struct {
    double  pos;          /* file frames, fractional */
    double  slip_pos;     /* shadow playhead while slipping */
    bool    slipping;
    double  cue_point;    /* frames */
    bool    cue_held;
    double  loop_start, loop_end;
    bool    loop_on;
    float   rate_now;     /* rate actually applied last block */
} playhead_t;

static playhead_t s_ph;
static espdj_resampler_t s_rs;
static espdj_channel_t s_chan;
static i2s_chan_handle_t s_tx;

static double frames_per_beat(void)
{
    float bpm = g_deck.meta.bpm > 1.0f ? g_deck.meta.bpm : 120.0f;
    return (double)FS_HZ * 60.0 / (double)bpm;
}

static double beat_at(double pos_frames)
{
    return (pos_frames / (double)FS_HZ - g_deck.meta.first_beat_s) /
           (60.0 / (double)(g_deck.meta.bpm > 1.0f ? g_deck.meta.bpm : 120.0f));
}

/* ---- SD feeder ---- */

static void feeder_task(void *arg)
{
    (void)arg;
    int16_t *chunk = heap_caps_malloc(FEED_CHUNK * 2 * sizeof(int16_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *scratch = heap_caps_malloc(FEED_CHUNK * 8 * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    assert(chunk && scratch);
    uint32_t my_gen = 0;

    for (;;) {
        deck_cmd_t cmd;
        /* Track loads come through the command queue but are executed
         * here so file I/O stays off the RT task. Other commands are
         * forwarded to the RT mailbox below. */
        if (xQueuePeek(g_deck.cmd_q, &cmd, 0) == pdTRUE &&
            cmd.op == DECK_CMD_LOAD_TRACK) {
            xQueueReceive(g_deck.cmd_q, &cmd, 0);
            atomic_store(&s_track_open, false);
            atomic_store(&g_deck.track_loaded, false);
            wav_close(&s_wav);
            if (wav_open(&s_wav, g_deck.track_path)) {
                track_meta_load(g_deck.track_path, &g_deck.meta);
                atomic_store(&s_total_frames, s_wav.total_frames);
                atomic_fetch_add(&s_generation, 1);
                atomic_store(&s_cache_lo, 0);
                atomic_store(&s_cache_hi, 0);
                atomic_store(&s_want_frame, 0);
                atomic_store(&s_track_open, true);
                atomic_store(&g_deck.track_loaded, true);
                ESP_LOGI(TAG, "loaded %s (%s, %.1f BPM)", g_deck.track_path,
                         g_deck.meta.title, g_deck.meta.bpm);
            }
            continue;
        }

        if (!atomic_load(&s_track_open)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t gen = atomic_load(&s_generation);
        uint32_t want = atomic_load(&s_want_frame);
        uint32_t lo = atomic_load(&s_cache_lo);
        uint32_t hi = atomic_load(&s_cache_hi);
        uint32_t total = atomic_load(&s_total_frames);

        if (gen != my_gen || want < lo || want + CACHE_BACK / 2 > hi + CACHE_FRAMES) {
            /* New track, or the playhead left the cache (big seek):
             * recenter. Keep CACHE_BACK behind for scratching. */
            my_gen = gen;
            lo = want > CACHE_BACK ? want - CACHE_BACK : 0;
            hi = lo;
            atomic_store(&s_cache_lo, lo);
            atomic_store(&s_cache_hi, hi);
        }

        /* Advance lo so the ring never wraps onto unread data. */
        uint32_t needed_lo = want > CACHE_BACK ? want - CACHE_BACK : 0;
        if (needed_lo > lo) {
            lo = needed_lo;
            atomic_store(&s_cache_lo, lo);
        }

        uint32_t ahead = hi > want ? hi - want : 0;
        uint32_t room = CACHE_FRAMES - (hi - lo);
        if (hi >= total || room < FEED_CHUNK) {
            vTaskDelay(pdMS_TO_TICKS(ahead > CACHE_MIN_AHEAD ? 20 : 2));
            continue;
        }

        uint32_t n = FEED_CHUNK;
        if (hi + n > total) n = total - hi;
        if (!wav_seek_frame(&s_wav, hi)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        uint32_t got = wav_read_stereo(&s_wav, chunk, n, 0, scratch, FEED_CHUNK);
        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Copy into the frame-indexed ring (at most two runs). */
        uint32_t done = 0;
        while (done < got) {
            uint32_t slot = (hi + done) & CACHE_MASK;
            uint32_t run = CACHE_FRAMES - slot;
            if (run > got - done) run = got - done;
            memcpy(&s_cache[slot * 2], &chunk[done * 2], run * 2 * sizeof(int16_t));
            done += run;
        }
        atomic_store(&s_cache_hi, hi + got);
        /* Yield so the RT task and the rest of core 1 breathe. */
        vTaskDelay(ahead > CACHE_MIN_AHEAD ? pdMS_TO_TICKS(5) : 1);
    }
}

/* ---- RT helpers ---- */

/* Gather src frames [first, first+count) from the cache into dst,
 * zero-filling anything outside the valid range or the track. */
static void gather_span(int64_t first, uint32_t count, int16_t *dst)
{
    uint32_t lo = atomic_load(&s_cache_lo);
    uint32_t hi = atomic_load(&s_cache_hi);
    for (uint32_t i = 0; i < count; i++) {
        int64_t f = first + (int64_t)i;
        if (f < 0 || f < (int64_t)lo || f >= (int64_t)hi) {
            dst[i * 2] = 0;
            dst[i * 2 + 1] = 0;
        } else {
            uint32_t slot = (uint32_t)f & CACHE_MASK;
            dst[i * 2]     = s_cache[slot * 2];
            dst[i * 2 + 1] = s_cache[slot * 2 + 1];
        }
    }
}

static void rt_handle_cmd(const deck_cmd_t *c)
{
    double fpb = frames_per_beat();
    switch (c->op) {
    case DECK_CMD_PLAY_TOGGLE:
        atomic_store(&g_deck.playing, !atomic_load(&g_deck.playing));
        break;
    case DECK_CMD_CUE_PRESS:
        if (atomic_load(&g_deck.playing)) {
            /* playing: stop and return to cue */
            atomic_store(&g_deck.playing, false);
            s_ph.pos = s_ph.cue_point;
        } else if (fabs(s_ph.pos - s_ph.cue_point) < 1.0) {
            /* stopped on cue: preview while held */
            s_ph.cue_held = true;
            atomic_store(&g_deck.playing, true);
        } else {
            /* stopped elsewhere: set cue here */
            s_ph.cue_point = s_ph.pos;
        }
        break;
    case DECK_CMD_CUE_RELEASE:
        if (s_ph.cue_held) {
            s_ph.cue_held = false;
            atomic_store(&g_deck.playing, false);
            s_ph.pos = s_ph.cue_point;
        }
        break;
    case DECK_CMD_HOT_CUE: {
        int i = c->arg;
        if (i >= 0 && i < TRACK_MAX_CUES && g_deck.meta.cues[i] >= 0.0) {
            s_ph.pos = g_deck.meta.cues[i] * FS_HZ;
            atomic_store(&g_deck.playing, true);
        }
        break;
    }
    case DECK_CMD_LOOP_TOGGLE:
        if (s_ph.loop_on) {
            s_ph.loop_on = false;
        } else {
            int beats = c->arg > 0 ? c->arg : 4;
            /* snap loop start to the previous beat */
            double b = floor(beat_at(s_ph.pos));
            double start = (g_deck.meta.first_beat_s * FS_HZ) + b * fpb;
            s_ph.loop_start = start >= 0 ? start : 0;
            s_ph.loop_end = s_ph.loop_start + beats * fpb;
            s_ph.loop_on = true;
            if (!s_ph.slipping && atomic_load(&g_deck.slip)) {
                /* entering a loop with slip armed: shadow keeps running */
                s_ph.slipping = true;
                s_ph.slip_pos = s_ph.pos;
            }
        }
        atomic_store(&g_deck.loop_active, s_ph.loop_on);
        break;
    case DECK_CMD_BEAT_JUMP:
        s_ph.pos += (double)c->arg * fpb;
        if (s_ph.pos < 0) s_ph.pos = 0;
        break;
    case DECK_CMD_SLIP_TOGGLE: {
        bool slip = !atomic_load(&g_deck.slip);
        atomic_store(&g_deck.slip, slip);
        if (!slip && s_ph.slipping) {
            s_ph.pos = s_ph.slip_pos; /* rejoin the timeline */
            s_ph.slipping = false;
            s_ph.loop_on = false;
            atomic_store(&g_deck.loop_active, false);
        }
        break;
    }
    case DECK_CMD_SYNC_TOGGLE:
        atomic_store(&g_deck.sync_enabled, !atomic_load(&g_deck.sync_enabled));
        break;
    case DECK_CMD_SEEK_MS:
        s_ph.pos = (double)c->arg * FS_HZ / 1000.0;
        break;
    default:
        break;
    }
}

static void audio_rt_task(void *arg)
{
    (void)arg;
    static int16_t span[(size_t)(BLOCK_FRAMES * 2.5) * 2 + 16];
    static int16_t block[BLOCK_FRAMES * 2];
    const uint32_t span_cap = (uint32_t)(sizeof(span) / (2 * sizeof(int16_t)));

    espdj_resampler_init(&s_rs);
    espdj_channel_init(&s_chan, (float)FS_HZ);

    for (;;) {
        /* Drain non-load commands (loads are consumed by the feeder). */
        deck_cmd_t cmd;
        while (xQueuePeek(g_deck.cmd_q, &cmd, 0) == pdTRUE &&
               cmd.op != DECK_CMD_LOAD_TRACK) {
            xQueueReceive(g_deck.cmd_q, &cmd, 0);
            rt_handle_cmd(&cmd);
        }

        /* ---- rate ---- */
        bool playing = atomic_load(&g_deck.playing);
        bool jog = atomic_load(&g_deck.jog_active);
        float pitch = atomic_load(&g_deck.pitch);
        float rate;
        if (jog) {
            rate = atomic_load(&g_deck.jog_rate); /* scratch: raw ring rate */
            espdj_resampler_set_ratio(&s_rs, rate, true);
        } else {
            rate = playing ? (1.0f + pitch) : 0.0f;
            if (playing && atomic_load(&g_deck.sync_enabled) &&
                !atomic_load(&g_deck.is_sync_master)) {
                portENTER_CRITICAL(&s_sync_mux);
                float trim = espdj_sync_update(&g_sync,
                                               (uint64_t)esp_timer_get_time(),
                                               beat_at(s_ph.pos));
                portEXIT_CRITICAL(&s_sync_mux);
                rate += trim * (1.0f + pitch);
            }
            espdj_resampler_set_ratio(&s_rs, rate, false);
        }
        s_ph.rate_now = rate;

        /* Slip shadow runs at the nominal (non-scratch) rate. */
        if (s_ph.slipping) {
            s_ph.slip_pos += (double)BLOCK_FRAMES * (playing ? (1.0 + pitch) : 0.0);
        }
        if (jog && atomic_load(&g_deck.slip) && !s_ph.slipping) {
            s_ph.slipping = true;
            s_ph.slip_pos = s_ph.pos;
        }

        bool have_track = atomic_load(&s_track_open);
        uint32_t total = atomic_load(&s_total_frames);

        if (!have_track || (!playing && !jog)) {
            memset(block, 0, sizeof(block));
        } else {
            /* ---- gather + resample one block ---- */
            float r = s_rs.ratio > s_rs.target_ratio ? s_rs.ratio : s_rs.target_ratio;
            float rmin = s_rs.ratio < s_rs.target_ratio ? s_rs.ratio : s_rs.target_ratio;
            float rmax = fabsf(r) > fabsf(rmin) ? fabsf(r) : fabsf(rmin);
            uint32_t span_n = (uint32_t)ceilf((float)BLOCK_FRAMES * rmax) + 8;
            if (span_n > span_cap) span_n = span_cap;

            int64_t first;
            double offset;
            if (s_rs.target_ratio >= 0.0f && s_rs.ratio >= 0.0f) {
                first = (int64_t)floor(s_ph.pos) - 1;
                offset = s_ph.pos - floor(s_ph.pos) + 1.0;
            } else {
                /* reverse: window ends just past the playhead */
                first = (int64_t)floor(s_ph.pos) - (int64_t)span_n + 3;
                offset = s_ph.pos - (double)first;
            }
            gather_span(first, span_n, span);
            s_rs.position = offset;
            size_t got = espdj_resample_stereo_i16(&s_rs, span, span_n,
                                                   block, BLOCK_FRAMES, NULL);
            if (got < BLOCK_FRAMES) {
                memset(&block[got * 2], 0, (BLOCK_FRAMES - got) * 2 * sizeof(int16_t));
            }
            s_ph.pos = (double)first + s_rs.position;

            /* ---- loop wrap ---- */
            if (s_ph.loop_on && s_ph.rate_now > 0 && s_ph.pos >= s_ph.loop_end) {
                s_ph.pos -= (s_ph.loop_end - s_ph.loop_start);
            }
            /* ---- end of track ---- */
            if (s_ph.pos >= (double)total) {
                s_ph.pos = (double)total;
                atomic_store(&g_deck.playing, false);
            }
            if (s_ph.pos < 0) s_ph.pos = 0;
        }

        /* ---- per-deck mix chain (crossfader is a control signal) ---- */
        float eq_l = atomic_load(&g_deck.eq_low_db);
        float eq_m = atomic_load(&g_deck.eq_mid_db);
        float eq_h = atomic_load(&g_deck.eq_high_db);
        if (eq_l != s_chan.low_db || eq_m != s_chan.mid_db || eq_h != s_chan.high_db) {
            espdj_channel_set_eq(&s_chan, eq_l, eq_m, eq_h);
        }
        float fpos = atomic_load(&g_deck.filter_pos);
        if (fabsf(fpos - s_chan.filter_pos) > 0.005f) {
            espdj_channel_set_filter(&s_chan, fpos);
        }
        s_chan.trim = atomic_load(&g_deck.trim);
        s_chan.xfader_gain = atomic_load(&g_deck.xfader_gain);

        /* Pre-fader tap feeds the CUE bus: hardware switch, not a copy. */
        gpio_set_level(PIN_CUE_ENABLE, atomic_load(&g_deck.cue_monitor));

        espdj_channel_process_i16(&s_chan, block, BLOCK_FRAMES);

        /* ---- publish status ---- */
        double disp_pos = s_ph.slipping ? s_ph.slip_pos : s_ph.pos;
        atomic_store(&g_deck.position_frames, (uint32_t)(s_ph.pos < 0 ? 0 : s_ph.pos));
        double beat = beat_at(disp_pos);
        double bfloor = floor(beat);
        atomic_store(&g_deck.beat_number, (uint32_t)(bfloor < 0 ? 0 : bfloor));
        atomic_store(&g_deck.beat_phase, (float)(beat - bfloor));
        atomic_store(&g_deck.effective_bpm,
                     (g_deck.meta.bpm > 1.0f ? g_deck.meta.bpm : 120.0f) *
                         fabsf(s_ph.rate_now == 0 ? 1.0f : s_ph.rate_now));
        atomic_store(&s_want_frame, (uint32_t)(s_ph.pos < 0 ? 0 : s_ph.pos));

        /* ---- out. Blocks on DMA: this is the task's pacing. ---- */
        size_t written = 0;
        i2s_channel_write(s_tx, block, sizeof(block), &written, portMAX_DELAY);
    }
}

void audio_task_start(void)
{
    s_cache = heap_caps_malloc(CACHE_FRAMES * 2 * sizeof(int16_t),
                               MALLOC_CAP_SPIRAM);
    assert(s_cache);

    gpio_config_t cue_io = {
        .pin_bit_mask = 1ULL << PIN_CUE_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cue_io));

    /* I2S standard mode, 44.1 kHz 16-bit stereo, PCM5102A (no MCLK). */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = BLOCK_FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FS_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    portENTER_CRITICAL(&s_sync_mux);
    espdj_sync_init(&g_sync);
    portEXIT_CRITICAL(&s_sync_mux);

    xTaskCreatePinnedToCore(feeder_task, "sd_feeder", 6144, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(audio_rt_task, "audio_rt", 8192, NULL, 22, NULL, 1);
    ESP_LOGI(TAG, "audio pipeline up: %d Hz, %d-frame blocks", FS_HZ, BLOCK_FRAMES);
}

uint32_t audio_total_frames_hint(void)
{
    return atomic_load(&s_total_frames);
}

/* Report/report-feed helpers used by bus & espnow (core 0). */
void audio_sync_feed_report(uint64_t local_now_us, uint32_t beat_number,
                            uint16_t phase_q16, uint16_t bpm_q8,
                            uint32_t latency_us)
{
    portENTER_CRITICAL(&s_sync_mux);
    espdj_sync_on_report(&g_sync, local_now_us, beat_number, phase_q16,
                         bpm_q8, latency_us);
    portEXIT_CRITICAL(&s_sync_mux);
}
