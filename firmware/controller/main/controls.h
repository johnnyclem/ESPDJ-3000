/*
 * ESP-DJ3000 controller — control-surface scanning.
 *
 * Faders/pots: RC-filtered ADC with hysteresis so a parked fader never
 * chatters frames onto the bus. Pads/encoders: MCP23017 over I2C,
 * polled at 1 kHz with software debounce.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "espdj_proto.h"

typedef struct {
    espdj_control_t control;     /* current surface state, 0..4095 */
    bool control_dirty;          /* changed since last cleared */
} controls_state_t;

typedef void (*pad_event_cb_t)(const espdj_pad_event_t *evt);
typedef void (*encoder_cb_t)(int encoder, int delta);

void controls_init(pad_event_cb_t on_pad, encoder_cb_t on_encoder);

/* Scan analog + expander once; call at >= 250 Hz. Fires callbacks for
 * pad edges and encoder detents; updates `out`. */
void controls_scan(controls_state_t *out);

/* Active pad layer (0 hot-cue / 1 loop / 2 fx / 3 shift), driven by the
 * shift pad combos; exposed for the LED task. */
uint8_t controls_layer(void);
