# D-01 合成の変調を Parameter から届ける（core / bank 形式 / buildtool への依頼）

依頼元: セッション D（ソース組 1、D-190〜D-192）。宛先: B（core）、C（buildtool）、統合担当（バンク形式の列と列挙の追加）。

## 背景

Oscillator の「周波数とデューティを Parameter で動かせる」（設計書 5 章、D-extensions.md 2.1）のうち、

- **周波数**は、既存の `CurveTarget::Pitch` → `Voice_pitch` → `RenderItem::pitch` の経路で既に動く（Oscillator の種別がピッチのランプをサンプルごとに反映する。D-190）。追加の配線は要らない。
- **デューティ**と**ウェーブテーブルの位置**は、core から source へ値を渡す欄が無かった。source 側は内部の `RenderItem` の末尾に次の 2 欄を足して、読むところまで作ってある（D-191）。

```cpp
// contract/internal/include/snd/internal/source_if.hpp, struct RenderItem の末尾
Ramp dutyOffset;           // OscillatorParams::duty に足す（Pulse）
Ramp tablePositionOffset;  // OscillatorParams::tablePosition に足す（ウェーブテーブル、0..1）
```

core は `sys_renderlist.cpp` で `it = source::RenderItem{}` と零にしてから写すので、今は 0（変調なし）が届く。

## 依頼の中身

1. **バンク形式（統合担当 / C）**: `bank_format.hpp` の `enum class CurveTarget` の末尾に
   `SynthDuty = 10`、`SynthTablePosition = 11` を足す（`CurveCompose` の既定は加算 = 新しい値 `AddLinear` か、既存の
   `Multiply` 以外で y をそのまま足す扱い。既存の `AddCents` / `AddDb` の流用でもよいが、単位が無いので加算の合成を 1 つ
   足すのが分かりやすい）。
2. **buildtool（C）**: `build.cpp` の `kCurveTarget` の末尾に `"duty"`、`"tablePosition"` を足し、`DefaultCompose` を加算にする。
   合成の SYNP に `tablePosition` の列（F32、既定 0）を足し、`"synth": {"tablePosition": 0.3}` を書けるようにする
   （今は core が `OscillatorParams::tablePosition = 0` 固定、`core/src/bank_registry.cpp` の SYNP の写し）。
3. **core（B）**:
   - `core/src/sys_dsp.cpp`（EvaluateDspParams）のパラメータ曲線の `switch` に 2 つのターゲットを足し、ボイスごとに
     `duty`、`tablePosition` の合計（加算）を求める。
   - ボイスごとにブロック先頭・末尾の値をランプで持つ（`Voice_pitch` と同じ形の列を `schema.hpp` に足すか、
     `VoiceStatic` の中に前のブロックの値を持つ）。最初のブロックは start = end。
   - `core/src/sys_render.cpp` の直列区間（b、`it.pitch = ...` の隣）で `it.dutyOffset` / `it.tablePositionOffset` に写す
     （遅れたチャンクの約束 D-43 のとおり、チャンクは列を読まない）。
   - 写す値は core 側でも有限に丸める（NaN・±∞ を渡さない）。source も受け取ったブロックごとに検査して 0 として扱うが（D-198）、
     曲線の評価の段で有限にしておくこと。
   - `bank_registry.cpp` で SYNP の `tablePosition` 列を `OscillatorParams::tablePosition` に写す。
4. **（任意、B）出力レートの合成のピッチの上限**: `sys_render.cpp` は合成のボイスのレート比も `Config.maxPitchRatio`（既定 4）と
   `dsp::kResamplerMinRatio`（1/256）で丸める。リサンプラを通らない種別（Oscillator / Noise / Callback）では上限は要らないので、
   出力レートの種別には丸めないか、別の上限（例えば 64）にしてほしい。今のままだと Parameter で周波数を上げられるのは
   基準の 2 オクターブ上まで。

## source 側で用意済みのもの

- Oscillator はパルスのデューティを `duty + dutyOffset`（サンプルごとに線形、[0.001, 0.999] に丸め）、ウェーブテーブルの位置を
  `tablePosition + tablePositionOffset`（[0, 1] に丸め）で使う（`source/src/oscillator_kind.cpp`）。
- 単体テスト `source/test/source_osc_test.cpp` の `DutyOffsetMovesThePulseWidth`、`WavetableReadsAndMixesTables` が、
  欄に値を入れたときの解析的な期待値（基本波の振幅 4/π·sin(πd)、テーブルの混ぜ具合）を確かめている。core の配線が入ったら、
  `source/test/source_synth_e2e_test.cpp` に Parameter → デューティの端から端までのテストを足すとよい。

## 追加（任意）: Noise のシード

- core は `NoiseParams::seed` を SYNP の行番号から作る（`bank_registry.cpp` の `s * 2654435761u + 1u`）。バンクに
  シードの列が無いので、デザイナーが「毎回同じ雑音」を選べない。SYNP に `seed`（U32、0 = 行番号から）を足して写してほしい（C / B）。
- `SourceStartRequest::seed` は `Playback の乱数状態 ^ (ボイスのスロット × 0x9E3779B9)`（`sys_event.cpp` の `CreateVoice`）で、
  ボイスのスロットに依る。同じコマンドの記録なら割り当ても同じなので決定論は保たれるが、「同じ Playback のシードなら同じ雑音」
  にはならない。Noise は両方を混ぜて使う（D-193）。スロットに依らない値（例えば Playback の中でのボイスの通し番号）にするかは B の判断。
