# D-05 入力ポートの設定を snd_config から渡す（api への依頼）

依頼元: セッション D（ソース組 1、D-194、D-195）。宛先: api（構成ルート）、統合担当。

## 背景

入力ポートの数・大きさ・チャンネル数は `source::RuntimeDesc`（`inputPorts` = 4、`inputPortFrames` = 8192、
`inputPortChannels` = 2）で決まる。api の `SourceKinds::Init`（`api/compose/compose.cpp`）は `RuntimeDesc` を既定のまま作るので、
ゲームはこれを変えられない。D の持ち場では `compose.cpp` を変えられず、`snd_config` に欄を足すと D-91 の `struct_size` の
受け付け（`config_check.cpp`）も変える必要があるので、ここでは足していない。

## 依頼の中身

1. `snd_config` の末尾（`struct_size` で守られる位置）に `input_ports`（既定 4、0 = Input を使わない）、`input_port_frames`
   （既定 8192）、`input_port_channels`（既定 2、1〜8）を足し、`config.h` の X マクロと C++ の `Config` にも足す。
   D-91 の受け付ける大きさに、今の大きさ（足す前）を加える。
2. `compose.cpp` の `SourceKinds::Init` に引数を足して `RuntimeDesc` に写す（`input_ports == 0` なら `enableInput = false`）。
3. 公開 API の追加（任意）:
   - `snd_set_input_format(sys, device_index, sample_rate, max_latency_frames)` → `InputPort::Configure`。
     レートを内部と違う値にするのは依頼 D-03（core）が入ってから。
   - `snd_stats` の予約の欄か末尾に、入力のアンダーラン・オーバーランの数（`source::RuntimeStats::inputUnderruns`、
     `InputPortStats::droppedFrames`）。
4. livelink の規則表には載せない（`snd_write_input` はコマンドを記録しない。`snd_write_user_buffer` と同じ）。
