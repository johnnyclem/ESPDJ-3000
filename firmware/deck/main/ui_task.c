/*
 * ESP-DJ3000 deck — UI (design doc §3).
 *
 * 240x240 round IPS (GC9A01, SPI) + CST816S touch, LVGL 9.
 * Layout: rotating waveform ring around the rim, center stack with
 * BPM / pitch / key / time. Touch zones: top = CUE, bottom = PLAY,
 * left = LOOP 4, right = BEAT-JUMP +4 (swipe left/right = jump ∓4),
 * two-finger/slow long-press anywhere = track browser. The physical
 * rotary ring is the jog wheel (haptic detents; scratch when touched).
 */
#include "ui_task.h"

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "audio_task.h"
#include "deck_state.h"
#include "haptics.h"
#include "pins.h"

static const char *TAG = "ui";

#define DISP_W 240
#define DISP_H 240
#define JOG_COUNTS_PER_REV 120   /* 30-detent encoder, 4x quadrature */
#define JOG_SECONDS_PER_REV 1.8f /* one ring turn ≈ 1.8 s of audio (CDJ feel) */
#define JOG_IDLE_TIMEOUT_US 150000

extern library_t g_library; /* app_main owns the scan */

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static pcnt_unit_handle_t s_pcnt;

static lv_obj_t *s_scr_player, *s_scr_browser;
static lv_obj_t *s_lbl_bpm, *s_lbl_pitch, *s_lbl_key, *s_lbl_time, *s_lbl_title;
static lv_obj_t *s_ring_arc[60];
static lv_obj_t *s_list;

/* ---- display plumbing ---- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px);
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_read_data(s_touch);
    uint16_t x, y;
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &cnt, 1) && cnt) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void display_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .max_transfer_sz = DISP_W * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_cfg, &io));
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_LCD_BL,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    /* touch */
    i2c_master_bus_handle_t i2c;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c));
    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tio_cfg =
        ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c, &tio_cfg, &tio));
    esp_lcd_touch_config_t t_cfg = {
        .x_max = DISP_W,
        .y_max = DISP_H,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tio, &t_cfg, &s_touch));

    /* LVGL */
    lv_init();
    s_disp = lv_display_create(DISP_W, DISP_H);
    static uint8_t buf1[DISP_W * 40 * 2];
    static uint8_t buf2[DISP_W * 40 * 2];
    lv_display_set_buffers(s_disp, buf1, buf2, sizeof(buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);
}

/* ---- jog ring (rotary encoder, PCNT quadrature) ---- */

static void jog_init(void)
{
    pcnt_unit_config_t ucfg = { .low_limit = -32768, .high_limit = 32767,
                                .flags.accum_count = true };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &s_pcnt));
    pcnt_chan_config_t c1 = { .edge_gpio_num = PIN_ENC_A,
                              .level_gpio_num = PIN_ENC_B };
    pcnt_channel_handle_t ch1;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &c1, &ch1));
    pcnt_channel_set_edge_action(ch1, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(ch1, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_chan_config_t c2 = { .edge_gpio_num = PIN_ENC_B,
                              .level_gpio_num = PIN_ENC_A };
    pcnt_channel_handle_t ch2;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &c2, &ch2));
    pcnt_channel_set_edge_action(ch2, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(ch2, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));

    gpio_config_t press = { .pin_bit_mask = 1ULL << PIN_ENC_PRESS,
                            .mode = GPIO_MODE_INPUT,
                            .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&press);
}

/* Called from the UI tick: convert ring motion to a signed scratch rate. */
static void jog_poll(void)
{
    static int last_count = 0;
    static int64_t last_t = 0;
    static int64_t last_move_t = 0;
    static bool center_was_down = false;

    int count = 0;
    pcnt_unit_get_count(s_pcnt, &count);
    int64_t now = esp_timer_get_time();
    int d = count - last_count;
    float dt = (float)(now - last_t) / 1e6f;
    last_count = count;
    last_t = now;

    if (d != 0) {
        last_move_t = now;
        haptics_pulse(HAPTIC_DETENT);
        float seconds_moved = (float)d * JOG_SECONDS_PER_REV / JOG_COUNTS_PER_REV;
        float rate = dt > 0.0005f ? seconds_moved / dt : 0.0f;
        if (rate > 8.0f) rate = 8.0f;
        if (rate < -8.0f) rate = -8.0f;
        atomic_store(&g_deck.jog_rate, rate);
        atomic_store(&g_deck.jog_active, true);
    } else if (atomic_load(&g_deck.jog_active) &&
               now - last_move_t > JOG_IDLE_TIMEOUT_US) {
        atomic_store(&g_deck.jog_active, false);
        atomic_store(&g_deck.jog_rate, 0.0f);
    }

    bool down = gpio_get_level(PIN_ENC_PRESS) == 0;
    if (down && !center_was_down) {
        haptics_pulse(HAPTIC_BUTTON);
        deck_send_cmd(DECK_CMD_PLAY_TOGGLE, 0);
    }
    center_was_down = down;
}

/* ---- player screen ---- */

static void player_touch_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - DISP_W / 2, dy = p.y - DISP_H / 2;

    if (code == LV_EVENT_LONG_PRESSED) {
        lv_screen_load(s_scr_browser);
        return;
    }
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_LEFT) deck_send_cmd(DECK_CMD_BEAT_JUMP, -4);
        if (dir == LV_DIR_RIGHT) deck_send_cmd(DECK_CMD_BEAT_JUMP, 4);
        return;
    }
    if (code != LV_EVENT_CLICKED) return;

    haptics_pulse(HAPTIC_BUTTON);
    if (abs(dx) > abs(dy)) {
        if (dx < 0) deck_send_cmd(DECK_CMD_LOOP_TOGGLE, 4);
        else        deck_send_cmd(DECK_CMD_SLIP_TOGGLE, 0);
    } else {
        if (dy < 0) deck_send_cmd(DECK_CMD_CUE_PRESS, 0);
        else        deck_send_cmd(DECK_CMD_PLAY_TOGGLE, 0);
    }
}

