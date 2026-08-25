/*
 * ESP-DJ3000 deck firmware — entry point.
 *
 * Core split (design doc §3):
 *   Core 1 (RT):  SD feeder + audio pipeline (audio_task.c)
 *   Core 0:       UI/LVGL, bus & ESP-NOW protocol, WiFi + web server,
 *                 WS2812 beat ring, haptics.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "audio_task.h"
#include "bus_link.h"
#include "deck_state.h"
#include "espnow_link.h"
#include "haptics.h"
#include "led_ring.h"
#include "net_task.h"
#include "sd_card.h"
#include "track_meta.h"
#include "ui_task.h"

static const char *TAG = "main";

deck_state_t g_deck;
library_t g_library;

void deck_apply_transport(const espdj_transport_t *t)
{
    switch ((espdj_transport_op_t)t->op) {
    case ESPDJ_TR_PLAY_TOGGLE: deck_send_cmd(DECK_CMD_PLAY_TOGGLE, 0); break;
    case ESPDJ_TR_CUE_PRESS:   deck_send_cmd(DECK_CMD_CUE_PRESS, 0); break;
    case ESPDJ_TR_CUE_RELEASE: deck_send_cmd(DECK_CMD_CUE_RELEASE, 0); break;
    case ESPDJ_TR_HOT_CUE:     deck_send_cmd(DECK_CMD_HOT_CUE, t->arg); break;
    case ESPDJ_TR_LOOP_TOGGLE: deck_send_cmd(DECK_CMD_LOOP_TOGGLE, t->arg); break;
    case ESPDJ_TR_BEAT_JUMP:   deck_send_cmd(DECK_CMD_BEAT_JUMP, (int8_t)t->arg); break;
    case ESPDJ_TR_SLIP_TOGGLE: deck_send_cmd(DECK_CMD_SLIP_TOGGLE, 0); break;
    case ESPDJ_TR_SYNC_TOGGLE: deck_send_cmd(DECK_CMD_SYNC_TOGGLE, 0); break;
    case ESPDJ_TR_LOAD_TRACK: {
        int idx = t->arg | ((int)t->arg2 << 8);
        if (idx >= 0 && idx < g_library.count) {
            strlcpy(g_deck.track_path, g_library.entry[idx].path,
                    sizeof(g_deck.track_path));
            deck_send_cmd(DECK_CMD_LOAD_TRACK, idx);
        }
        break;
    }
    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-DJ3000 deck booting");
    ESP_ERROR_CHECK(nvs_flash_init());

    memset(&g_deck, 0, sizeof(g_deck));
    atomic_store(&g_deck.trim, 1.0f);
    atomic_store(&g_deck.xfader_gain, 1.0f); /* alone until told otherwise */
    atomic_store(&g_deck.side, ESPDJ_SIDE_LEFT);
    g_deck.cmd_q = xQueueCreate(16, sizeof(deck_cmd_t));

    if (sd_card_mount("/sdcard")) {
        library_scan(&g_library, "/sdcard");
        sd_card_bench_kbps(g_library.count > 0 ? g_library.entry[0].path
                                               : "/sdcard/bench.bin");
    }

    haptics_start();
    audio_task_start();   /* core 1: feeder + RT pipeline */
    ui_task_start();      /* core 0: LVGL + jog ring */
    led_ring_start();
    net_task_start();     /* SoftAP + captive portal + dj.local + WS */
    espnow_link_start();  /* rides on the same WiFi */
    bus_link_start();     /* SENSE + half-duplex DATA bus */

    ESP_LOGI(TAG, "all tasks up");
}
