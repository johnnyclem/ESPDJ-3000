#include "bus_master.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "soc/uart_periph.h"

#include "pins.h"

static const char *TAG = "bus_master";

#define POLL_HZ 250

static uint8_t s_mac[6];
static uint8_t s_seq;
static SemaphoreHandle_t s_tx_mutex;
static uint8_t s_last_tx[ESPDJ_MAX_FRAME];
static size_t s_last_tx_len;

static espdj_topo_unit_t s_units[3];
static int s_unit_count;
static espdj_deck_state_t s_deck_state[2];
static bool s_deck_state_valid[2];

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

    gpio_config_t sense = {
        .pin_bit_mask = (1ULL << PIN_SENSE_LEFT) | (1ULL << PIN_SENSE_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sense));
}

static void bus_send(espdj_frame_t *f)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    f->seq = s_seq++;
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(f, wire);
    memcpy(s_last_tx, wire, n);
    s_last_tx_len = n;
    uart_write_bytes(BUS_UART_NUM, wire, n);
    xSemaphoreGive(s_tx_mutex);
}

static bool is_own_echo(const espdj_frame_t *f)
{
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(f, wire);
    return n == s_last_tx_len && memcmp(wire, s_last_tx, n) == 0;
}

static void note_unit(const espdj_announce_t *a)
{
    for (int i = 0; i < s_unit_count; i++) {
        if (memcmp(s_units[i].mac, a->mac, 6) == 0) {
            s_units[i].unit_type = a->unit_type;
            s_units[i].left_occupied = a->left_occupied;
            s_units[i].right_occupied = a->right_occupied;
            return;
        }
    }
    if (s_unit_count < 3) {
        espdj_topo_unit_t *u = &s_units[s_unit_count++];
        memcpy(u->mac, a->mac, 6);
        u->unit_type = a->unit_type;
        u->left_occupied = a->left_occupied;
        u->right_occupied = a->right_occupied;
        ESP_LOGI(TAG, "unit %d joined (type %d)", s_unit_count, a->unit_type);
    }
}

static void announce_self(void)
{
    /* SENSE here is plain occupancy: pulled low by the neighbor's ID
     * resistor when a face is mated. */
    espdj_announce_t a = {
        .unit_type = ESPDJ_UNIT_CONTROLLER,
        .left_occupied = gpio_get_level(PIN_SENSE_LEFT) == 0,
        .right_occupied = gpio_get_level(PIN_SENSE_RIGHT) == 0,
        .proto_version = 1,
    };
    memcpy(a.mac, s_mac, 6);
    note_unit(&a);
    espdj_frame_t f;
    espdj_pack_announce(&f, &a);
    bus_send(&f);
}

static void handle_frame(const espdj_frame_t *f)
{
    switch (f->type) {
    case ESPDJ_MSG_ANNOUNCE: {
        espdj_announce_t a;
        if (espdj_unpack_announce(f, &a)) note_unit(&a);
        break;
    }
    case ESPDJ_MSG_DECK_STATE: {
        espdj_deck_state_t s;
        if (espdj_unpack_deck_state(f, &s)) {
            int i = (s.side == ESPDJ_SIDE_RIGHT) ? 1 : 0;
            s_deck_state[i] = s;
            s_deck_state_valid[i] = true;
        }
        break;
    }
    default:
        /* beat-phase and transport frames are deck<->deck traffic;
         * the master just leaves the wire open for them */
        break;
    }
}

static void bus_task(void *arg)
{
    (void)arg;
    espdj_decoder_t dec;
    espdj_decoder_init(&dec);
    espdj_frame_t frame;
    uint8_t rx[64];
    uint64_t last_announce = 0;
    uint64_t last_poll = 0;
    int poll_idx = 0;

    for (;;) {
        uint64_t now = (uint64_t)esp_timer_get_time();
        if (now - last_announce > 1000000) {
            last_announce = now;
            announce_self();
        }

        int n = uart_read_bytes(BUS_UART_NUM, rx, sizeof(rx), 0);
        for (int i = 0; i < n; i++) {
            if (espdj_decoder_feed(&dec, rx[i], &frame) && !is_own_echo(&frame)) {
                handle_frame(&frame);
            }
        }

        /* poll decks round-robin between control broadcasts */
        if (s_unit_count > 1 && now - last_poll > 1000000 / POLL_HZ) {
            last_poll = now;
            poll_idx = (poll_idx + 1) % s_unit_count;
            if (s_units[poll_idx].unit_type == ESPDJ_UNIT_DECK) {
                espdj_frame_t p = { .type = ESPDJ_MSG_POLL, .len = 6 };
                memcpy(p.payload, s_units[poll_idx].mac, 6);
                bus_send(&p);
            }
        }
        vTaskDelay(1);
    }
}

void bus_master_send_control(const espdj_control_t *c)
{
    espdj_frame_t f;
    espdj_pack_control(&f, c);
    bus_send(&f);
}

void bus_master_send_pad(const espdj_pad_event_t *p)
{
    espdj_frame_t f;
    espdj_pack_pad_event(&f, p);
    bus_send(&f);
}

void bus_master_send_transport(const espdj_transport_t *t)
{
    espdj_frame_t f;
    espdj_pack_transport(&f, t);
    bus_send(&f);
}

bool bus_master_deck_state(espdj_side_t side, espdj_deck_state_t *out)
{
    int i = (side == ESPDJ_SIDE_RIGHT) ? 1 : 0;
    if (!s_deck_state_valid[i]) return false;
    *out = s_deck_state[i];
    return true;
}

void bus_master_start(void)
{
    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_WIFI_STA));
    s_tx_mutex = xSemaphoreCreateMutex();
    bus_uart_init();
    xTaskCreate(bus_task, "bus_master", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "bus master up @ %d baud", BUS_BAUD);
}
