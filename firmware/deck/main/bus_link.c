#include "bus_link.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "soc/uart_periph.h"

#include "audio_task.h"
#include "deck_state.h"
#include "espdj_mix.h"
#include "espnow_link.h"
#include "pins.h"

static const char *TAG = "bus";

#define PROTO_VERSION     1
#define DEBOUNCE_US       100000  /* 100 ms mate debounce (§2 discovery) */
#define ANNOUNCE_WINDOW_MS 120
#define POLL_HZ           250
#define BEAT_BCAST_HZ     10

typedef struct {
    espdj_topo_unit_t unit[3];
    int count;
    bool resolved;
    espdj_topology_t topo;
} bus_view_t;

static uint8_t s_mac[6];
static bus_view_t s_view;
static espdj_control_t s_last_control;
static _Atomic bool s_have_control;
static _Atomic uint64_t s_last_sense_change_us;
static adc_oneshot_unit_handle_t s_adc;
static uint8_t s_seq;

/* ---- half-duplex single-wire UART ----
 * TX and RX signals of BUS_UART_NUM are both routed to PIN_BUS_DATA via
 * the GPIO matrix; the pad runs open-drain with a pull-up so multiple
 * transmitters can share the wire (idle high, dominant low). We hear our
 * own transmissions and drop them after the fact (compare against the
 * last frame sent). */
static uint8_t s_last_tx[ESPDJ_MAX_FRAME];
static size_t s_last_tx_len;

static void bus_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = BUS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BUS_UART_NUM, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BUS_UART_NUM, &cfg));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUS_DATA,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    esp_rom_gpio_connect_out_signal(PIN_BUS_DATA,
        UART_PERIPH_SIGNAL(1, SOC_UART_TX_PIN_IDX), false, false);
    esp_rom_gpio_connect_in_signal(PIN_BUS_DATA,
        UART_PERIPH_SIGNAL(1, SOC_UART_RX_PIN_IDX), false);
}

static void bus_send(espdj_frame_t *f)
{
    f->seq = s_seq++;
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(f, wire);
    memcpy(s_last_tx, wire, n);
    s_last_tx_len = n;
    uart_write_bytes(BUS_UART_NUM, wire, n);
}

/* On the shared wire we hear our own transmissions; a decoded frame that
 * re-encodes to exactly the last thing we sent is our echo. */
static bool is_own_echo(const espdj_frame_t *f)
{
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(f, wire);
    return n == s_last_tx_len && memcmp(wire, s_last_tx, n) == 0;
}

/* ---- SENSE ---- */

static void IRAM_ATTR sense_isr(void *arg)
{
    (void)arg;
    /* Timestamp only; classification happens after debounce in-task. */
    uint64_t now = (uint64_t)esp_timer_get_time();
    atomic_store(&s_last_sense_change_us, now);
}

static espdj_unit_type_t sense_read(adc_channel_t ch)
{
    int mv_sum = 0;
    for (int i = 0; i < 4; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc, ch, &raw);
        mv_sum += raw; /* 12-bit vs 3.3V full scale approximation */
    }
    uint32_t mv = (uint32_t)((mv_sum / 4) * 3300 / 4095);
    return espdj_sense_classify(mv, 3300);
}

static void sense_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = SENSE_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, SENSE_ADC_CH_LEFT, &ccfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, SENSE_ADC_CH_RIGHT, &ccfg));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_SENSE_LEFT) | (1ULL << PIN_SENSE_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_SENSE_LEFT, sense_isr, NULL);
    gpio_isr_handler_add(PIN_SENSE_RIGHT, sense_isr, NULL);
}

/* ---- discovery / topology ---- */

static void view_note_announce(const espdj_announce_t *a)
{
    for (int i = 0; i < s_view.count; i++) {
        if (memcmp(s_view.unit[i].mac, a->mac, 6) == 0) {
            s_view.unit[i].unit_type = a->unit_type;
            s_view.unit[i].left_occupied = a->left_occupied;
            s_view.unit[i].right_occupied = a->right_occupied;
            return;
        }
    }
    if (s_view.count < 3) {
        espdj_topo_unit_t *u = &s_view.unit[s_view.count++];
        memcpy(u->mac, a->mac, 6);
        u->unit_type = a->unit_type;
        u->left_occupied = a->left_occupied;
        u->right_occupied = a->right_occupied;
    }
}

static void announce_self(bool left_occ, bool right_occ)
{
    espdj_announce_t a = { .unit_type = ESPDJ_UNIT_DECK,
                           .left_occupied = left_occ,
                           .right_occupied = right_occ,
                           .proto_version = PROTO_VERSION };
    memcpy(a.mac, s_mac, 6);
    view_note_announce(&a); /* we are part of our own view */
    espdj_frame_t f;
    espdj_pack_announce(&f, &a);
    bus_send(&f);
}

