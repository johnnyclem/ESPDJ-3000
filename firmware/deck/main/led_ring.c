/*
 * Beat-phase indication (design doc §0 #11): a comet sweeps the ring once
 * per beat; the whole ring flashes on the downbeat (beat 1 of the bar).
 * Deck color follows the side: Left = cyan, Right = amber. 1 GPIO, high
 * playability value.
 */
#include "led_ring.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

#include "deck_state.h"
#include "pins.h"

static const char *TAG = "led";

#define LED_FPS 60

static led_strip_handle_t s_strip;

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000 / LED_FPS));

        bool loaded = atomic_load(&g_deck.track_loaded);
        bool playing = atomic_load(&g_deck.playing);
        float phase = atomic_load(&g_deck.beat_phase);
        uint32_t beat = atomic_load(&g_deck.beat_number);
        bool right = atomic_load(&g_deck.side) == ESPDJ_SIDE_RIGHT;
        int bpb = g_deck.meta.beats_per_bar > 0 ? g_deck.meta.beats_per_bar : 4;
        bool downbeat = (beat % (uint32_t)bpb) == 0;

        uint8_t base_r = right ? 40 : 0;
        uint8_t base_g = right ? 18 : 28;
        uint8_t base_b = right ? 0 : 40;

        for (int i = 0; i < LED_RING_COUNT; i++) {
            float r = 0, g = 0, b = 0;
            if (!loaded) {
                /* idle breathe */
                float br = 0.1f + 0.08f * sinf((float)xTaskGetTickCount() * 0.01f);
                r = base_r * br; g = base_g * br; b = base_b * br;
            } else if (playing) {
                /* comet head at phase around the ring, short tail */
                float head = phase * LED_RING_COUNT;
                float d = fmodf((float)i - head + LED_RING_COUNT, (float)LED_RING_COUNT);
                float bright = d < 3.0f ? (1.0f - d / 3.0f) : 0.06f;
                if (downbeat && phase < 0.12f) bright = 1.0f; /* bar flash */
                r = base_r * bright * 4; g = base_g * bright * 4; b = base_b * bright * 4;
            } else {
                r = base_r; g = base_g; b = base_b;
            }
            led_strip_set_pixel(s_strip, i,
                                (uint32_t)(r > 255 ? 255 : r),
                                (uint32_t)(g > 255 ? 255 : g),
                                (uint32_t)(b > 255 ? 255 : b));
        }
        led_strip_refresh(s_strip);
    }
}

void led_ring_start(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED_RING,
        .max_leds = LED_RING_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    xTaskCreatePinnedToCore(led_task, "led_ring", 3072, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "beat ring: %d px", LED_RING_COUNT);
}