static void build_player(void)
{
    s_scr_player = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_player, lv_color_black(), 0);

    /* waveform ring: 60 arc segments, 6° each, radius near the rim */
    for (int i = 0; i < 60; i++) {
        lv_obj_t *a = lv_arc_create(s_scr_player);
        lv_obj_set_size(a, 236, 236);
        lv_obj_center(a);
        lv_arc_set_bg_angles(a, i * 6 + 271, i * 6 + 275);
        lv_arc_set_value(a, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_style(a, NULL, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(a, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(0x103040), LV_PART_MAIN);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        s_ring_arc[i] = a;
    }

    s_lbl_title = lv_label_create(s_scr_player);
    lv_obj_align(s_lbl_title, LV_ALIGN_CENTER, 0, -58);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_title, 140);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0x9FB4C8), 0);

    s_lbl_bpm = lv_label_create(s_scr_player);
    lv_obj_align(s_lbl_bpm, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_text_font(s_lbl_bpm, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_bpm, lv_color_white(), 0);

    s_lbl_pitch = lv_label_create(s_scr_player);
    lv_obj_align(s_lbl_pitch, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_text_color(s_lbl_pitch, lv_color_hex(0x50E0A0), 0);

    s_lbl_key = lv_label_create(s_scr_player);
    lv_obj_align(s_lbl_key, LV_ALIGN_CENTER, -40, 32);
    lv_obj_set_style_text_color(s_lbl_key, lv_color_hex(0xE0B050), 0);

    s_lbl_time = lv_label_create(s_scr_player);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 30, 32);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_white(), 0);

    lv_obj_add_event_cb(s_scr_player, player_touch_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_scr_player, LV_OBJ_FLAG_CLICKABLE);
}

static void browser_pick_event(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < g_library.count) {
        snprintf(g_deck.track_path, sizeof(g_deck.track_path), "%s",
                 g_library.entry[idx].path);
        deck_send_cmd(DECK_CMD_LOAD_TRACK, (int32_t)idx);
        haptics_pulse(HAPTIC_CUE);
    }
    lv_screen_load(s_scr_player);
}

static void build_browser(void)
{
    s_scr_browser = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_browser, lv_color_black(), 0);
    s_list = lv_list_create(s_scr_browser);
    lv_obj_set_size(s_list, 200, 200);
    lv_obj_center(s_list);
    for (int i = 0; i < g_library.count; i++) {
        char line[96];
        snprintf(line, sizeof(line), "%s  %.0f", g_library.entry[i].title,
                 (double)g_library.entry[i].bpm);
        lv_obj_t *btn = lv_list_add_button(s_list, NULL, line);
        lv_obj_add_event_cb(btn, browser_pick_event, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

/* 20 Hz data refresh: ring rotation + center stack. */
static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    jog_poll();

    char buf[48];
    float bpm = atomic_load(&g_deck.effective_bpm);
    snprintf(buf, sizeof(buf), "%.1f", (double)bpm);
    lv_label_set_text(s_lbl_bpm, buf);
    snprintf(buf, sizeof(buf), "%+.1f%%", (double)(atomic_load(&g_deck.pitch) * 100.0f));
    lv_label_set_text(s_lbl_pitch, buf);
    lv_label_set_text(s_lbl_key, g_deck.meta.key[0] ? g_deck.meta.key : "--");
    uint32_t ms = (uint32_t)((uint64_t)atomic_load(&g_deck.position_frames) * 1000 / 44100);
    snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(ms / 60000),
             (unsigned long)((ms / 1000) % 60));
    lv_label_set_text(s_lbl_time, buf);
    lv_label_set_text(s_lbl_title, g_deck.meta.title);

    /* rotate the waveform overview so the playhead is at 12 o'clock */
    int n = g_deck.meta.overview_len > 0 ? g_deck.meta.overview_len : 0;
    float ppos = 0.0f;
    uint32_t total = 0;
    if (n > 0) {
        /* position as a fraction of the track */
        total = audio_total_frames_hint();
        if (total > 0) {
            ppos = (float)atomic_load(&g_deck.position_frames) / (float)total;
        }
    }
    bool playing = atomic_load(&g_deck.playing);
    for (int i = 0; i < 60; i++) {
        uint8_t level = 32;
        if (n > 0) {
            int bin = ((int)(ppos * n) + (i - 0) * n / 60) % n;
            if (bin < 0) bin += n;
            level = g_deck.meta.overview[bin];
        }
        uint8_t g = 0x30 + (level >> 1);
        lv_color_t c = (i < 2 || i > 57)
                           ? lv_color_hex(0xFF4040) /* playhead marker */
                           : lv_color_make(playing ? 0x10 : 0x08, g, 0x40);
        lv_obj_set_style_arc_color(s_ring_arc[i], c, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_ring_arc[i], 2 + (level * 8) / 255,
                                   LV_PART_MAIN);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    display_init();
    jog_init();
    build_player();
    build_browser();
    lv_screen_load(s_scr_player);
    lv_timer_create(refresh_cb, 50, NULL);

    for (;;) {
        uint32_t wait = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(wait < 5 ? 5 : (wait > 40 ? 40 : wait)));
    }
}

void ui_task_start(void)
{
    xTaskCreatePinnedToCore(ui_task, "ui", 12288, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "UI task started");
}