static void resolve_roles(void)
{
    s_view.resolved = espdj_resolve_topology(s_view.unit, s_view.count, &s_view.topo);
    if (!s_view.resolved) {
        ESP_LOGW(TAG, "topology inconsistent (%d units), retrying", s_view.count);
        return;
    }
    int self = -1;
    for (int i = 0; i < s_view.count; i++) {
        if (memcmp(s_view.unit[i].mac, s_mac, 6) == 0) self = i;
    }
    if (self < 0) return;

    bool master = (s_view.topo.master == self);
    atomic_store(&g_deck.side, (uint8_t)s_view.topo.deck_side[self]);
    atomic_store(&g_deck.is_master, master);
    /* Sync master follows bus master unless the user moved it; the left
     * deck leads when a controller is master. */
    if (s_view.unit[s_view.topo.master].unit_type == ESPDJ_UNIT_CONTROLLER) {
        atomic_store(&g_deck.is_sync_master,
                     s_view.topo.deck_side[self] == ESPDJ_SIDE_LEFT);
    } else {
        atomic_store(&g_deck.is_sync_master, master);
    }
    atomic_store(&g_deck.wireless, false);
    ESP_LOGI(TAG, "role: side=%d master=%d units=%d",
             s_view.topo.deck_side[self], master, s_view.count);
}

/* ---- frame handling ---- */

static void broadcast_beat_phase(void)
{
    espdj_beat_phase_t b = {
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .beat_number = atomic_load(&g_deck.beat_number),
        .phase_q16 = (uint16_t)(atomic_load(&g_deck.beat_phase) * 65536.0f),
        .bpm_q8 = (uint16_t)(atomic_load(&g_deck.effective_bpm) * 256.0f),
    };
    espdj_frame_t f;
    espdj_pack_beat_phase(&f, &b);
    bus_send(&f);
}

static void handle_frame(const espdj_frame_t *f)
{
    switch (f->type) {
    case ESPDJ_MSG_ANNOUNCE: {
        espdj_announce_t a;
        if (espdj_unpack_announce(f, &a)) {
            view_note_announce(&a);
            resolve_roles();
        }
        break;
    }
    case ESPDJ_MSG_CONTROL: {
        espdj_control_t c;
        if (espdj_unpack_control(f, &c)) {
            s_last_control = c;
            atomic_store(&s_have_control, true);
            /* Crossfader is a control signal: compute my own gain. */
            float pos = (float)c.crossfader / 4095.0f;
            float curve = (float)c.xfader_curve / 255.0f;
            float ga, gb;
            espdj_xfader_gains(pos, curve, &ga, &gb);
            uint8_t side = atomic_load(&g_deck.side);
            atomic_store(&g_deck.xfader_gain,
                         side == ESPDJ_SIDE_RIGHT ? gb : ga);
            int t = (side == ESPDJ_SIDE_RIGHT) ? 1 : 0;
            atomic_store(&g_deck.pitch,
                         ((float)c.tempo[t] - 2048.0f) / 2048.0f * 0.08f);
        }
        break;
    }
    case ESPDJ_MSG_PAD_EVENT: {
        espdj_pad_event_t p;
        if (espdj_unpack_pad_event(f, &p) && p.pressed &&
            p.side == atomic_load(&g_deck.side)) {
            static const espdj_transport_op_t layer_op[4] = {
                ESPDJ_TR_HOT_CUE, ESPDJ_TR_LOOP_TOGGLE,
                ESPDJ_TR_BEAT_JUMP, ESPDJ_TR_SLIP_TOGGLE };
            espdj_transport_t t = { .side = p.side, .op = layer_op[p.layer & 3] };
            if (t.op == ESPDJ_TR_HOT_CUE) t.arg = p.pad;
            if (t.op == ESPDJ_TR_LOOP_TOGGLE) t.arg = (uint8_t)(1 << p.pad);
            if (t.op == ESPDJ_TR_BEAT_JUMP)
                t.arg = (uint8_t)(int8_t)((p.pad < 2) ? -(1 << p.pad) : (1 << (p.pad - 2)));
            deck_apply_transport(&t);
        }
        break;
    }
    case ESPDJ_MSG_TRANSPORT: {
        espdj_transport_t t;
        if (espdj_unpack_transport(f, &t)) {
            if (t.side == atomic_load(&g_deck.side)) {
                deck_apply_transport(&t);
            }
        }
        break;
    }
    case ESPDJ_MSG_BEAT_PHASE: {
        espdj_beat_phase_t b;
        if (espdj_unpack_beat_phase(f, &b) && !atomic_load(&g_deck.is_sync_master)) {
            /* Wired bus: transit time at 1 Mbps is ~200 us for the frame;
             * treat sender timestamp as "now - frame time". */
            audio_sync_feed_report((uint64_t)esp_timer_get_time(),
                                   b.beat_number, b.phase_q16, b.bpm_q8, 200);
        }
        break;
    }
    case ESPDJ_MSG_POLL:
        if (f->len >= 6 && memcmp(f->payload, s_mac, 6) == 0) {
            espdj_deck_state_t s = {
                .side = atomic_load(&g_deck.side),
                .flags = (uint8_t)((atomic_load(&g_deck.playing) ? 1 : 0) |
                                   (atomic_load(&g_deck.loop_active) ? 2 : 0) |
                                   (atomic_load(&g_deck.slip) ? 4 : 0) |
                                   (atomic_load(&g_deck.sync_enabled) ? 8 : 0)),
                .bpm_q8 = (uint16_t)(atomic_load(&g_deck.effective_bpm) * 256.0f),
                .position_ms = (uint32_t)((uint64_t)atomic_load(&g_deck.position_frames) * 1000 / 44100),
                .length_ms = 0,
                .pitch_q8 = (int16_t)(atomic_load(&g_deck.pitch) * 100.0f * 256.0f),
            };
            espdj_frame_t r;
            espdj_pack_deck_state(&r, &s);
            bus_send(&r);
        }
        break;
    default:
        break;
    }
}

