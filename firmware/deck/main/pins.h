/*
 * ESP-DJ3000 deck — pin map.
 *
 * Base board: MaTouch 1.28" ToolSet_Controller (ESP32-S3). The display,
 * touch, rotary ring, haptic motor and microSD pins below follow the
 * Makerfabs reference firmware; VERIFY against the schematic of your board
 * revision before first flash. The expansion pins (I2S DAC, bus, SENSE,
 * LED ring) are our own wiring on the free GPIOs.
 */
#pragma once

/* --- GC9A01 240x240 round IPS (SPI) --- */
#define PIN_LCD_SCLK      14
#define PIN_LCD_MOSI      13
#define PIN_LCD_DC        12
#define PIN_LCD_CS        11
#define PIN_LCD_RST       21
#define PIN_LCD_BL        47

/* --- CST816S capacitive touch (I2C) --- */
#define PIN_TOUCH_SDA     38
#define PIN_TOUCH_SCL     39
#define PIN_TOUCH_INT     40
#define PIN_TOUCH_RST     41

/* --- Rotary ring encoder + center press --- */
#define PIN_ENC_A         9
#define PIN_ENC_B         10
#define PIN_ENC_PRESS     6

/* --- Haptic motor (DRV/transistor, PWM) --- */
#define PIN_HAPTIC        7

/* --- microSD (SDMMC 1-bit) --- */
#define PIN_SD_CLK        2
#define PIN_SD_CMD        1
#define PIN_SD_D0         3

/* --- PCM5102A I2S DAC (program audio, line level) --- */
#define PIN_I2S_BCK       16
#define PIN_I2S_LRCK      15
#define PIN_I2S_DOUT      17

/* --- Cue select: routes pre-fader tap to the CUE bus buffer --- */
#define PIN_CUE_ENABLE    18

/* --- Interconnect (§2): DATA is one half-duplex UART pin --- */
#define PIN_BUS_DATA      8
/* SENSE per face: neighbor detect + resistor ID (ADC-capable pins). */
#define PIN_SENSE_LEFT    4   /* ADC1_CH3 */
#define PIN_SENSE_RIGHT   5   /* ADC1_CH4 */
#define SENSE_ADC_UNIT    ADC_UNIT_1
#define SENSE_ADC_CH_LEFT  ADC_CHANNEL_3
#define SENSE_ADC_CH_RIGHT ADC_CHANNEL_4

/* --- WS2812 beat ring / under-glow --- */
#define PIN_LED_RING      48
#define LED_RING_COUNT    16

/* --- Bus UART --- */
#define BUS_UART_NUM      UART_NUM_1
#define BUS_BAUD          1000000
