/*
 * ESP-DJ3000 controller — ESP32-C3 pin map (custom PCB, design doc §5).
 *
 * 17 controls: crossfader + 2 tempo faders + 2 pots on the C3's five
 * ADC1 channels (RC-filtered, 100n to GND at the pin); 8 pads + 4
 * encoders (A/B) on an MCP23017 over I2C; encoder center-presses share
 * the pad matrix's spare bank via the same expander INT line.
 */
#pragma once

/* Analog (ADC1): RC-filtered wipers */
#define PIN_ADC_XFADER    0  /* ADC1_CH0 */
#define PIN_ADC_TEMPO_L   1  /* ADC1_CH1 */
#define PIN_ADC_TEMPO_R   2  /* ADC1_CH2 */
#define PIN_ADC_POT_L     3  /* ADC1_CH3 */
#define PIN_ADC_POT_R     4  /* ADC1_CH4 */

/* MCP23017 I2C */
#define PIN_I2C_SDA       5
#define PIN_I2C_SCL       6
#define PIN_MCP_INT       7
#define MCP23017_ADDR     0x20
/* Port A: 8 pads (active low). Port B: 4 encoders A/B (b0..b7). */

/* Interconnect */
#define PIN_BUS_DATA      10
#define PIN_SENSE_LEFT    18 /* plain GPIO + ADC not needed: ctrl only
                                cares *that* a neighbor exists; type comes
                                from the ANNOUNCE broadcast */
#define PIN_SENSE_RIGHT   19

/* WS2812 chain: 8 pad edge-light pixels */
#define PIN_PAD_LEDS      8
#define PAD_LED_COUNT     8

#define BUS_UART_NUM      UART_NUM_1
#define BUS_BAUD          1000000
