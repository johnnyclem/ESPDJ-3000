/*
 * Pad edge illumination per the reference aesthetic: color encodes the
 * active layer (hot-cue amber, loop green, fx violet, shift white); the
 * bank on each side pulses with its deck's beat phase, from polled
 * DECK_STATE (play flag + bpm give us a local phase estimate between
 * 250 Hz polls).
 */
#include "pad_leds.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "bus_master.h"
#include "controls.h"
#include "pins.h"

static const char *TAG = "pad_leds";

static led_strip_handle_t s_strip;

static const uint8_t LAYER_RGB[4][3] = {
    { 255, 120, 10 },  /* hot cue: amber */
    { 30, 220, 80 },   /* loop: green */
    { 150, 60, 255 },  /* fx: violet */
    { 200, 200, 200 }, /* shift: white */
};

static float beat_pulse(espdj_side_t side)
{
    espdj_deck_state_t s;
    if (!bus_master_deck_state(side, &s) || !(s.flags & 1) || s.bpm_q8 == 0) {
        return 0.25f; /* idle glow */
    }
    float bps = ((float)s.bpm_q8 / 256.0f) / 60.0f;
    float t = (float)esp_timer_get_time() / 1e6f;
    float phase = t * bps;
    phase -= floorf(phase);
    return 0.25f + 0.75f * (1.0f - phase) * (1.0f - phase);
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        const uint8_t *c = LAYER_RGB[controls_layer() & 3];
        float pl = beat_pulse(ESPDJ_SIDE_LEFT);
        float pr = beat_pulse(ESPDJ_SIDE_RIGHT);
        for (int i = 0; i < PAD_LED_COUNT; i++) {
            float p = (i < 4) ? pl : pr;
            led_strip_set_pixel(s_strip, i, (uint32_t)(c[0] * p),
                                (uint32_t)(c[1] * p), (uint32_t)(c[2] * p));
        }
        led_strip_refresh(s_strip);
    }
}

void pad_leds_start(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_PAD_LEDS,
        .max_leds = PAD_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    xTaskCreate(led_task, "pad_leds", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "pad edge lighting up");
}
