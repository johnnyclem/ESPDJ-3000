#include "espnow_link.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "audio_task.h"
#include "deck_state.h"

static const char *TAG = "espnow";

#define ESPNOW_LATENCY_US 3000 /* typical one-way, used for sync compensation */
#define PEER_TIMEOUT_US   3000000

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t s_mac[6];
static uint8_t s_peer[6];
static _Atomic bool s_active;
static _Atomic bool s_paired;
static _Atomic uint64_t s_last_rx_us;

static void ensure_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_AP, .encrypt = false };
        memcpy(p.peer_addr, mac, 6);
        esp_now_add_peer(&p);
    }
}

static void announce_self(void)
{
    espdj_announce_t a = { .unit_type = ESPDJ_UNIT_DECK, .proto_version = 1 };
    memcpy(a.mac, s_mac, 6);
    espdj_frame_t f;
    espdj_pack_announce(&f, &a);
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(&f, wire);
    esp_now_send(BCAST, wire, n);
}

static void adopt_peer(const uint8_t *mac)
{
    memcpy(s_peer, mac, 6);
    ensure_peer(mac);
    atomic_store(&s_paired, true);
    atomic_store(&g_deck.wireless, true);
    /* Deterministic roles: lower MAC = Left + sync master. */
    bool lower = memcmp(s_mac, mac, 6) < 0;
    atomic_store(&g_deck.side, lower ? ESPDJ_SIDE_LEFT : ESPDJ_SIDE_RIGHT);
    atomic_store(&g_deck.is_sync_master, lower);
    atomic_store(&g_deck.is_master, lower);
    /* No shared analog bus in wireless mode: full gain, local jack. */
    atomic_store(&g_deck.xfader_gain, 1.0f);
    ESP_LOGI(TAG, "paired with %02x:%02x:..%02x — side=%s",
             mac[0], mac[1], mac[5], lower ? "L" : "R");
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!atomic_load(&s_active)) return;

    espdj_decoder_t dec;
    espdj_decoder_init(&dec);
    espdj_frame_t f;
    bool got = false;
    for (int i = 0; i < len && !got; i++) {
        got = espdj_decoder_feed(&dec, data[i], &f);
    }
    if (!got) return;
    atomic_store(&s_last_rx_us, (uint64_t)esp_timer_get_time());

    switch (f.type) {
    case ESPDJ_MSG_ANNOUNCE: {
        espdj_announce_t a;
        if (espdj_unpack_announce(&f, &a) && a.unit_type == ESPDJ_UNIT_DECK &&
            memcmp(a.mac, s_mac, 6) != 0) {
            if (!atomic_load(&s_paired)) {
                adopt_peer(a.mac);
                announce_self(); /* let the peer adopt us promptly */
            }
        }
        break;
    }
    case ESPDJ_MSG_BEAT_PHASE: {
        espdj_beat_phase_t b;
        if (espdj_unpack_beat_phase(&f, &b) &&
            !atomic_load(&g_deck.is_sync_master)) {
            audio_sync_feed_report((uint64_t)esp_timer_get_time(),
                                   b.beat_number, b.phase_q16, b.bpm_q8,
                                   ESPNOW_LATENCY_US);
        }
        break;
    }
    case ESPDJ_MSG_TRANSPORT: {
        espdj_transport_t t;
        if (espdj_unpack_transport(&f, &t) &&
            t.side == atomic_load(&g_deck.side)) {
            deck_apply_transport(&t);
        }
        break;
    }
    default:
        break;
    }
    (void)info;
}

static void pair_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (atomic_load(&s_active)) {
            uint64_t now = (uint64_t)esp_timer_get_time();
            if (atomic_load(&s_paired) &&
                now - atomic_load(&s_last_rx_us) > PEER_TIMEOUT_US) {
                atomic_store(&s_paired, false);
                atomic_store(&g_deck.wireless, false);
                ESP_LOGW(TAG, "peer lost");
            }
            if (!atomic_load(&s_paired)) {
                announce_self();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void espnow_link_start(void)
{
    /* WiFi (SoftAP for the phone app) is already up; ESP-NOW rides on it. */
    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_WIFI_STA));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ensure_peer(BCAST);
    xTaskCreatePinnedToCore(pair_task, "espnow_pair", 3072, NULL, 6, NULL, 0);
    ESP_LOGI(TAG, "ESP-NOW link ready");
}

void espnow_link_set_active(bool active)
{
    atomic_store(&s_active, active);
    if (!active) {
        atomic_store(&s_paired, false);
    }
}

bool espnow_link_paired(void)
{
    return atomic_load(&s_paired);
}

void espnow_link_send_frame(const espdj_frame_t *f)
{
    if (!atomic_load(&s_paired)) return;
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(f, wire);
    esp_now_send(s_peer, wire, n);
}

void espnow_link_broadcast_beat(void)
{
    if (!atomic_load(&s_paired)) return;
    espdj_beat_phase_t b = {
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .beat_number = atomic_load(&g_deck.beat_number),
        .phase_q16 = (uint16_t)(atomic_load(&g_deck.beat_phase) * 65536.0f),
        .bpm_q8 = (uint16_t)(atomic_load(&g_deck.effective_bpm) * 256.0f),
    };
    espdj_frame_t f;
    espdj_pack_beat_phase(&f, &b);
    espnow_link_send_frame(&f);
}
