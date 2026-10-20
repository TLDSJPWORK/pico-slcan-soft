/**
 * Copyright 2025 JP WORKS. All rights reserved.
 *
 * Implementation of SLCAN using Raspberry Pi Pico and MCP251XFD to receive CAN FD.
 * 
 */

#include "slcan.h"

uint32_t mSerial = 0xFFFF;
uint32_t mOSC = 40000000;
uint32_t mArbitration_bit_rate = DEFAULT_ARBITRATION_BIT_RATE;
const double mArb_SP = DEFAULT_ARB_SP;
uint32_t mData_bit_rate = DEFAULT_DATA_BIT_RATE;
const double mData_SP = DEFAULT_DATA_SP;

// 関数ポインタを定義
void (*can_init_func)(void) = NULL;
void (*can_stop_func)(void) = NULL;
bool (*can_speedchange_func)(uint32_t, uint32_t, double, uint32_t, double) = NULL;
bool (*can_speedchange_custom_func)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) = NULL;

/**
 * @brief SLCAN形式のCANメッセージを送信します
 *        Send a CAN message in SLCAN format.
 *
 * 日本語:
 * この関数はASCII形式のSLCANコマンド文字列を解析し、CANメッセージを送信します。
 * RTR（リモートフレーム）や拡張IDの指定に対応します。
 *
 * English:
 * This function parses an ASCII SLCAN command string and sends a CAN message.
 * Supports Remote Transmission Request (RTR) and extended CAN IDs.
 *
 * @param msg
 * 日本語: 送信するSLCANコマンド文字列（ASCII形式）
 * English: The SLCAN command string to send (ASCII format).
 *
 * @param rtr
 * 日本語: trueの場合、リモートフレームとして送信
 * English: If true, send as a Remote Transmission Request (RTR) frame.
 *
 * @param ext
 * 日本語: trueの場合、拡張CAN ID（29ビット）を使用
 * English: If true, use an extended CAN ID (29-bit).
 *
 * @return
 * 日本語: TX FIFOへの受付成功ならtrue、満杯または受付不可ならfalse
 * English: Returns true on TX FIFO acceptance, false when full or unavailable.
 *
 * @note
 * 日本語: この関数はCANコントローラが初期化済みである必要があります。
 * English: The CAN controller must be initialized before calling this function.
 */
bool (*send_canmsg_func)(char *, bool, bool) = NULL;

bool (*recv_canmsg_func)(can_frame_t *) = NULL;
bool (*set_ac_func)(char *) = NULL;
bool (*set_am_func)(char *) = NULL;
bool (*can_statusread_func)(can_status_t *) = NULL;
bool (*versionread_func)(char *, size_t) = NULL;
void (*oled_update_func)(void) = NULL;
uint32_t (*get_osc_func)(void) = NULL;

bool working           = false;
bool timestamp         = false;
bool cr                = false;

#define HEXVAL "0123456789ABCDEF"

//----------------------------------------------------------------

uint32_t millis() {
    return to_ms_since_boot(get_absolute_time());
}

//----------------------------------------------------------------

// 初期化時に代入
void register_can_init(void (*func)(void)) {
    can_init_func = func;
}

// 初期化時に代入
void register_can_stop(void (*func)(void)) {
    can_stop_func = func;
}

// 初期化時に代入
void register_can_speedchange(bool (*func)(uint32_t, uint32_t, double, uint32_t, double)) {
    can_speedchange_func = func;
}

// 初期化時に代入
void register_can_speedchange_custom(bool (*func)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t)) {
    can_speedchange_custom_func = func;
}

/* 使い方
// コールバック関数
void my_speed_handler(int speed) {
    printf("CAN speed changed to %d kbps\n", speed);
}

int main() {
    // コールバック登録
    register_can_speedchange(my_speed_handler);

    // 速度変更イベントが発生したら呼び出す
    if (can_speedchange_func) {
        can_speedchange_func(500); // 500kbpsに変更
    }
}*/

// 初期化時に代入
void register_send_canmsg(bool (*func)(char *, bool, bool)) {
    send_canmsg_func = func;
}

// 初期化時に代入
void register_recv_canmsg(bool (*func)(can_frame_t *)) {
    recv_canmsg_func = func;
}

// 初期化時に代入
void register_set_ac(bool (*func)(char *)) {
    set_ac_func = func;
}

