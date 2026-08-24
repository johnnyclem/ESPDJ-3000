/* ESP-DJ3000 deck — haptic motor pulses (jog detents, cues, downbeats). */
#pragma once

typedef enum {
    HAPTIC_DETENT,   /* jog-ring tick, very short */
    HAPTIC_CUE,      /* passed a cue point */
    HAPTIC_DOWNBEAT, /* bar boundary while beatmatching by feel */
    HAPTIC_BUTTON,   /* touch/pad acknowledge */
} haptic_kind_t;

void haptics_start(void);
void haptics_pulse(haptic_kind_t kind);
