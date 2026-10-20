/**
 * Copyright 2025 JP WORKS. All rights reserved.
 *
 * Implementation of SLCAN using Raspberry Pi Pico and MCP251XFD to receive CAN FD.
 * 
 */

#include "pico-slcan.h"

static can_controller_t mController;
static critical_section_t can_rx_lock;
can_bitrate_t mBitrate;
can_errorcode_t mRc;
bool mIrq_registerd = false;
bool mController_started = false;

// Core0 owns the FIFO. The CAN ISR only publishes completed transmissions.
#define SLCAN_TX_BUFFER_SIZE 1000U
static can_frame_t tx_buffer[SLCAN_TX_BUFFER_SIZE];
static uint32_t tx_head, tx_tail, tx_count;
static bool tx_inflight;
static volatile uint32_t tx_completed;
static uint32_t tx_submit_completed;
static uint32_t tx_accepted, tx_rejected, tx_high_water, tx_retries;
static uint32_t tx_recoveries, tx_uncertain, tx_last_faults;
static uint64_t tx_next_attempt_us, tx_next_health_us;

void TIME_CRITICAL can_isr_callback_frame_tx(can_uref_t uref, uint32_t timestamp) {
    (void)uref;
    (void)timestamp;
    tx_completed++;
}

static void tx_buffer_clear(void) {
    tx_head = tx_tail = tx_count = 0;
    tx_inflight = false;
    tx_next_attempt_us = tx_next_health_us = 0;
}

// Keep the head until its completion event. One hardware frame at a time also
// prevents the hardware priority queue from reordering equal-ID FIFO frames.
static void TIME_CRITICAL pico_slcan_tx_service(void) {
    if (!mController_started) return;
    critical_section_enter_blocking(&can_rx_lock);
    // Completion and fault snapshots must be atomic with the ISR. A positively
    // completed head is removed before recovery; an unconfirmed head is retained.
    if (tx_inflight && tx_completed != tx_submit_completed) {
        if (++tx_head == SLCAN_TX_BUFFER_SIZE) tx_head = 0;
        tx_count--;
        tx_inflight = false;
    }
    if (!tx_count && !mController.target_specific.tx_faults) goto out;
    uint64_t now = time_us_64();
    if (mController.target_specific.tx_faults) {
        if (now < tx_next_attempt_us) goto out;
        uint32_t reasons = mController.target_specific.tx_faults;
        tx_next_attempt_us = now + 1000U;
        if (!can_recover_tx(&mController)) goto out;
        tx_recoveries++;
        tx_last_faults = reasons;
        if (tx_inflight) tx_uncertain++;
        // At-least-once: a lost completion may already have reached the peer.
        // Keep the FIFO head and retry it instead of silently dropping it.
        tx_inflight = false;
    }
    if (tx_inflight) {
        if (now >= tx_next_health_us) {
            tx_next_health_us = now + 10000U;
            // This does not time out an ACK wait. TXREQ still set is legitimate.
            can_check_tx_consistency(&mController, tx_inflight);
        }
        goto out;
    }
    if (!tx_count || now < tx_next_attempt_us) goto out;
    tx_submit_completed = tx_completed;
    can_errorcode_t rc = can_send_frame(&mController, &tx_buffer[tx_head], false);
    if (rc == CAN_ERC_NO_ERROR) {
        tx_inflight = true;
        tx_next_health_us = now + 10000U;
    } else {
        tx_retries++;
        tx_next_attempt_us = now + 1000U;
        if (now >= tx_next_health_us) {
            tx_next_health_us = now + 10000U;
            can_check_tx_consistency(&mController, tx_inflight);
        }
    }
out:
    critical_section_exit(&can_rx_lock);
}

// Status extension; counts include the in-flight head.
void pico_slcan_tx_buffer_status(void) {
    pico_slcan_tx_service();
    printf("W accepted=%lu rejected=%lu completed=%lu queued=%lu capacity=%u high_water=%lu retries=%lu inflight=%u recoveries=%lu uncertain=%lu faults=%lu pending=%lu\r",
        (unsigned long)tx_accepted, (unsigned long)tx_rejected,
        (unsigned long)tx_completed, (unsigned long)tx_count, SLCAN_TX_BUFFER_SIZE,
        (unsigned long)tx_high_water, (unsigned long)tx_retries, tx_inflight ? 1U : 0U,
        (unsigned long)tx_recoveries, (unsigned long)tx_uncertain,
        (unsigned long)tx_last_faults, (unsigned long)mController.target_specific.tx_faults);
}

ssd1306_t mDisp;