// 初期化時に代入
void register_set_am(bool (*func)(char *)) {
    set_am_func = func;
}

void register_can_statusread(bool (*func)(can_status_t *)) {
    can_statusread_func = func;
}

void register_versionread(bool (*func)(char *, size_t)) {
    versionread_func = func;
}

void register_oled_update(void (*func)(void)) {
    oled_update_func = func;
}

void register_get_osc(uint32_t (*func)(void)) {
    get_osc_func = func;
}

//----------------------------------------------------------------

void slcan_ack()
{
  printf("\r");
} // slcan_ack()

void slcan_nack()
{
  printf("\a");
} // slcan_nack()

bool pars_slcancmd_speedchange_custom(char *buf) {
  size_t len = strlen(buf);

  // NULLチェック
  if (!buf) {
      return false;
  }

  // 固定長チェック（1文字 + 9引数 → 18文字 + 終端1文字）
  if (len != 1 + 18 + 1) {
      slcan_nack();
      return false;
  }

  if (can_speedchange_custom_func && !working) {
    int args[9] = {0};

    for (size_t i = 1; i < 19; i += 2) {
      char temp[3] = {buf[i], buf[i + 1], '\0'};

      // 10進数文字チェック
      if (!isdigit(temp[0]) || !isdigit(temp[1])) {
        return false;
      }

      char *endptr;
      long val = strtol(temp, &endptr, 10);

      // 変換エラーチェック（0～99）
      if (*endptr != '\0' || val < 0 || val > 99) {
        return false;
      }

      args[(i-1) / 2] = (int)val;
    }

    // 関数呼び出し
    if (can_speedchange_custom_func(
            args[0], args[1], args[2], args[3], args[4],
            args[5], args[6], args[7], args[8])) {
      return true;
    } else {
      return false;
    }
  }
  return false;
}


//----------------------------------------------------------------

extern void pico_slcan_tx_buffer_status(void);

