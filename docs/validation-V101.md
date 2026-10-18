# V101 実機検証

2026-10-18、RP2040 pico-slcanと、Raspberry Pi上のMCP2518FD CANポートをCAN配線で接続して試験しました。Piの再起動は行っていません。公開UF2のSHA-256は[SHA256SUMS](../firmware/V101/SHA256SUMS)に記録しています。

## 送信FIFO

公称125 kbit/sのClassic CAN・8バイトで、入力がCAN送出速度を上回る条件を作りました。

| 条件 | 結果 |
|---|---|
| 1,000件をまとめて入力 | 受付1,000、受信1,000、順序・内容一致、満杯エラー0 |
| ACK停止中に1,100件を入力 | 1,000件保持、成功応答1,000、満杯エラー100 |
| 上記のACK復帰 | 保持した1,000件が順番どおり到着、待ち0 |
| 復帰後の低速送信 | 全件一致 |

旧ファームの比較試験では、同じ125 kbit/s・1,000件バーストで受信336、成功応答1,000でした。V101は受付できない場合に成功応答を返しません。

## 受信と同時送受信

公称500 kbit/s、FDデータ2 Mbit/s、Pi側の公称/データサンプル点80%、公称SJW=8、データSJW=2を使いました。

- 受信52パターン：標準/拡張ID、Classic CANとRTRのDLC 0/1/8、FDの長さ0/1/8/12/16/20/24/32/48/64、BRSなし/あり。USB出力のコマンド種別・ID・DLC・データを全件照合しました。
- 同時送受信：FD64バイト・BRSありをPico→PiとPi→Picoへ各1,000件。両方向とも欠落・重複・内容相違・順序違い0でした。
- 送信試験には、受付応答とCANバス上の受信を独立に比較しています。受付応答だけで成功とは判定していません。

## 範囲と制限

- 検証済みの組み合わせは上記条件です。全ビットレート・全配線・全負荷への保証ではありません。
- 125 kbit/s公称/2 Mbit/sデータの初期確認では、DLC 0・BRSありの送信が待ち状態になりました。原因は未特定です。BRS使用時の確認済み条件は500 kbit/s/2 Mbit/sです。
- 長時間ソーク、SEQ破損やTEFオーバーフローの故障注入、bus-offからの自動復帰は未評価です。障害時の待ちデータを明示的に取り消す場合は`C`を使います。
- ハードウェアへ1件ずつ渡すFIFO方式の最大スループットやレイテンシ上限は未測定です。
- UF2にはシリアル番号の個別書換えデータを含めていません。試験機では更新前後のシリアル番号が維持されました。

## 再実行

専用の試験バスで実行してください。`CAN_IF`以外のポートは操作しません。リンク停止を含む`noack`試験では、そのポートだけが一時的にDOWN/UPになります。非root実行では、当該ポートの`ip link`操作に対する`sudo -n`権限が必要です。

対向SocketCANは、試験ごとに設定を合わせて有効化しておきます。例（500k/2M）：

```sh
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 500000 sample-point 0.8 sjw 8 \
  dbitrate 2000000 dsample-point 0.8 dsjw 2 fd on
```

Picoと対向CANを同じLinuxマシンへ接続した場合：

```sh
SERIAL_PORT=/dev/ttyACM0 CAN_IF=can0 BUFFER_STATS=1 \
  python3 tests/hardware_burst.py output/burst
SERIAL_PORT=/dev/ttyACM0 CAN_IF=can0 \
  python3 tests/hardware_rx_duplex.py matrix output/rx.json
SERIAL_PORT=/dev/ttyACM0 CAN_IF=can0 \
  python3 tests/hardware_rx_duplex.py duplex output/duplex.json
```

PicoをMacへUSB接続し、CAN対向を別のLinuxマシンで動かす場合は、同じコマンドへ`SSH_TARGET=user@can-test-host`を追加し、`SERIAL_PORT`を実際の`/dev/cu.usbmodem...`に変更します。SSHは既存の鍵認証を使います。

満杯・復帰試験は公称125 kbit/sに設定した対向で行います：

```sh
SERIAL_PORT=/dev/ttyACM0 CAN_IF=can0 SLCAN_SPEED=S4 PEER_BITRATE=125000 \
  BUFFER_STATS=1 DRAIN_SECONDS=3 \
  PHASES='[["baseline",20,0.01],["burst1000",1000,0],["noack",1100,0],["recovery",20,0.01]]' \
  python3 tests/hardware_burst.py output/full
```

試験スクリプトの送信時間はUSBホストへの書込み時間です。バス上の送信完了時間とは区別してください。結果に含まれる`queue_after.queued=0`と受信データ照合で、待ちデータの送出完了を確認します。
