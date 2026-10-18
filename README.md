# pico-slcan firmware

JP WORKS pico-slcan（RP2040 + MCP251xFD）用のUSB SLCAN / CAN FDファームウェアです。

## V101

USBからCANの送信能力を超えるバーストを入力したときに、送信できなかったフレームへ成功応答を返す問題を修正しました。1,000件の送信FIFOでバーストを保持し、送信完了に応じて順番に送り出します。

- FIFOへ受け付けたとき：CR（`0x0D`）。CANバス上の送信完了とは異なります。
- FIFO満杯またはCAN Close中：BEL（`0x07`）。FIFO内の既存データは上書きしません。
- 1,000件にはコントローラで送信中の先頭1件も含みます。1件76バイト、FIFO合計76,000バイトです。
- ACKがない間も受付済みのデータを保持します。ACKが復帰すると続けて送信します。
- `C`はCANを停止し、送信待ちを取り消します。`O`で再開できます。

入力がCANの送信能力を継続的に超える場合は、1,000件でも満杯になります。ホストはBELを扱い、送信ペースを制御してください。順序維持のためハードウェアへは1件ずつ渡します。最大スループットや遅延上限の測定結果ではありません。

V101ではFD受信時のデータコピー、拡張IDの受信表示、TX管理配列の初期化、RX FIFOサイズ設定、CAN割り込みと別コアの受信処理の排他も修正しています。

## ダウンロードと更新

[V101リリース](https://github.com/TLDSJPWORK/pico-slcan-soft/releases/tag/V101)から `pico-slcan-V101.uf2` をダウンロードしてください。[SHA-256](firmware/V101/SHA256SUMS)も確認できます。

1. ホスト側のCAN通信を停止します。
2. PicoのBOOTSELを押しながらUSB接続します。
3. `RPI-RP2` ドライブへUF2をコピーします。
4. 再起動後、USBシリアルから `V\r` でバージョンを確認します。下位2桁がソフトウェア版の `02` です。上位2桁は基板のGPIO設定に依存します。試験機では `V0102` でした。

配布UF2は本リポジトリの `PICO_BOARD=pico` 向けです。異なるSPI配線やCANコントローラの発振子を持つ基板には、そのまま適用しないでください。

## コマンド例

USBシリアルでは各コマンドをCRで終端します。

```text
C       CAN停止・待ちデータ取消
S6      公称500 kbit/s（停止中に設定）
Y2      FDデータ2 Mbit/s（停止中に設定）
O       CAN開始
V       バージョン
N       シリアル番号
w       TX FIFO状態
```

送信は `t/T`（標準/拡張CAN）、`r/R`（RTR）、`d/D`（FD・BRSなし）、`b/B`（FD・BRSあり）に対応します。例：`t1238AABBCCDDEEFF0011\r`。

`w\r` は `W accepted=... rejected=... completed=... queued=... capacity=1000 high_water=... retries=... inflight=...\r` を返します。`queued`は送信中を含む現在の待ち件数です。各カウンタは起動後の累積で、`C`による待ちデータの取消を成功件数から差し引きません。`retries`はコントローラへの投入を拒否された回数で、ACK待ちによるCANハードウェアの再送回数ではありません。

## ビルド

Pico SDK 2.2.0、Arm GNU Toolchain 14.2系、CMake、Ninjaでビルドを確認しています。

```sh
cmake -S . -B build -G Ninja \
  -DPICO_SDK_PATH=/path/to/pico-sdk \
  -DPICO_BOARD=pico
cmake --build build
```

SDKに必要なpicotoolがある場合、`build/pico-slcan.uf2`が生成されます。ELFから変換する場合：

```sh
picotool uf2 convert build/pico-slcan.elf pico-slcan.uf2
```

## 検証

[V101の実機検証記録](docs/validation-V101.md)と[機械可読の結果](tests/results/V101.json)を参照してください。受信52パターン、FD64バイトの各方向1,000件同時送受信、バースト、満杯時エラー、ACK復帰を公開するUF2で照合しています。

試験プログラムはPython標準ライブラリのみを使います。LinuxでPicoとSocketCAN対向を同じマシンへ接続するか、`SSH_TARGET`で対向Linuxを指定できます。試験条件と実行例は検証記録を参照してください。