void pars_slcancmd(char *buf)
{                           // LAWICEL PROTOCOL
  switch (buf[0]) {
    case 'w': pico_slcan_tx_buffer_status(); break;
    case 'O':               // OPEN CAN
      working=true;
      if (can_init_func) {
          can_init_func(); // 登録された関数を呼び出す
      }
      slcan_ack();
      break;
    case 'C':               // CLOSE CAN
      working=false;
      if (can_stop_func) {
          can_stop_func(); // 登録された関数を呼び出す
      }
      slcan_ack();
      break;
    case 't':               // SEND STD FRAME
      // send_canmsg(buf,false,false);
      if (send_canmsg_func && working && send_canmsg_func(buf,false,false)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'T':               // SEND EXT FRAME
      if (send_canmsg_func && working && send_canmsg_func(buf,false,true)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'r':               // SEND STD RTR FRAME
      if (send_canmsg_func && working && send_canmsg_func(buf,true,false)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'R':               // SEND EXT RTR FRAME
      if (send_canmsg_func && working && send_canmsg_func(buf,true,true)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'd':               // SEND NORMAL FD FRAME
      // send_canmsg(buf,false,false);
      if (send_canmsg_func && working && send_canmsg_func(buf,false,false)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'D':               // SEND EXT FD FRAME
      if (send_canmsg_func && working && send_canmsg_func(buf,false,true)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'b':               // SEND NORMAL FD FRAME with BRS
      if (send_canmsg_func && working && send_canmsg_func(buf,false,false)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'B':               // SEND EXT FD FRAME with BRS
      if (send_canmsg_func && working && send_canmsg_func(buf,false,true)) {
          slcan_ack(); // Accepted into the TX queue, not yet transmitted on the bus.
      } else {
          slcan_nack();
      }
      break;
    case 'Z':               // ENABLE TIMESTAMPS
      switch (buf[1]) {
        case '0':           // TIMESTAMP OFF  
          timestamp = false;
          slcan_ack();
          break;
        case '1':           // TIMESTAMP ON
          timestamp = true;
          slcan_ack();
          break;
        default:
          break;
      }
      break;
    case 'M':               ///set ACCEPTANCE CODE ACn REG
      if (set_ac_func && !working) {
        set_ac_func(buf); // 登録された関数を呼び出す
        slcan_ack();
      } else {
        slcan_nack();
      }
      break;
    case 'm':               // set ACCEPTANCE CODE AMn REG
      if (set_am_func && !working) {
        set_am_func(buf); // 登録された関数を呼び出す
        slcan_ack();
      } else {
        slcan_nack();
      }
      break;
    case 's':               // CUSTOM CAN bit-rate
      if (pars_slcancmd_speedchange_custom(buf)) {
        mArbitration_bit_rate = 0;
        mData_bit_rate = 0;
        slcan_ack();
      } else {
        slcan_nack();
      }
      break;
    case 'S':               // CAN bit-rate
      if (mArbitration_bit_rate == CUSTOM_ALL) {
        mArbitration_bit_rate = DEFAULT_ARBITRATION_BIT_RATE;
        mData_bit_rate = DEFAULT_DATA_BIT_RATE;
      }
      uint32_t prev_arbitration_bit_rate = mArbitration_bit_rate; 
      switch (buf[1]) {
        case '0':           // 10k
          mArbitration_bit_rate = 10 * 1000;
          break;
        case '1':           // 20k
          mArbitration_bit_rate = 20 * 1000;
          break;
        case '2':           // 50k
          mArbitration_bit_rate = 50 * 1000;
          break;
        case '3':           // 100k
          mArbitration_bit_rate = 100 * 1000;
          break;
        case '4':           // 125k
          mArbitration_bit_rate = 125 * 1000;
          break;
        case '5':           // 250k
          mArbitration_bit_rate = 250 * 1000;
          break;
        case '6':           // 500k
          mArbitration_bit_rate = 500 * 1000;
          break;
        case '7': // 800k
          mArbitration_bit_rate = 800 * 1000;
          break;
        case '8':           // 1000k
          mArbitration_bit_rate = 1000 * 1000;
          break;
        default:
          break;
      }
      if (can_speedchange_func && !working) {
          if (can_speedchange_func(mOSC, mArbitration_bit_rate, mArb_SP, mData_bit_rate, mData_SP)) {
            slcan_ack();
          } else {
            mArbitration_bit_rate = prev_arbitration_bit_rate;
            slcan_nack();
          }
      }
      break;
    case 'Y':               // STATUS FLAGS
      if (mArbitration_bit_rate == CUSTOM_ALL) {
        mArbitration_bit_rate = DEFAULT_ARBITRATION_BIT_RATE;
        mData_bit_rate = DEFAULT_DATA_BIT_RATE;
      }
      uint32_t prev_data_bit_rate = mData_bit_rate;
      switch (buf[1]) {
        case '0':           // NONE
          mData_bit_rate = 0;
          break;
        case '1':           // 1M
          mData_bit_rate = 1 * 1000 * 1000;
          break;
        case '2':           // 2M
          mData_bit_rate = 2 * 1000 * 1000;
          break;
        case '3':           // 3M
          mData_bit_rate = 3 * 1000 * 1000;
          break;
        case '4':           // 4M
          mData_bit_rate = 4 * 1000 * 1000;
          break;
        case '5':           // 5M
          mData_bit_rate = 5 * 1000 * 1000;
          break;
        default:
          break;
      }
      if (can_speedchange_func && !working) {
          if (can_speedchange_func(mOSC, mArbitration_bit_rate, mArb_SP, mData_bit_rate, mData_SP)) {
            slcan_ack();
          } else {
            mData_bit_rate = prev_data_bit_rate;
            slcan_nack();
          }
      }
      break;
    case 'F':               // STATUS FLAGS
      if (can_statusread_func && working) {
        can_status_t st;
        if (can_statusread_func(&st)) {

          // SJA1000互換ビットを1バイトにまとめる
          uint8_t value = 0;
          if (st.status_c1fifosta1 & (1U << 2))  value |= (1U << 0); // RX FIFO Full
          if (!(st.status_c1txqsta    & (1U << 0))) value |= (1U << 1); // TX FIFO Full
          if (st.status & ((1U << 18) | (1U << 17))) value |= (1U << 2); // Error Warning
          if (st.status_c1rxovif    & (1U << 0)) value |= (1U << 3); // Data Overrun
          // Bit 4 未使用
          if (st.status             & (1U << 20)) value |= (1U << 5); // Error Passive
          if (st.status_c1txqsta    & (1U << 6)) value |= (1U << 6); // Arbitration Lost
          if ((st.status & (1U << 21)) ||         // C1TREC: TXBO (Bus Off)
              (st.status_c1bdiag1 & ((1U << 16) | // NBIT0ERR
                                     (1U << 17) | // NBIT1ERR
                                     (1U << 18) | // NACKERR
                                     (1U << 19) | // NFORMERR
                                     (1U << 20) | // NSTUFERR
                                     (1U << 21) | // NCRCERR
                                     (1U << 23) | // TXBOERR
                                     (1U << 24) | // DBIT0ERR
                                     (1U << 25) | // DBIT1ERR
                                     (1U << 27) | // DFORMERR
                                     (1U << 28) | // DSTUFERR
                                     (1U << 29)   // DCRCERR
                                     )))
          {
            value |= (1U << 7); // Bus Error
          }
          
          // ASCII形式で出力: "F" + 2桁HEX + CR
          printf("F%02X\r", value);
          slcan_ack();
        } else {
          slcan_nack();
        }
      } else { 
        slcan_nack();
      }
      break;
    case 'V':               // VERSION NUMBER
      if (versionread_func) {
        char version[5];
        if (versionread_func(version, sizeof(version))) {
          // ASCII形式で出力: "V" + 4桁HEX + CR
          printf("V%04s\r", version);
          slcan_ack();
        } else {
          slcan_nack();
        }
      } else { 
        slcan_nack();
      }
      break;
    case 'N':               // SERIAL NUMBER
      printf("N%04X", mSerial);
      slcan_ack();
      break;
    case 'l':               // (NOT SPEC) TOGGLE LINE FEED ON SERIAL
      cr = !cr;
      slcan_nack();
      break;
    case 'h':               // (NOT SPEC) HELP SERIAL
      printf("\n");
      printf("https://github.com/TLDSJPWORK/pico-slcanfd - slcan fd raspberry pi pico\n");
      printf("\n");
      printf("O\t=\tStart slcan\n");
      printf("C\t=\tStop slcan\n");
      printf("t\t=\tSend std frame\n");
      printf("r\t=\tSend std rtr frame\n");
      printf("T\t=\tSend ext frame\n");
      printf("R\t=\tSend ext rtr frame\n");
      printf("d\t=\tSend normal fd frame\n");
      printf("b\t=\tSend normal fd with BRS frame\n");
      printf("D\t=\tSend ext fd frame\n");
      printf("B\t=\tSend ext fd with BRS frame\n");
      printf("Z0\t=\tTimestamp Off\n");
      printf("Z1\t=\tTimestamp On\n");
      printf("snn\t=\tSpeed 0xnnk N/A\n");
      printf("S0\t=\tSpeed 10k N/A\n");
      printf("S1\t=\tSpeed 20k N/A\n");
      printf("S2\t=\tSpeed 50k N/A\n");
      printf("S3\t=\tSpeed 100k\n");
      printf("S4\t=\tSpeed 125k\n");
      printf("S5\t=\tSpeed 250k\n");
      printf("S6\t=\tSpeed 500k\n");
      printf("S7\t=\tSpeed 800k\n");
      printf("S8\t=\tSpeed 1000k\n");
      printf("F\t=\tFlags N/A\n");
      printf("N\t=\tSerial No\n");
      printf("V\t=\tVersion\n");
      printf("-----NOT SPEC-----\n");
      printf("h\t=\tHelp\n");
      printf("l\t=\tToggle CR ");
      if (cr) {
        printf("ON\n");
      } else {
        printf("OFF\n");
      }
      printf("CAN_SPEED:\t");
      // TODO 復帰が必要
      /*switch(can_speed) {
        case 100:
          printf("100");
          break;
        case 125:
          printf("125");
          break;
        case 250:
          printf("250");
          break;
        case 500:
          printf("500");
          break;
        case 800:
          printf("800");
          break;
        case 1000:
          printf("1000");
          break;
        default:
          break;
      }*/
      printf("kbps\n");
      if (timestamp) {
        printf("\tT");
      }
      if (working) {
        printf("\tON");
      } else {
        printf("\tOFF");
      }
      printf("\n");
      slcan_nack();
      break;
    default:
      slcan_nack();
      break;
  }
} // pars_slcancmd()

//----------------------------------------------------------------

void TIME_CRITICAL transfer_tty2can() {
    static char cmdbuf[MAX_CMD_LEN];
    static int cmdidx = 0;
    char input[64];
    int count;

    // Read available USB data in chunks. Keep partial SLCAN commands between calls.
    while ((count = stdio_get_until(input, sizeof(input), make_timeout_time_us(0))) > 0) {
        for (int i = 0; i < count; ++i) {
            char val = input[i];
            cmdbuf[cmdidx++] = val;

            if (cmdidx == MAX_CMD_LEN) {
                slcan_nack();
                cmdidx = 0;
            } else if (val == '\r') {
                cmdbuf[cmdidx] = '\0';
                pars_slcancmd(cmdbuf);
                cmdidx = 0;
            }
        }
    }
} // transfer_tty2can()

//----------------------------------------------------------------

void TIME_CRITICAL transfer_can2tty()
{
  can_frame_t rx_frame;

  if (recv_canmsg_func) {
      if (recv_canmsg_func(&rx_frame)) {// 登録された関数を呼び出す

          int idx = 0;
          char command[1 + 8 + 1 + 128 + 2 + 8 + 30] = ""; // コマンド+ID+DLC+データ+FDフラグ+タイムスタンプ+CR + TODO 謎の30

          if(working) {
              // 実データ長を計算
              uint8_t len = (rx_frame.dlc < 16) ? dlc_to_len[rx_frame.dlc] : 0;
              const uint8_t *data_ptr = (const uint8_t *)rx_frame.fd_data;

              // FDフレーム判定
              bool is_fd = (rx_frame.flags & CAN_FRAME_FLAG_FDF) != 0;
              bool is_ext = can_id_is_extended(rx_frame.canid);
              uint32_t arbitration_id = rx_frame.canid.id & CAN_ID_ARBITRATION_ID;
              bool is_rtr = (rx_frame.flags & CAN_FRAME_FLAG_RTR) != 0;
              bool is_brs = (rx_frame.flags & CAN_FRAME_FLAG_BRS) != 0;
              bool is_esi = (rx_frame.flags & CAN_FRAME_FLAG_ESI) != 0;

              // コマンド文字
              if (is_fd) {
                  command[idx++] = (is_ext) ? (is_brs ? 'B' : 'D') : (is_brs ? 'b' : 'd');
              } else {
                  command[idx++] = (is_ext) ? (is_rtr ? 'R' : 'T') : (is_rtr ? 'r' : 't');
              }

              // ID追加
              if (is_ext) {
                  for (int shift = 28; shift >= 0; shift -= 4) {
                      command[idx++] = HEXVAL[(arbitration_id >> shift) & 0xF];
                  }
              } else {
                  for (int shift = 8; shift >= 0; shift -= 4) {
                      command[idx++] = HEXVAL[(arbitration_id >> shift) & 0xF];
                  }
              }

              // DLC追加（FD対応）
              command[idx++] = HEXVAL[rx_frame.dlc];

              // SavvyCANでは不要フィールド
              // TODO ESIを表せるフォーマットが今後必要
              // FDフラグ（BRS/ESI）
              /*if (is_fd) {
                  command[idx++] = is_brs ? '1' : '0';
                  command[idx++] = is_esi ? '1' : '0';
              }*/

              // データ追加（RTRならデータなし）
              if (!is_rtr) {
                  for (int i = 0; i < len; i++) {
                      command[idx++] = HEXVAL[(data_ptr[i] >> 4) & 0xF];
                      command[idx++] = HEXVAL[data_ptr[i] & 0xF];
                  }
              }

              // タイムスタンプ
              if(timestamp) {
                  long time_now = millis() % 60000;
                  command[idx++] = HEXVAL[(time_now >> 12) & 15];
                  command[idx++] = HEXVAL[(time_now >> 8) & 15];
                  command[idx++] = HEXVAL[(time_now >> 4) & 15];
                  command[idx++] = HEXVAL[time_now & 15];
              }

              // 終端
              command[idx++] = '\r';
              command[idx] = '\0';

              printf("%s", command);

              if (cr) printf("\r\n");
          }
      }
  }
} // transfer_can2tty()

//----------------------------------------------------------------

//----------------------------------------------------------------

void slcan_setup() {
  //呼び出し元での stdio_init_all(); が必要

  if (get_osc_func) {
      mOSC = get_osc_func();
  }

  if (can_speedchange_func && !working) {
      can_speedchange_func(mOSC, mArbitration_bit_rate, mArb_SP, mData_bit_rate, mData_SP);
  }

  mSerial = *(volatile uint32_t *)NVM_ADDR;

  if (oled_update_func) {
    oled_update_func();
  }
} // slcan_setup()
