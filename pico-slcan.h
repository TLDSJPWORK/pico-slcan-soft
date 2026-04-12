/**
 * Copyright 2025 JP WORKS. All rights reserved.
 *
 * Implementation of SLCAN using Raspberry Pi Pico and MCP251XFD to receive CAN FD.
 * 
 */

#ifndef PICO_SLCAN_H
#define PICO_SLCAN_H

#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"

// Include the WiFi chip access functions for driving the on-board LED on the Pico W
#ifndef PICO_DEFAULT_LED_PIN
#include "pico/cyw43_arch.h"
#endif

#include "mcp25xxfd/canapi.h"
#include "pico-ssd1306/ssd1306.h"
#include "slcan/slcan.h"

#define SOFTWARE_VERSION 0x01
#define SLEEPTIME 25
#define NUM_PINS 8

#define MAX_BRP 16
#define MAX_TSEG1 255
#define MAX_TSEG2 127
#define MIN_TQ 1
#define MAX_TQ 385
#define ALLOWABLE_SP_ERROR 0.01 // 1%

// 表示用テーブル
typedef struct {
    uint32_t value;
    const char *label;
} RateMap;

RateMap arb_map[] = {
    {500000, "500K"}, {10000, " 10K"}, {20000, " 20K"}, {50000, " 50K"},
    {100000, "100K"}, {125000, "125K"}, {250000, "250K"},
    {800000, "800K"}, {1000000, "  1M"}
};

RateMap data_map[] = {
    {0, "None"}, {1000000, "  1M"}, {2000000, "  2M"}, {3000000, "  3M"},
    {4000000, "  4M"}, {5000000, "  5M"}
};

void pico_slcan_can_init(void);
void pico_slcan_can_stop(void);
bool pico_slcan_can_speedchange(uint32_t, uint32_t, double, uint32_t, double);
bool pico_slcan_can_speedchange_custom(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
bool pico_slcan_send_canmsg(char *, bool, bool);
bool pico_slcan_recv_canmsg(can_frame_t *);
bool pico_slcan_set_ac(char *);
bool pico_slcan_set_am(char *);
bool pico_slcan_can_statusread(can_status_t *);
bool pico_slcan_versionread(char *, size_t);
void pico_slcan_oled_update(void);
uint32_t pico_slcan_get_osc(void);

void print_frame(can_frame_t*, uint32_t);
void TIME_CRITICAL irq_handler(void);
void led_init(void);
void led_on(void);
void led_off(void);

can_mode_t get_can_mode(const can_bitrate_t *);
void oled_gpios_setup(void);
void oled_update(void);
const char* get_label(uint32_t, RateMap *, size_t);

void setup(void);
void loop_core0(void);
void loop_core1(void);

int main(void);
void main_core1(void);

#endif // PICO_SLCAN_H