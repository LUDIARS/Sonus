# 依頼 D-11: コンボリューションの IR をバンクからバスのエフェクトへ（core = B、buildtool = C、bank-format.md = 統合担当）

依頼元: セッション D（dsp 組）。決定: CONTRACTS.md「段階 7-D」の D-211〜D-215。
dsp 側（`snd/dsp/convolution.hpp`、標準エフェクト `snd.fx.convolution`、準備済み IR の登録表、ブロブ形式 SCIR）とその単体テストは
D のブランチで完成している（テストの中で IR を準備・登録し、エフェクトとして処理できる）。足りないのは、バンクの IR を
ランタイムで準備して登録し、インスタンスの `IrSlot` に入れる配線と、ビルドツールの IR の取り込み。

## 1. 既存のテストが 1 件落ちる（統合担当、文書の 1 行）

`tests/integration/build_pipeline_test.cpp` の `BuildPipeline.BankFormatEffectTableMatchesDsp` は、dsp の全種別の行が
`spec/interface/bank-format.md` 6.4 の表にあることを確かめる。D は bank-format.md を書けないので、次の 1 行を表の最後
（`snd.fx.surround` の行の次）に足してほしい。あわせて本文の「次の 14 種」を「次の 15 種」に。

```
| `snd.fx.convolution` | `0x8837ECB0` | `kEffectConvolution` | 7 | 一様分割のコンボリューションリバーブ（IR は EFFX のブロブ、形式 SCIR、D-213） |
```

テストは書き換えない。この行が入るまで、このテストは D のブランチで失敗する（他は通る）。
別の解決策（採らなかったもの）: コンボリューションを登録表に載せず別の入口（`ConvolutionStateBytes` などの専用関数）にすれば
このテストは通るが、バスのエフェクト・DSPN・ライブリンク・ツールがそのまま扱えなくなり、core に種別ごとの分岐が要る。

## 2. バンクのブロブ形式（bank-format.md に追記、形式 2.0 のまま）

コンボリューションの `EFFX` 行は `blobOffset` / `blobBytes` で `EFBL` の中の IR を指す（列は既にある。core と bank は既に
`snd_effect_desc.blob` / `blob_bytes` に入れて渡している）。中身は SCIR（D-213）:

| オフセット | 型 | 内容 |
| --- | --- | --- |
| 0 | u32 | magic `'SCIR'` = 0x52494353 |
| 4 | u32 | 版 1 |
| 8 | u32 | チャンネル数 1〜16（1 = モノ、C = チャンネルごと、C×C = 行列 / true stereo（LL、LR、RL、RR）） |
| 12 | u32 | フレーム数（≥ 1） |
| 16 | u32 | サンプルレート 1〜384000 |
| 20 | f32 | gain（有限） |
| 24 | u32 × 2 | 予約 0 |
| 32 | f32 × ch × frames | サンプル、チャンネルごとに続けて（非インターリーブ） |

リトルエンディアン、境界不問、`blobBytes` はちょうど 32 + 4 × ch × frames。検証は `dsp::ConvolutionParseIrBlob`
（bank は dsp に依存できないので、bank の検証では大きさの整合だけ見て、中身は core が読み込みのときに確かめる、でよい）。

## 3. core（セッション B）: 読み込みのときに IR を準備して登録する

`core/src/bank_registry.cpp`（エフェクトの記述を作って `EffectInstanceBytes` / `EffectInit` している所）で、
`d.effect_type == dsp::kEffectConvolution` かつ `d.blob != nullptr` の行について:

1. 分割の大きさ: `const std::uint32_t B = dsp::ConvolutionPartitionFrames(d);`（`d.max_frames` = ブロック長と
   パラメータ 4 から決まる。インスタンスと同じ値）。
2. 大きさ: `const std::size_t irBytes = dsp::ConvolutionIrBytesFromBlob(d.blob, d.blob_bytes, im.cfg.sampleRate, B);`
   0 ならブロブが壊れている → そのエフェクトは IR なし（ウェット無音）で続ける（ログ）。読み込みを失敗にはしない。