const uint pins[NUM_PINS] = {2, 4, 7, 9, 12, 13, 14, 15};

//----------------------------------------------------------------
void pico_slcan_can_init(void) {
    // This Pico SDK call binds the GPIO vector to calling the CAN mController ISR. If there are
    // then the appropriate handler has to be called.
    // other devices connected to GPIO interrupts (e.g. the WiFi chip on the Pico W)
    if (!mIrq_registerd) {
        irq_add_shared_handler(IO_IRQ_BANK0, irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        mIrq_registerd = true;
    }

    // Bind the Pico SPI interface to the hardware pin layout
    //mcp25xxfd_spi_bind_canpico(&mController.host_interface);
    //bitrate = {.profile = CAN_BITRATE_500K_75}; 
    
    if (!mController_started) {
        irq_set_enabled(IO_IRQ_BANK0, true);
        mcp25xxfd_spi_bind_pico_slcan(&mController.host_interface);
        // bitrate.profile = CAN_BITRATE_500K_75;

        // Set up the CAN mController on the CANPico using the bound SPI interface
        while (true) {
            mRc = can_setup_controller(&mController, &mBitrate, CAN_NO_FILTERS, get_can_mode(&mBitrate), CAN_OPTIONS_NONE);
            if (mRc != CAN_ERC_NO_ERROR) {
                // This can fail if the CAN transceiver isn't powered up properly. That might happen
                // if the board had 3.3V but not 5V (the transceiver needs 5V to operate). 
                //printf("CAN setup error: %d\n", mRc);
                // Try again after 1 second
                sleep_ms(1000);
            }
            else {
                break;
            }
        }
        critical_section_enter_blocking(&can_rx_lock);
        mController_started = true;
        critical_section_exit(&can_rx_lock);
    }
    pico_slcan_oled_update();
}

void pico_slcan_can_stop(void) {
    if(mController_started) {
        irq_set_enabled(IO_IRQ_BANK0, false);
        critical_section_enter_blocking(&can_rx_lock);
        mController_started = false;
        can_stop_controller(&mController);
        critical_section_exit(&can_rx_lock);
    }
    tx_buffer_clear();
    pico_slcan_oled_update();
}

bool pico_slcan_can_speedchange(uint32_t osc, uint32_t arbitration_bit_rate, double arb_sp, uint32_t data_bit_rate, double data_sp) {
    if(!mController_started) {

        double best_error = 1e9;
        bool found = false;

        for (int nbrp = 0; nbrp <= MAX_BRP; nbrp++) {
            double tq_nominal = (double)osc / (arbitration_bit_rate * (nbrp + 1));
            int tq_int = (int)round(tq_nominal);
            if (tq_int < MIN_TQ || tq_int > MAX_TQ) continue;

            for (int ntseg2 = 1; ntseg2 <= MAX_TSEG2; ntseg2++) {
                int ntseg1 = tq_int - 1 - ntseg2;
                if (ntseg1 < 1 || ntseg1 > MAX_TSEG1) continue;

                double sp_nominal = (1.0 + ntseg1) / (1.0 + ntseg1 + ntseg2);
                if (fabs(sp_nominal - arb_sp) > 0.2) continue;

                if(data_bit_rate == 0) { // for CAN
                    double error = fabs(sp_nominal - arb_sp);

                    if (error < best_error) {
                        best_error = error;
                        mBitrate.osc = osc;
                        mBitrate.nominal_bit_rate = arbitration_bit_rate;
                        mBitrate.nominal_sp = arb_sp;
                        mBitrate.data_bit_rate = data_bit_rate;
                        mBitrate.data_sp = data_sp;

                        mBitrate.nbrp = nbrp;
                        mBitrate.ntseg1 = ntseg1 - 1; // 0 index
                        mBitrate.ntseg2 = ntseg2 - 1; // 0 index
                        mBitrate.nsjw = ntseg2 - 1;  // 0 index

                        mBitrate.dbrp = 0;
                        mBitrate.dtseg1 = 0;
                        mBitrate.dtseg2 = 0;
                        mBitrate.dsjw = 0;
                        mBitrate.tcdo = 0; // NOTE tdco not use / Auto

                        found = (fabs(sp_nominal - arb_sp) < ALLOWABLE_SP_ERROR);
                    }
                } else { // for CANFD
                    int dbrp = nbrp;
                    if (dbrp < 0 || dbrp > 255) continue;

                    double tq_data_nominal = (double)osc / (data_bit_rate * (dbrp + 1));
                    int tq_data_int = (int)round(tq_data_nominal);
                    if (tq_data_int < MIN_TQ || tq_data_int > MAX_TQ) continue;

                    for (int dtseg2 = 1; dtseg2 <= 127; dtseg2++) {
                        int dtseg1 = tq_data_int - 1 - dtseg2;
                        if (dtseg1 < 1 || dtseg1 > 255) continue;

                        double sp_data = (1.0 + dtseg1) / (1.0 + dtseg1 + dtseg2);
                        double error = fabs(sp_nominal - arb_sp) + fabs(sp_data - data_sp);

                        if (error < best_error) {
                            best_error = error;
                            mBitrate.osc = osc;
                            mBitrate.nominal_bit_rate = arbitration_bit_rate;
                            mBitrate.nominal_sp = arb_sp;
                            mBitrate.data_bit_rate = data_bit_rate;
                            mBitrate.data_sp = data_sp;

                            mBitrate.nbrp = nbrp;
                            mBitrate.ntseg1 = ntseg1 - 1;  // 0 index
                            mBitrate.ntseg2 = ntseg2 - 1;  // 0 index
                            mBitrate.nsjw = ntseg2 - 1;  // 0 index

                            mBitrate.dbrp = dbrp;
                            mBitrate.dtseg1 = dtseg1 - 1;  // 0 index
                            mBitrate.dtseg2 = dtseg2 - 1;  // 0 index
                            mBitrate.dsjw = dtseg2 - 1;  // 0 index
                            mBitrate.tcdo = 0; // NOTE tdco not use / Auto

                            found = (fabs(sp_nominal - arb_sp) < ALLOWABLE_SP_ERROR) && (fabs(sp_data - data_sp) < ALLOWABLE_SP_ERROR);
                        }
                    }
                }
                if (found) break;
            }
            if (found) break;
        }

        pico_slcan_oled_update();
        return true;
    } else {
        return false;
    }
}

/**
 * @brief CANビットレートをカスタム設定します / Set custom CAN bitrate parameters.
 *
 * @details
 * 日本語:
 * この関数はCAN FDのノーマルフェーズとデータフェーズのビットタイミングを設定し、
 * グローバル構造体 mBitrate に保存します。コントローラが未起動の場合のみ設定可能です。
 *
 * English:
 * This function configures CAN FD nominal and data phase bit timing parameters
 * and stores them in the global mBitrate structure. It can only be applied when
 * the controller is not started.
 *
 * @param nbrp
 * 日本語: ノーマルフェーズのビットレートプリスケーラ (0 = /1)
 * English: Nominal phase baud rate prescaler (0 = divide by 1)
 *
 * @param ntseg1
 * 日本語: ノーマルフェーズのタイムセグメント1 (TSEG1) - 1
 * English: Nominal phase time segment 1 (TSEG1) minus 1
 *
 * @param ntseg2
 * 日本語: ノーマルフェーズのタイムセグメント2 (TSEG2) - 1
 * English: Nominal phase time segment 2 (TSEG2) minus 1
 *
 * @param nsjw
 * 日本語: ノーマルフェーズの同期ジャンプ幅 (SJW) - 1
 * English: Nominal phase synchronization jump width (SJW) minus 1
 *
 * @param dbrp
 * 日本語: データフェーズのビットレートプリスケーラ (0 = /1)
 * English: Data phase baud rate prescaler (0 = divide by 1)
 *
 * @param dtseg1
 * 日本語: データフェーズのタイムセグメント1 (TSEG1) - 1
 * English: Data phase time segment 1 (TSEG1) minus 1
 *
 * @param dtseg2
 * 日本語: データフェーズのタイムセグメント2 (TSEG2) - 1
 * English: Data phase time segment 2 (TSEG2) minus 1
 *
 * @param dsjw
 * 日本語: データフェーズの同期ジャンプ幅 (SJW) - 1
 * English: Data phase synchronization jump width (SJW) minus 1
 *
 * @param tcdo
 * 日本語: CAN FD送信遅延補償オフセット。推奨値は以下の式:
 * @code
 * tcdo ≈ 1 + dtseg1
 * @endcode
 * English: Transmitter Delay Compensation Offset for CAN FD.
 * Recommended value:
 * @code
 * tcdo ≈ SyncSeg + dtseg1 = 1 + dtseg1
 * @endcode
 * This positions the compensation point near the sample point in the data phase.
 *
 * @return
 * 日本語: true 設定成功（コントローラ未起動時）、false 設定不可（起動済み）
 * English: true if applied successfully (controller not started), false if controller is already running.
 *
 * @note
 * 日本語: パラメータはハードウェア仕様に従って設定してください。
 * English: Ensure all parameters comply with the CAN controller hardware specification.
 */
bool pico_slcan_can_speedchange_custom(uint8_t nbrp, uint8_t ntseg1, uint8_t ntseg2, uint8_t nsjw, uint8_t dbrp, uint8_t dtseg1, uint8_t dtseg2, uint8_t dsjw, uint8_t tcdo)
{
    if (!mController_started) {
        mBitrate.profile = CAN_BITRATE_CUSTOM;
        mBitrate.nominal_bit_rate = CUSTOM_ALL;
        //mBitrate.nominal_sp = 0;
        mBitrate.data_bit_rate = CUSTOM_ALL;
        //mBitrate.data_sp = 0;
        mBitrate.nbrp    = nbrp;
        mBitrate.ntseg1  = ntseg1;
        mBitrate.ntseg2  = ntseg2;
        mBitrate.nsjw    = nsjw;
        mBitrate.dbrp    = dbrp;
        mBitrate.dtseg1  = dtseg1;
        mBitrate.dtseg2  = dtseg2;
        mBitrate.dsjw    = dsjw;
        mBitrate.tcdo    = tcdo;  // NOTE tdco not use / Auto

        pico_slcan_oled_update();
        return true;
    } else {
        return false;
    }
}

bool TIME_CRITICAL pico_slcan_send_canmsg(char *msg, bool rtr, bool ext) {
    if (!msg || !mController_started) return false;
    pico_slcan_tx_service();
    if (tx_count == SLCAN_TX_BUFFER_SIZE) {
        tx_rejected++;
        return false;
    }
    led_on();

    can_frame_t tx_frame;
    uint32_t queued_ok = 0;
    int msg_id = 0;
    int msg_ide = 0;
    int msg_len = 0;
    uint8_t data[64];

        if (!msg) return false;

        // 初期化
        tx_frame.uref = can_uref_null;
        tx_frame.flags = 0;
        tx_frame.id_filter = 0;
        memset(tx_frame.fd_data, 0, sizeof(tx_frame.fd_data));

        char cmd = msg[0];

        if (cmd == 'T' || cmd == 'R') {
            // 拡張CAN (29bit)
            tx_frame.canid.id = hex_to_uint(&msg[1], 8);
            tx_frame.canid.id |= (1U << CAN_ID_EXT_BIT); // IDEビットをセット
            tx_frame.flags |= (cmd == 'R') ? CAN_FRAME_FLAG_RTR : 0;

            uint8_t dlc_raw = hex_to_uint(&msg[9], 1);
            tx_frame.dlc = (dlc_raw < 16) ? dlc_raw : 0;
            uint8_t len = dlc_to_len[tx_frame.dlc];

            const char *data_ptr = &msg[10];
            uint8_t *dst = (uint8_t *)tx_frame.fd_data;
            for (int i = 0; i < len; i++) {
                dst[i] = hex_to_uint(&data_ptr[i * 2], 2);
            }
        }
        else if (cmd == 't' || cmd == 'r') {
            // 標準CAN (11bit)
            tx_frame.canid.id = hex_to_uint(&msg[1], 3);
            tx_frame.flags |= (cmd == 'r') ? CAN_FRAME_FLAG_RTR : 0;

            uint8_t dlc_raw = hex_to_uint(&msg[4], 1);
            tx_frame.dlc = (dlc_raw < 16) ? dlc_raw : 0;
            uint8_t len = dlc_to_len[tx_frame.dlc];

            const char *data_ptr = &msg[5];
            uint8_t *dst = (uint8_t *)tx_frame.fd_data;
            for (int i = 0; i < len; i++) {
                dst[i] = hex_to_uint(&data_ptr[i * 2], 2);
            }
        }
        else if (cmd == 'D' || cmd == 'd') {
            // CAN FDフレーム
            tx_frame.flags |= CAN_FRAME_FLAG_FDF;
            tx_frame.canid.id = (cmd == 'D') ? hex_to_uint(&msg[1], 8) : hex_to_uint(&msg[1], 3);
            if (cmd == 'D') tx_frame.canid.id |= (1U << CAN_ID_EXT_BIT); // FD拡張IDならIDEビットセット

            int dlc_pos = (cmd == 'D') ? 9 : 4;
            uint8_t dlc_raw = hex_to_uint(&msg[dlc_pos], 1);
            tx_frame.dlc = (dlc_raw < 16) ? dlc_raw : 0;
            uint8_t len = dlc_to_len[tx_frame.dlc];
            
            // BRSはセットなし/ESIフラグ
            // TODO ESIフラグ対応が必要
            //if (msg[dlc_pos + 2] == '1') tx_frame.flags |= CAN_FRAME_FLAG_ESI;

            const char *data_ptr = &msg[dlc_pos + 1];
            uint8_t *dst = (uint8_t *)tx_frame.fd_data;
            for (int i = 0; i < len; i++) {
                dst[i] = hex_to_uint(&data_ptr[i * 2], 2);
            }
        }
        else if (cmd == 'B' || cmd == 'b') {
            // CAN FDフレーム
            tx_frame.flags |= CAN_FRAME_FLAG_FDF;
            tx_frame.canid.id = (cmd == 'B') ? hex_to_uint(&msg[1], 8) : hex_to_uint(&msg[1], 3);
            if (cmd == 'B') tx_frame.canid.id |= (1U << CAN_ID_EXT_BIT); // FD拡張IDならIDEビットセット

            int dlc_pos = (cmd == 'B') ? 9 : 4;
            uint8_t dlc_raw = hex_to_uint(&msg[dlc_pos], 1);
            tx_frame.dlc = (dlc_raw < 16) ? dlc_raw : 0;
            uint8_t len = dlc_to_len[tx_frame.dlc];

            // TODO ESIフラグ対応が必要
            // BRSはセット/ESIフラグ
            tx_frame.flags |= CAN_FRAME_FLAG_BRS;
            //if (msg[dlc_pos + 2] == '1') tx_frame.flags |= CAN_FRAME_FLAG_ESI;

            const char *data_ptr = &msg[dlc_pos + 1];
            uint8_t *dst = (uint8_t *)tx_frame.fd_data;
            for (int i = 0; i < len; i++) {
                dst[i] = hex_to_uint(&data_ptr[i * 2], 2);
            }
        }
        else {
            return false; // 未対応コマンド
        }

    tx_buffer[tx_tail] = tx_frame;
    if (++tx_tail == SLCAN_TX_BUFFER_SIZE) tx_tail = 0;
    tx_count++;
    tx_accepted++;
    if (tx_count > tx_high_water) tx_high_water = tx_count;
    pico_slcan_tx_service();
    led_off();
    return true; // Accepted into the FIFO; completion is asynchronous.
} // send_canmsg()

bool TIME_CRITICAL pico_slcan_recv_canmsg(can_frame_t *frame) {
    can_rx_event_t rx_event;
    can_rx_event_t *e = &rx_event;
    critical_section_enter_blocking(&can_rx_lock);
    bool received = mController_started && can_recv(&mController, e);
    critical_section_exit(&can_rx_lock);
    if (received && can_event_is_frame(e)) {
        led_on();

        can_frame_t *f = can_event_get_frame(e);

        frame->canid.id = f->canid.id;
        frame->dlc = f->dlc;

        // flags初期化
        frame->flags = 0;
        if (f->flags & CAN_FRAME_FLAG_RTR) frame->flags |= CAN_FRAME_FLAG_RTR;
        if (f->flags & CAN_FRAME_FLAG_FDF) frame->flags |= CAN_FRAME_FLAG_FDF;
        if (f->flags & CAN_FRAME_FLAG_ESI) frame->flags |= CAN_FRAME_FLAG_ESI;
        if (f->flags & CAN_FRAME_FLAG_BRS) frame->flags |= CAN_FRAME_FLAG_BRS;

        // DLCに基づくデータコピー
        uint8_t len = (f->dlc < 16) ? dlc_to_len[f->dlc] : 0;
        memcpy(frame->fd_data, f->fd_data, len);

        led_off();
        return true;
    }
    return false;
}

bool pico_slcan_set_ac(char *msg) {
    //TODO レジスター設定実体を記載必要
    return false;
}

bool pico_slcan_set_am(char *msg) {
    //TODO レジスター設定実体を記載必要
    return false;
}

/**
 * @brief CANコントローラのステータスを取得し、SJA1000互換ビットを判定します / Read CAN controller status and evaluate SJA1000-compatible bits.
 *
 * @details
 * 日本語:
 * この関数は can_get_status() を呼び出し、MCP2518FDのレジスタ値を取得します。
 * 取得した値を status 構造体に格納し、SJA1000互換ビットを判定します。
 * ビット定義:
 * [0]=RX FIFO Full, [1]=TX FIFO Full, [2]=Error Warning,
 * [3]=Data Overrun, [4]=未使用, [5]=Error Passive, [6]=Arbitration Lost, [7]=Bus Error
 *
 * English:
 * This function calls can_get_status(), stores MCP2518FD register values in the status struct,
 * and evaluates SJA1000-compatible bits:
 * [0]=RX FIFO Full, [1]=TX FIFO Full, [2]=Error Warning,
 * [3]=Data Overrun, [4]=Not used, [5]=Error Passive, [6]=Arbitration Lost, [7]=Bus Error
 *
 * @param status
 * 日本語: CANステータス構造体へのポインタ（結果を格納）
 * English: Pointer to can_status_t struct to store raw register values.
 *
 * @return
 * 日本語: true = 正常, false = 異常あり
 * English: true if status is normal, false if any error/warning condition is present.
 *
 * @note
 * 日本語: CRC付き読み出しでErrata対策済み。
 * English: Uses CRC-protected reads for errata compliance.
 */
bool pico_slcan_can_statusread(can_status_t *status) {
    if (status == NULL) {
        return false; // NULLは異常扱い
    }

    // MCP2518FDのレジスタ値を取得
    *status = can_get_status(&mController);

    // ビット判定
    bool rx_fifo_full     = (status->status_c1fifosta1 & (1U << 8)) != 0;  // FIFOFUL
    bool tx_fifo_full     = (status->status_c1txqsta    & (1U << 0)) != 0; // TXQNIF
    bool error_warning    = (status->status             & ((1U << 21) | (1U << 20))) != 0; // TXBP/RXBP
    bool data_overrun     = (status->status_c1rxovif    & (1U << 0)) != 0; // RXOVIF
    bool error_passive    = error_warning; // 同じ判定
    bool arbitration_lost = (status->status_c1bdiag1    & (1U << 2)) != 0; // ALF
    bool bus_error        = (status->status_c1bdiag1    & (1U << 0)) != 0; // BEF

    return true;
}


/**
 * @brief GPIO入力状態を読み取り、HEX文字列に変換します / Read GPIO states and convert to HEX string.
 *
 * @details
 * 日本語:
 * この関数は指定されたGPIOピン群の入力状態を読み取り、8ビットの値にまとめて
 * 16進文字列として出力します。バッファサイズが不足している場合は失敗します。
 *
 * English:
 * This function reads the input states of configured GPIO pins, combines them
 * into an 8-bit value, and converts it to a hexadecimal string representation.
 * Returns false if the provided buffer is too small.
 *
 * @param[out] version
 * 日本語: HEX文字列を格納するバッファへのポインタ。最低3バイト必要（"FF\0"）。
 * English: Pointer to the buffer where the HEX string will be stored.
 * Minimum size is 3 bytes ("FF\0").
 *
 * @param[in] buf_size
 * 日本語: versionバッファのサイズ（バイト単位）。
 * English: Size of the version buffer in bytes.
 *
 * @return
 * 日本語: true 読み取り成功、false バッファ不足。
 * English: true if successful, false if buffer size is insufficient.
 *
 * @note
 * 日本語: NUM_PINSの値に応じて必要なバッファサイズを確保してください。
 * English: Ensure the buffer size is sufficient based on NUM_PINS.
 *
 * 使用例 / Example:
 * @code
 * char ver_str[3];
 * if (pico_slcan_versionread(ver_str, sizeof(ver_str))) {
 *     printf("Version: %s\n", ver_str);
 * } else {
 *     printf("Buffer too small!\n");
 * }
 * @endcode
 */
bool pico_slcan_versionread(char *version, size_t buf_size) {
    uint8_t value_gpio = 0;
    const uint8_t value_const = SOFTWARE_VERSION; // 下位2桁に入れる定数

    // GPIO状態を読み取り、8ビット値に変換
    for (int i = 0; i < NUM_PINS; i++) {
        if (gpio_get(pins[i])) {
            value_gpio |= (1 << i);
        }
    }

    // 16ビット値を作成（上位: GPIO、下位: 定数）
    uint16_t combined = ((uint16_t)value_gpio << 8) | value_const;

    // 必要サイズを計算（16ビットなら "FFFF" + '\0' → 5バイト）
    size_t required = 4 + 1; // 4桁HEX + 終端
    if (buf_size < required) {
        return false; // バッファ不足
    }

    // snprintfで安全に書き込み
    snprintf(version, buf_size, "%04X", combined);

    return true;
}

void pico_slcan_oled_update(void) {
    if (mBitrate.profile != CAN_BITRATE_CUSTOM) {
        // バッファ作成
        char line1[20], line2[20], line3[40], line4[40];
        snprintf(line1, sizeof(line1), "Arb:  %s", get_label(mArbitration_bit_rate, arb_map, sizeof(arb_map)/sizeof(arb_map[0])));
        snprintf(line2, sizeof(line2), "Data: %s", get_label(mData_bit_rate, data_map, sizeof(data_map)/sizeof(data_map[0])));
        snprintf(line3, sizeof(line3), "Serial: %04X", mSerial);
        snprintf(line4, sizeof(line4), "Copyright JP WORKS");

        // OLEDに表示
        ssd1306_clear(&mDisp);
        ssd1306_draw_string(&mDisp, 4, 16, 2, line1);
        ssd1306_draw_string(&mDisp, 4, 32, 2, line2);
        ssd1306_draw_string(&mDisp, 4, 48, 1, line3);
        ssd1306_draw_string(&mDisp, 4, 56, 1, line4);
    } else {
        // バッファ作成
        char line1[40], line2[40], line3[40], line4[40], line5[40], line6[40];
        snprintf(line1, sizeof(line1), "BRP:%d/TSEG1:%d", mBitrate.nbrp, mBitrate.ntseg1);
        snprintf(line2, sizeof(line2), "TSEG2:%d/SJW:%d", mBitrate.ntseg2, mBitrate.dsjw);
        snprintf(line3, sizeof(line3), "DBRP:%d/DTSEG1:%d", mBitrate.dbrp, mBitrate.dtseg1);
        snprintf(line4, sizeof(line4), "DTSEG2:%d/DSJW:%d", mBitrate.dtseg2, mBitrate.dsjw);
        snprintf(line5, sizeof(line5), "TCDO:%d", mBitrate.tcdo);
        snprintf(line6, sizeof(line6), "Copyright JP WORKS");

        // OLEDに表示
        ssd1306_clear(&mDisp);
        ssd1306_draw_string(&mDisp, 4, 16, 1, line1);
        ssd1306_draw_string(&mDisp, 4, 24, 1, line2);
        ssd1306_draw_string(&mDisp, 4, 32, 1, line3);
        ssd1306_draw_string(&mDisp, 4, 40, 1, line4);
        ssd1306_draw_string(&mDisp, 4, 48, 1, line5);
        ssd1306_draw_string(&mDisp, 4, 56, 1, line6);
    }
    if(get_can_mode(&mBitrate) == CAN_MODE_NORMAL) {
        if(mController_started) {
            ssd1306_draw_string(&mDisp, 4, 0, 2, "CAN:NOR_OP");
        } else {
            ssd1306_draw_string(&mDisp, 4, 0, 2, "CAN:NOR_CL");
        }
    } else {
        if(mController_started) {
            ssd1306_draw_string(&mDisp, 4, 0, 2, "CAN:FD_OP");
        } else {
            ssd1306_draw_string(&mDisp, 4, 0, 2, "CAN:FD_CL");
        }
    }
    ssd1306_show(&mDisp);
}

uint32_t pico_slcan_get_osc(void) {
    switch (HW_OSC_PLL) {
        case OSC_40MHz:
            return 40000000;
            break;
        case OSC_20MHz:
            return 20000000;
            break;
    }
    //TODO multi OSC support
    /*case OSC_40MHz_DIVIDED_BY_2:
    case OSC_20MHz_DIVIDED_BY_2:
    case OSC_4MHz_DIVIDED_BY_2:
    case OSC_4MHz10xPLL:
    case OSC_4MHz10xPLL_DIVIDED_BY_2:*/
}

//----------------------------------------------------------------

// Utility function to print a CAN frame to stdout
void print_frame(can_frame_t *f, uint32_t timestamp)
{
    uint8_t len = can_frame_get_data_len(f);
    printf(can_frame_is_extended(f) ? "0x%08x " : "0x%03x ", can_frame_get_arbitration_id(f));
    if (can_frame_is_remote(f)) {
        printf("R"); // Remote frame doesn't have any data, len will be zero
    }
    for (uint32_t i = 0; i < len; i++) {
        printf("%02x", can_frame_get_data(f)[i]);
    }
    printf(" (%d)\n", timestamp);
}

// Wrapper to parameterize the API's IRQ handler with the handle to the single CAN
// mController on the CANPico (the structure is defined above and configured by
// binding to the SPI port and pins used on the CANPico).
//
// NB: The RP2040 with execute-in-place (XIP) flash suffers from very long cache
// miss delays, and these can cause significant delays to the interrupt handler.
// Generally the entire chain of interrupt handling (from vector table to first-level
// handler to device-specific handlers) should be located in RAM to avoid these
// cache delays. The CAN drivers are allocated to RAM with the "TIME_CRITICAL"
// attribute that causes the compiler to place the function in RAM.
//
// This problem of delaying interrupts also extends to critical sections: code that
// disables an interrupt around some function also should execute from RAM while in
// that function to avoid delaying an urgent interrupt handler. This "priority
// inversion" is discussed further here:
//
// https://kentindell.github.io/2021/03/05/pico-priority-inversion/

void TIME_CRITICAL irq_handler(void)
{
    // Work out if this interrupt is from the the MCP25xxFD. The bound interface
    // defines the pin used for the interrupt line from the CAN mController.
    uint8_t spi_irq = mController.host_interface.spi_irq;
    uint32_t events = gpio_get_irq_event_mask(spi_irq); 

    if (events & GPIO_IRQ_LEVEL_LOW) {
        critical_section_enter_blocking(&can_rx_lock);
        mcp25xxfd_irq_handler(&mController);
        critical_section_exit(&can_rx_lock);
    }
}

void led_init(void)
{
#ifndef PICO_DEFAULT_LED_PIN
    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed");
    }
#else
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
}

