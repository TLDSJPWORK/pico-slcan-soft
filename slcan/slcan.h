/**
 * Copyright 2025 JPWORKS. All rights reserved.
 *
 * Using Raspberry Pi Pico and MCP251XFD for SLCAN.
 * 
 */

#ifndef SLCAN_H
#define SLCAN_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/sync.h"

#include "../mcp25xxfd/canapi.h"

#define NVM_SIZE 4096
#define NVM_ADDR ((XIP_BASE + (PICO_FLASH_SIZE_BYTES)) - NVM_SIZE) // アプリ領域の終端

#define MAX_CMD_LEN 256

// Frame-specific flags
// Must fit in 8 bits
#define CAN_FRAME_FLAG_RTR        (1U << 0)
#define CAN_FRAME_FLAG_FDF        (1U << 1)
#define CAN_FRAME_FLAG_ESI        (1U << 2)
#define CAN_FRAME_FLAG_BRS        (1U << 3)

uint32_t millis();

#define CUSTOM_ALL 99999999  // カスタム値の例
#define DEFAULT_ARBITRATION_BIT_RATE 500000
#define DEFAULT_ARB_SP 0.75
#define DEFAULT_DATA_BIT_RATE 0
#define DEFAULT_DATA_SP 0.8

extern uint32_t mSerial;

extern uint32_t mArbitration_bit_rate;
extern const double mArb_SP;
extern uint32_t mData_bit_rate;
extern const double mData_SP;

// 関数ポインタを定義
extern void (*can_init_func)(void);
extern void (*can_stop_func)(void);
extern bool (*can_speedchange_func)(uint32_t, uint32_t, double, uint32_t, double);
extern bool (*can_speedchange_custom_func)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
extern bool (*send_canmsg_func)(char *, bool, bool);
extern bool (*recv_canmsg_func)(can_frame_t *);
extern bool (*set_ac_func)(char *);
extern bool (*set_am_func)(char *);
extern bool (*can_statusread_func)(can_status_t *);
extern bool (*versionread_func)(char *, size_t);
extern void (*oled_update_func)(void);
extern uint32_t (*get_osc)(void);

void register_can_init(void (*func)(void));
void register_can_stop(void (*func)(void));
void register_can_speedchange(bool (*func)(uint32_t, uint32_t, double, uint32_t, double));
void register_can_speedchange_custom(bool (*func)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t));
void register_send_canmsg(bool (*func)(char *, bool, bool));
void register_recv_canmsg(bool (*func)(can_frame_t *));
void register_set_ac(bool (*func)(char *));
void register_set_am(bool (*func)(char *));
void register_can_statusread(bool (*func)(can_status_t *));
void register_versionread(bool (*func)(char *, size_t));
void register_oled_update(void (*func)(void));
void register_get_osc(uint32_t (*func)(void));

void slcan_ack();
void slcan_nack();

bool pars_slcancmd_speedchange_custom(char *buf);
void pars_slcancmd(char *buf);
void transfer_tty2can();
void transfer_can2tty();
void slcan_setup();

#endif // SLCAN_H