3. メモリ: `im.alloc->Allocate(irBytes, 64, MemTag::DspPool)`。DSP プールの予約（D-54 の `ComputeDspPoolNeed`）とは別に、
   バンクのレコードに IR のメモリとして持つ（`dspMemory` と同じ寿命）。同じブロブ（同じオフセット）を指す行が複数あれば 1 回だけ準備して共有してよい。
4. 準備: `const dsp::ConvolutionIr* ir = dsp::ConvolutionPrepareIrFromBlob(mem, irBytes, d.blob, d.blob_bytes, im.cfg.sampleRate, B);`
   （通常ワーカーで。2 秒・ステレオ・44.1 kHz → 48 kHz で数百 ms かかることがある。レートが同じなら FFT だけで速い）。
5. 登録: `const std::uint32_t slot = dsp::ConvolutionRegisterIr(ir);`（0 なら登録表が満杯 → IR なしで続ける）。
6. パラメータ: `EffectInit` に渡す記述の初期値と、`EffectInstance::params` の両方で
   `params[dsp::kConvIrSlot] = static_cast<float>(slot);`（整数のまま float で正確）。
   さらに推奨: `params[dsp::kConvMaxIrMs]` を IR の長さに合わせる（`1000 × ConvolutionIrFrames(ir) / sampleRate` を切り上げ、
   10〜10000 に収める）と、DSP プールの状態が IR にぴったりになる。その場合は `EffectInstanceBytes` の前に入れること
   （状態の大きさは `MaxIrMs` で決まる、D-214）。
7. 解放（バンクを外すとき）: エポックで「どのスレッドもこのバンクのエフェクトを処理していない」ことを確かめてから
   `dsp::ConvolutionUnregisterIr(slot)` → メモリの `Free`。順序を逆にしない（登録表から外す前に解放するとぶら下がる）。
- スナップショットやライブリンクで `IrSlot` を書き換えられないようにする（0〜1024 の任意の値を入れると、他のバンクの IR を
  指せてしまう。メモリ安全ではある（IR の妥当性は毎回確かめる）が、意図しない音になる）。ライブリンクのパラメータの一覧では
  読み取り専用として扱う。
- 状態の大きさ: ステレオ・48 kHz・2 秒で約 1.55 MB（D-214）。`Config` の DSP プールの予備（`dspPoolReserve` 等）の既定値が
  小さい場合は見直しが要る。

## 4. buildtool（セッション C）: IR を SCIR にしてバンクに入れる

- プロジェクトのバスのエフェクトに `{"type": "snd.fx.convolution", "ir": "audio/hall.wav", "params": [...]}` を書けるようにする。
  `ir` は音声アセット（WAV など、既存のアセットの読み込みを使う）。チャンネル数はそのまま（1、バスと同じ、バスの 2 乗）。
  レート変換はしない（ランタイムが準備のときに変換する。SCIR にファイルのレートを書く）。gain は `"irGain"`（dB、既定 0）から。
- `dsp::ConvolutionWriteIrBlob` がそのまま使える（buildtool は dsp に依存してよい、依存表）。既存の `"blob"` の指定（生のバイト列）
  でも SCIR のファイルを渡せば動く。
- `MaxIrMs`（パラメータ 6）が指定されていなければ、IR の長さ（ms、切り上げ）を入れる。指定が IR より短ければ警告
  （「IR の後ろが切れる」）。10000 ms を超える IR はエラー。
- `IrSlot`（パラメータ 0）はランタイムが入れるので、プロジェクトに書かれていたら警告して 0 にする。
- DSPN の計算（`DefaultEffectStateBytes`）は今の `EffectInstanceBytes` のまま正しい（状態は `MaxIrMs` で決まる）。
  IR の準備済みのメモリは DSPN に入らない（ランタイムが別に確保する）。必要なら DSPN の注記に IR の大きさの合計を足す。

## 5. ツール（任意）

- エフェクトの UI で `IrSlot` を隠し、IR のアセット名を出す。`ZeroLatency` = 0 のときは遅延（B フレーム）を表示する。
