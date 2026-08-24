#include "controls.h"

#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "pins.h"

static const char *TAG = "controls";

/* ADC hysteresis: ignore movement under ~4 LSB of 12-bit — an RC-filtered
 * fader parked mid-travel stays silent on the bus. */
#define ADC_HYST 4

/* MCP23017 registers (IOCON.BANK=0) */
#define MCP_IODIRA 0x00
#define MCP_IODIRB 0x01
#define MCP_GPPUA  0x0C
#define MCP_GPPUB  0x0D
#define MCP_GPIOA  0x12
#define MCP_GPIOB  0x13

static adc_oneshot_unit_handle_t s_adc;
static i2c_master_dev_handle_t s_mcp;
static pad_event_cb_t s_on_pad;
static encoder_cb_t s_on_encoder;

static uint16_t s_adc_last[5];
static uint8_t s_pads_last = 0xFF;
static uint8_t s_enc_last;
static uint8_t s_layer;
/* pad hold tracking for the shift layer: holding pad 7 while pressing
 * pads 0..3 selects the layer instead of firing an event */
static bool s_shift_held;

static void mcp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    i2c_master_transmit(s_mcp, buf, 2, 20);
}

static uint8_t mcp_read(uint8_t reg)
{
    uint8_t val = 0;
    i2c_master_transmit_receive(s_mcp, &reg, 1, &val, 1, 20);
    return val;
}

void controls_init(pad_event_cb_t on_pad, encoder_cb_t on_encoder)
{
    s_on_pad = on_pad;
    s_on_encoder = on_encoder;

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    for (int ch = 0; ch < 5; ch++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, (adc_channel_t)ch, &ccfg));
    }

    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bcfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bcfg, &bus));
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MCP23017_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dcfg, &s_mcp));

    mcp_write(MCP_IODIRA, 0xFF); /* pads in */
    mcp_write(MCP_IODIRB, 0xFF); /* encoders in */
    mcp_write(MCP_GPPUA, 0xFF);
    mcp_write(MCP_GPPUB, 0xFF);
    s_enc_last = mcp_read(MCP_GPIOB);

    ESP_LOGI(TAG, "surface ready (5 analog, 8 pads, 4 encoders)");
}

static uint16_t adc_read_hyst(int ch)
{
    int raw = 0;
    adc_oneshot_read(s_adc, (adc_channel_t)ch, &raw);
    int last = s_adc_last[ch];
    if (abs(raw - last) > ADC_HYST || raw == 0 || raw >= 4090) {
        s_adc_last[ch] = (uint16_t)raw;
    }
    return s_adc_last[ch];
}

/* Quadrature transition table: [prev<<2 | curr] -> -1/0/+1 */
static const int8_t QUAD[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

void controls_scan(controls_state_t *out)
{
    espdj_control_t prev = out->control;

    out->control.crossfader = adc_read_hyst(PIN_ADC_XFADER);
    out->control.tempo[0]   = adc_read_hyst(PIN_ADC_TEMPO_L);
    out->control.tempo[1]   = adc_read_hyst(PIN_ADC_TEMPO_R);
    out->control.pot[0]     = adc_read_hyst(PIN_ADC_POT_L);
    out->control.pot[1]     = adc_read_hyst(PIN_ADC_POT_R);
    if (memcmp(&prev, &out->control, sizeof(prev)) != 0) {
        out->control_dirty = true;
    }

    /* pads: active low; left bank pads 0-3 address the Left deck,
     * right bank 4-7 the Right deck. Pad 7 held = layer select chord. */
    uint8_t pads = mcp_read(MCP_GPIOA);
    uint8_t changed = pads ^ s_pads_last;
    if (changed) {
        for (int i = 0; i < 8; i++) {
            if (!(changed & (1 << i))) continue;
            bool pressed = !(pads & (1 << i));
            if (i == 7) {
                s_shift_held = pressed;
                continue;
            }
            if (s_shift_held && pressed && i < 4) {
                s_layer = (uint8_t)i; /* chord: shift + pad = layer */
                continue;
            }
            if (s_on_pad) {
                espdj_pad_event_t e = {
                    .side = (i < 4) ? ESPDJ_SIDE_LEFT : ESPDJ_SIDE_RIGHT,
                    .pad = (uint8_t)(i & 3),
                    .layer = s_layer,
                    .pressed = pressed,
                };
                s_on_pad(&e);
            }
        }
        s_pads_last = pads;
    }

    /* encoders: 4x quadrature on port B */
    uint8_t enc = mcp_read(MCP_GPIOB);
    for (int e = 0; e < 4; e++) {
        uint8_t p = (s_enc_last >> (e * 2)) & 3;
        uint8_t c = (enc >> (e * 2)) & 3;
        if (p != c) {
            int d = QUAD[(p << 2) | c];
            if (d && s_on_encoder) {
                s_on_encoder(e, d);
            }
        }
    }
    s_enc_last = enc;
}

uint8_t controls_layer(void)
{
    return s_layer;
}
