#include "haptics.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "pins.h"

static const char *TAG = "haptic";

static QueueHandle_t s_q;

typedef struct {
    uint16_t duty_pct;
    uint16_t on_ms;
} pulse_t;

static const pulse_t PULSES[] = {
    [HAPTIC_DETENT]   = { 45, 6 },
    [HAPTIC_CUE]      = { 80, 18 },
    [HAPTIC_DOWNBEAT] = { 60, 12 },
    [HAPTIC_BUTTON]   = { 70, 10 },
};

static void haptic_task(void *arg)
{
    (void)arg;
    haptic_kind_t kind;
    for (;;) {
        if (xQueueReceive(s_q, &kind, portMAX_DELAY) != pdTRUE) continue;
        const pulse_t *p = &PULSES[kind];
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                      (1023 * p->duty_pct) / 100);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        vTaskDelay(pdMS_TO_TICKS(p->on_ms));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        /* brief refractory so a burst of detents feels like texture,
         * not a continuous buzz */
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void haptics_start(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_HAPTIC,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));

    s_q = xQueueCreate(8, sizeof(haptic_kind_t));
    xTaskCreatePinnedToCore(haptic_task, "haptic", 2048, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "haptics ready");
}

void haptics_pulse(haptic_kind_t kind)
{
    if (s_q) {
        xQueueSend(s_q, &kind, 0);
    }
}
