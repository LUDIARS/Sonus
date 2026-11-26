# D-03 入力のレートをボイスの元のレートにする（core への依頼）

依頼元: セッション D（ソース組 1、D-194）。宛先: B（core）。

## 背景

Input の種別は元のレートで読む種別（D-22、`RendersAtOutputRate() == false`）で、リサンプルとピッチは core が受け持つ。
core はボイスの元のレートを `VoiceStatic.sourceRate` に持つが、資産の無い Input では `CreateVoice`（`core/src/sys_event.cpp`）が
内部レート（`im.cfg.sampleRate`）を入れる。ゲームのマイクが 44.1 kHz や 16 kHz のとき、そのままでは音程と速さがずれる。

source 側は、ポートのレート（`InputPort::Configure(sampleRate, ...)`、0 = 内部レート）を開始の結果の予約の欄で返すように
してある（`SourceStartResult::reserved`、内部レートと同じなら 0。`source_if.hpp` の注記、D-194）。

## 依頼の中身

- `core/src/sys_event.cpp` の Start の結果を受けるループ（`kind->Start(b.req, b.count, b.res)` の後）で、
  `r.reserved != 0` かつその種別が `!RendersAtOutputRate()` なら `im.ev.vs[v].sourceRate = static_cast<float>(r.reserved)` にする。
  `StartPlayback` で UserBuffer に `sourceRate` を入れているのと同じ位置付け。リサンプラの状態は最初の描画で作られるので、
  開始の結果の時点で入れれば間に合う。
- 予約の欄は他の種別では 0 なので、既存の振る舞いは変わらない。契約の注記は source が `source_if.hpp` に足してある
  （内部の契約の「予約の欄を使う」追加）。

## source 側で用意済みのもの

- `source/src/input_kind.cpp` の `Start` が `res.reserved` を書く。単体テスト `SourceInput.PortRateIsReportedAtTheStart`。
- core の配線が入ったら、`source/test/source_synth_e2e_test.cpp` に「44.1 kHz で書いた 1 kHz が 1 kHz で鳴る」テストを足すとよい。
