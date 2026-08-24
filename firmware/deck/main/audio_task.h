#pragma once

#include "espdj_sync.h"

/* Beat-sync discipline state, fed by bus/espnow reports, consumed by the
 * audio task. Guarded by its own small critical section (not RT-critical). */
extern espdj_sync_t g_sync;

/* Start the SD feeder + RT audio tasks (both pinned to core 1). */
void audio_task_start(void);

/* Loaded track length in frames (0 if none) — for UI progress display. */
uint32_t audio_total_frames_hint(void);

/* Thread-safe entry for beat-phase reports arriving from the bus or
 * ESP-NOW link (called from core 0). */
void audio_sync_feed_report(uint64_t local_now_us, uint32_t beat_number,
                            uint16_t phase_q16, uint16_t bpm_q8,
                            uint32_t latency_us);