void TIME_CRITICAL led_on(void)
{
#ifndef PICO_DEFAULT_LED_PIN
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
#else
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
}

void TIME_CRITICAL led_off(void)
{
#ifndef PICO_DEFAULT_LED_PIN
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
#else
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
#endif
}

//----------------------------------------------------------------

can_mode_t get_can_mode(const can_bitrate_t *bitrate)
{
    if (bitrate == NULL) {
        return CAN_MODE_NORMAL; // デフォルト
    }

    switch (bitrate->profile) {
        case CAN_BITRATE_PRESET:
        if (bitrate->data_bit_rate == 0) {
            return CAN_MODE_NORMAL;
        } else {
            return CAN_MODE_NORMAL_FD;
        }
        case CAN_BITRATE_CUSTOM:
            if (bitrate->dbrp == 0 &&
                bitrate->dtseg1 == 0 &&
                bitrate->dtseg2 == 0 &&
                bitrate->dsjw == 0 &&
                bitrate->tcdo == 0) {
                return CAN_MODE_NORMAL;
            } else {
                return CAN_MODE_NORMAL_FD;
            }
        default:
            return CAN_MODE_NORMAL;
    }
}

void oled_gpios_setup(void) {
    i2c_init(i2c0, 400000);
    gpio_set_function(16, GPIO_FUNC_I2C);
    gpio_set_function(17, GPIO_FUNC_I2C);
    gpio_pull_up(16);
    gpio_pull_up(17);
}