/* ---- main bus task ---- */

static void bus_task(void *arg)
{
    (void)arg;
    espdj_decoder_t dec;
    espdj_decoder_init(&dec);
    espdj_frame_t frame;
    uint8_t rx[64];

    uint64_t last_enum_us = 0;
    uint64_t last_beat_us = 0;
    uint64_t last_poll_us = 0;
    int poll_idx = 0;
    espdj_unit_type_t left_prev = ESPDJ_UNIT_NONE, right_prev = ESPDJ_UNIT_NONE;

    for (;;) {
        /* -- SENSE debounce & (re)enumeration -- */
        uint64_t now = (uint64_t)esp_timer_get_time();
        uint64_t change = atomic_load(&s_last_sense_change_us);
        espdj_unit_type_t l = sense_read(SENSE_ADC_CH_LEFT);
        espdj_unit_type_t r = sense_read(SENSE_ADC_CH_RIGHT);
        bool settled = (now - change) > DEBOUNCE_US;
        if (settled && (l != left_prev || r != right_prev ||
                        (now - last_enum_us > 2000000 && !s_view.resolved))) {
            left_prev = l;
            right_prev = r;
            last_enum_us = now;
            memset(&s_view, 0, sizeof(s_view));
            if (l == ESPDJ_UNIT_NONE && r == ESPDJ_UNIT_NONE) {
                /* Undocked: single unit; ESP-NOW pairing may take over. */
                announce_self(false, false);
                resolve_roles();
                espnow_link_set_active(true);
            } else {
                espnow_link_set_active(false);
                announce_self(l != ESPDJ_UNIT_NONE, r != ESPDJ_UNIT_NONE);
                vTaskDelay(pdMS_TO_TICKS(ANNOUNCE_WINDOW_MS));
                /* announces collected by handle_frame during the window */
                resolve_roles();
            }
        }

        /* -- receive -- */
        int n = uart_read_bytes(BUS_UART_NUM, rx, sizeof(rx), 0);
        for (int i = 0; i < n; i++) {
            if (espdj_decoder_feed(&dec, rx[i], &frame) && !is_own_echo(&frame)) {
                handle_frame(&frame);
            }
        }

        /* -- master duties -- */
        if (s_view.resolved && atomic_load(&g_deck.is_master) &&
            !atomic_load(&g_deck.wireless)) {
            if (s_view.count > 1 && now - last_poll_us > 1000000 / POLL_HZ) {
                last_poll_us = now;
                poll_idx = (poll_idx + 1) % s_view.count;
                if (memcmp(s_view.unit[poll_idx].mac, s_mac, 6) != 0) {
                    espdj_frame_t p = { .type = ESPDJ_MSG_POLL, .len = 6 };
                    memcpy(p.payload, s_view.unit[poll_idx].mac, 6);
                    bus_send(&p);
                }
            }
        }
        if (atomic_load(&g_deck.is_sync_master) &&
            atomic_load(&g_deck.playing) &&
            now - last_beat_us > 1000000 / BEAT_BCAST_HZ) {
            last_beat_us = now;
            if (!atomic_load(&g_deck.wireless)) {
                broadcast_beat_phase();
            }
            espnow_link_broadcast_beat();
        }

        vTaskDelay(1);
    }
}

void link_route_transport(const espdj_transport_t *t)
{
    if (t->side == atomic_load(&g_deck.side) || t->side == ESPDJ_SIDE_NONE) {
        deck_apply_transport(t);
        return;
    }
    espdj_frame_t f;
    espdj_pack_transport(&f, t);
    if (atomic_load(&g_deck.wireless)) {
        espnow_link_send_frame(&f);
    } else {
        bus_send(&f);
    }
}

bool link_get_control(espdj_control_t *out)
{
    if (!atomic_load(&s_have_control)) return false;
    *out = s_last_control;
    return true;
}

void bus_link_start(void)
{
    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_WIFI_STA));
    bus_uart_init();
    sense_init();
    xTaskCreatePinnedToCore(bus_task, "bus", 6144, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "bus link up (1-wire UART @ %d)", BUS_BAUD);
}
