# 依頼 D-11: コンボリューションの IR をバンクからバスのエフェクトへ（core = B、buildtool = C、bank-format.md = 統合担当）

依頼元: セッション D（dsp 組）。決定: CONTRACTS.md「段階 7-D」の D-211〜D-215、差し戻しの D-218 / D-219（登録の番号、寿命、切り替え）。
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
5. 登録: `const std::uint32_t num = dsp::ConvolutionRegisterIr(ir);`（世代つきの番号 1..2^24−1、D-219。0 なら登録表が満杯 → IR なしで続ける）。
   準備は NaN / ±Inf のサンプルを含む IR を拒否する（nullptr）。そのときも IR なしで続ける（ログ）。
6. パラメータ: `EffectInit` に渡す記述の初期値と、`EffectInstance::params` の両方で
   `params[dsp::kConvIrSlot] = static_cast<float>(num);`（24 ビットなので float で正確）。
   さらに推奨: `params[dsp::kConvMaxIrMs]` を IR の長さに合わせる（`1000 × ConvolutionIrFrames(ir) / sampleRate` を切り上げ、
   10〜10000 に収める）と、DSP プールの状態が IR にぴったりになる。その場合は `EffectInstanceBytes` の前に入れること
   （状態の大きさは `MaxIrMs` で決まる、D-214）。
7. 解放（バンクを外すとき、D-219 の寿命の約束）: `dsp::ConvolutionUnregisterIr(num)`（登録が返した番号そのもの）→
   **その IR を使いうる全てのシステムの描画が 1 回以上終わるのを待つ**（core のエポック。インスタンスは呼び出しの先頭で表を引き、
   外されたのを見たら古い IR を読まずにフェードアウトする、D-218）→ メモリの `Free`。順序を逆にしない。
   番号をそのシステムの外へ渡さない（同じプロセスの別の snd_system からも番号を知れば引けてしまう）。
- IR を差し替える（ライブリンクで IR のアセットを入れ替える等）ときは、新しい IR を準備・登録してから `IrSlot` を新しい番号に
  書き換え、古い番号を 7 の手順で外す。インスタンスは 1 分割（B フレーム）のクロスフェードで切り替える（D-218）。
- **`IrSlot` をライブリンクとスナップショットで読み取り専用にする**（D-219）。番号は世代つきで古い番号は無効になるが、
  有効な番号を書けば他のバンク（他のシステム）の IR を指せる。メモリ安全ではある（IR の妥当性は毎回確かめる）が、意図しない音になる。
  ライブリンクのパラメータの一覧では読み取り専用（または表示しない）、スナップショットの対象から外す。
- 状態の大きさ: ステレオ・48 kHz・2 秒で約 1.56 MB（1563200 バイト、D-214。D-218 で先頭のタップの写しが増えた）。`Config` の DSP プールの予備（`dspPoolReserve` 等）の既定値が
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

## 6. システムごとの登録表にする案（D-219、今回は入れていない）

今の登録表はプロセスに 1 つ。システムごとにするには、`EffectProcess` まで表を届ける経路が要る（既存の dsp と core の変更）。案:

```cpp
// dsp（新しい型、呼び出し側のメモリ）
struct ConvolutionIrTable;                                    // 枠 + 世代、ロックなし
std::size_t ConvolutionIrTableBytes(std::uint32_t entries) noexcept;
ConvolutionIrTable* ConvolutionIrTableInit(void* mem, std::size_t bytes, std::uint32_t entries) noexcept;
std::uint32_t ConvolutionIrTableRegister(ConvolutionIrTable*, const ConvolutionIr*) noexcept;
bool ConvolutionIrTableUnregister(ConvolutionIrTable*, std::uint32_t number) noexcept;
```

- 表をインスタンスへ届ける経路は 2 案:
  1. `snd_render_context`（`ParamBlock::ctx`）に表のポインタを足す。内部の契約の構造体の末尾への追加になる（版の判断が要る）。
     `ProcessEffectChain` の標準エフェクトの経路は今 `ctx` を `EffectProcess` に渡していないので、`EffectProcess` の引数か
     `FxIo` に `ctx` を通す変更も要る（`effects.cpp` の既存の行）。
  2. `EffectInit` のあとに core が `ConvolutionBindTable(state, table)` でインスタンスに表を結び付ける。状態にポインタを持つことになるので、
     `EffectReset` / NaN からの回復（`ReinitKeepingParams` が状態全体を作り直す）で消えないよう、依頼 D-12 の
     「種別の Reset フックを使う」変更が先に要る。
- どちらでも、システムを壊すときに表ごと捨てれば、外し忘れの IR が他のシステムから見えることは無くなる。