// ビットレート値からラベルを取得
const char* get_label(uint32_t value, RateMap *map, size_t map_size) {
    for (size_t i = 0; i < map_size; i++) {
        if (map[i].value == value) {
            return map[i].label;
        }
    }
    return "Cust"; // 見つからなければカスタム
}

//----------------------------------------------------------------

void setup() {
    critical_section_init(&can_rx_lock);
    stdio_init_all();
    led_init();
    oled_gpios_setup();

    // GPIO初期化（入力モード）
    // for read_version
    for (int i = 0; i < NUM_PINS; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], false); // false = input
        gpio_pull_down(pins[i]);      // 必要ならプルダウン
    }

    // Light on
    led_on();

    register_can_init(pico_slcan_can_init);
    register_can_stop(pico_slcan_can_stop);
    register_can_speedchange(pico_slcan_can_speedchange);
    register_can_speedchange_custom(pico_slcan_can_speedchange_custom);
    register_send_canmsg(pico_slcan_send_canmsg);
    register_recv_canmsg(pico_slcan_recv_canmsg);
    register_set_ac(pico_slcan_set_ac);
    register_set_am(pico_slcan_set_am);
    register_can_statusread(pico_slcan_can_statusread);
    register_versionread(pico_slcan_versionread);
    register_oled_update(pico_slcan_oled_update);
    register_get_osc(pico_slcan_get_osc);

    mDisp.external_vcc=false;
    ssd1306_init(&mDisp, 128, 64, 0x3C, i2c0);
    ssd1306_clear(&mDisp);
    ssd1306_draw_string(&mDisp, 0, 0, 2, "Booting...");
    ssd1306_show(&mDisp);

    mBitrate.profile = CAN_BITRATE_PRESET;
    mBitrate.nominal_sp = mArb_SP;
    mBitrate.data_sp = mData_SP;

    slcan_setup();

    // Light off
    led_off();
}

void TIME_CRITICAL loop_core0() {
    pico_slcan_tx_service();
    transfer_tty2can();
    pico_slcan_tx_service();
}

void TIME_CRITICAL loop_core1() {
    transfer_can2tty();
}

int main() {
     setup();

    multicore_launch_core1(main_core1);

    while (true) {
        loop_core0();
    }
}

void main_core1() {
    while (true) {
        loop_core1();
    }
}