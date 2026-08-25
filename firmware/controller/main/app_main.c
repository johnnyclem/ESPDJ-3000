/*
 * ESP-DJ3000 controller firmware — ESP32-C3, pure control surface (§5).
 *
 * No display, no audio processing: the analog buses pass straight
 * through; its jacks are just more buffered taps. This firmware scans
 * 17 controls, is always bus master when docked, and broadcasts:
 *   - CONTROL frames at up to 250 Hz (only when something moved),
 *   - PAD_EVENT frames on pad edges,
 *   - TRANSPORT frames from the encoders (browse / load).
 * Crossfader stays a control signal: each deck computes its own gain.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "bus_master.h"
#include "controls.h"
#include "pad_leds.h"

static const char *TAG = "main";

#define CONTROL_HZ 250

static controls_state_t s_state = {
    .control = { .crossfader = 2048, .tempo = { 2048, 2048 },
                 .pot = { 2048, 2048 } },
};

static void on_pad(const espdj_pad_event_t *evt)
{
    bus_master_send_pad(evt);
}

static void on_encoder(int encoder, int delta)
{
    /* Encoders 0/1 = Left deck browse + beat-jump nudge, 2/3 = Right.
     * v1 maps the outer pair to beat-jump, the inner pair to library
     * browsing (delta rides the transport arg). */
    espdj_side_t side = (encoder < 2) ? ESPDJ_SIDE_LEFT : ESPDJ_SIDE_RIGHT;
    bool outer = (encoder % 2) == 0;
    espdj_transport_t t = {
        .side = (uint8_t)side,
        .op = outer ? ESPDJ_TR_BEAT_JUMP : ESPDJ_TR_LOAD_TRACK,
        .arg = (uint8_t)(int8_t)delta,
    };
    bus_master_send_transport(&t);
}

static void scan_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    uint64_t last_control_us = 0;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1)); /* 1 kHz scan */
        controls_scan(&s_state);

        uint64_t now = (uint64_t)esp_timer_get_time();
        if (s_state.control_dirty && now - last_control_us >= 1000000 / CONTROL_HZ) {
            s_state.control_dirty = false;
            last_control_us = now;
            bus_master_send_control(&s_state.control);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-DJ3000 controller booting");
    ESP_ERROR_CHECK(nvs_flash_init());

    bus_master_start();
    controls_init(on_pad, on_encoder);
    pad_leds_start();
    xTaskCreate(scan_task, "scan", 4096, NULL, 12, NULL);

    ESP_LOGI(TAG, "control surface live");
}
