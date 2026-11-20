# 依頼 D-10: タイムストレッチをボイスにつなぐ（core = セッション B、api、bank / buildtool = C）

依頼元: セッション D（dsp 組）。決定: CONTRACTS.md「段階 7-D」の D-209、D-210。
部品（`dsp/include/snd/dsp/time_stretch.hpp`、`dsp/src/time_stretch.cpp`）とその単体テストは D のブランチで完成している。
この依頼は、Sample と Stream（と UserBuffer / Input の source-rate 種別）のボイスで「ピッチを変えずに再生速度を変える」ための配線。

## 1. 置く場所: ソース → タイムストレッチ → リサンプラ

タイムストレッチは**ソースのレート**で動き、リサンプラ（D-22、D-44）の前に入る。ピッチはこれまでどおりリサンプラの比で、速度は
ソースの消費量だけを変える（ソース 1 フレームあたりの出力は 比 × 速度）。

`core/src/sys_render.cpp` の `RenderRun`（`kItemSourceRate` の分岐）を次の順にする。速度を使うボイス（下の 3）だけ。

```cpp
// 1. リサンプラの入力 = タイムストレッチの出力
const std::uint32_t nR   = dsp::ResamplerInputFrames(rs, outFrames, in.ratio0, in.ratio1);
const std::uint32_t padR = dsp::ResamplerPadFrames(dsp::ResamplerGetQuality(rs), im.cfg.maxPitchRatio);
// 2. タイムストレッチの入力 = ソースが描く量
std::uint8_t* ts = TimeStretchState(im, slot);          // 3 の枠
const std::uint32_t nT   = dsp::TimeStretchInputFrames(ts, nR, in.speed0, in.speed1);
const std::uint32_t padT = dsp::TimeStretchPadFrames(ts);
// スレッド別アリーナから、チャンネルごとに
//   tsIn[c]: padT + nT floats（ソースの書き先は tsIn[c] + padT）
//   rsIn[c]: padR + nR floats（タイムストレッチの書き先は rsIn[c] + padR）
it.out[c] = tsIn[c] + padT;  it.framesRequested = nT;  // ソースへ
// --- source->RenderBatch ---
// 3. 後段
dsp::TimeStretchProcess(ts, tsIn, nT, rsInAt /* rsIn[c] + padR */, nR, in.speed0, in.speed1);
dsp::ResamplerProcess(rs, rsIn, nR, outAt, frames - in.frameOffset, in.ratio0, in.ratio1);
```

- `TimeStretchProcess` は `in[c]` の先頭 `padT` フレームを履歴で上書きする（リサンプラのパッドと同じ約束）。
  `out` と `in` は重ねない。`TimeStretchProcess` の戻り値は `nT`（足りなければ 0 で無音、状態は変わらない）。
- `in.speed0` / `in.speed1` はブロックの始めと終わりの速度（`ItemInput` に 2 欄足す。ratio0 / ratio1 と同じ作り方）。
  0.25〜maxSpeed に切り詰められる。ランプはブロック内で一次（D-210）。
- 速度 1 のボイスは今までどおり（タイムストレッチを通さない）。**速度が初めて 1 以外になったブロックで**
  `TimeStretchInit` してから通し始めればよい: 初期化直後の速度 1 は入力とビット一致で遅れもない（D-210）ので、
  途中から差し込んでも継ぎ目は出ない（ソースの読み位置からそのまま流し込む）。
  一度通し始めたら、ボイスが終わるまで通し続ける（1 に戻しても外さない。外すと最大 ±8 ms の跳びが出る）。
- シーク（`Seek`）、仮想化からの復帰（D-15 のフェード）、ループのないソースの途中からの再開では `TimeStretchReset` してから
  新しい位置のフレームを流す。Sample のループはソースが連続したフレームとして渡すので、タイムストレッチは何もしなくてよい。

## 2. 状態のプール（D-43 / D-44 と同じ考え方）

- 状態の大きさは `dsp::TimeStretchStateBytes(channels, sourceRate, maxSpeed)`（64 の倍数）。
  ステレオ・48 kHz・最大速度 4 で 47 KB、8ch・96 kHz・4 では約 300 KB になるので、**全ての実ボイスに持たせない**。
- 推奨: 速度を使うボイスだけが借りる固定個数のプール。
  - 枠の大きさ = `TimeStretchStateBytes(Config.maxStretchChannels, Config.maxStretchRate, Config.maxPlaybackSpeed)`、
    個数 = `Config.maxStretchVoices`、`MemTag::Tables`、64 バイト境界。初期化のときに確保する（以後は確保しない）。
  - 借りる / 返すのはオーディオスレッドの直列区間（D-43: 枠の番号を RealVoice の列か小さな表に持ち、チャンクは番号で引くだけ）。
  - 空きが無いとき、ソースのレートが `maxStretchRate` を超えるとき、チャンネル数が `maxStretchChannels` を超えるときは
    **速度 1 で鳴らす**（拒否しない）。統計（`snd_stats` の予約欄か新しい欄）に数える。
  - `TimeStretchInit(state, slotBytes, channels, sourceRate, Config.maxPlaybackSpeed)`。実際のソースのレートとチャンネル数で初期化すれば、
    枠より小さい状態で済む。
- スレッド別アリーナの見積もりに 1 ボイスあたり
  `channels × (padT + TimeStretchMaxInputFrames(ts, nRmax))` floats を足す。
  `nRmax` はブロック長と `maxPitchRatio` から決まるリサンプラの入力の最大（例: 256 フレーム、比 4、速度 2 で `nT` は約 2900、
  `padT` は 48 kHz・速度 4 で 4384）。

## 3. Config の候補（`snd_config` の末尾、`struct_size` で守る。版は統合で 1.3）

| 名前 | 型 | 既定 | 範囲 | 意味 |
| --- | --- | --- | --- | --- |
| `max_stretch_voices` | u32 | 16 | 0〜`max_real_voices` | 速度を変えられる同時ボイス数（0 で機能なし） |
| `max_stretch_channels` | u32 | 2 | 1〜8 | 速度を変えられるボイスのチャンネル数の上限 |
| `max_stretch_rate` | u32 | 48000 | 8000〜384000 | 速度を変えられるソースのレートの上限 |
| `max_playback_speed` | f32 | 2.0 | 1〜4（`dsp::kTimeStretchMaxSpeed`） | 速度の上限（下限は 0.25 固定） |

## 4. 公開 API の案（api、`c_api.cpp` の末尾）

```c
/* Playback speed without changing the pitch (time stretch, D-209). speed > 0, 1 = normal;
 * clamped to [0.25, Config.max_playback_speed]. Applies to the source-rate voices of the
 * playback (Sample, Stream, user buffers); synth and callback voices ignore it. Starts at 1,
 * applies from the block that applies the command, ramped over that block. Voices that cannot
 * get a stretch slot play at speed 1 (counted in snd_stats). Thread: registered. Must-deliver
 * (a playback-scope parameter with a reserved id, like snd_set_playback_pitch, D-104).
 * Errors: SND_ERR_INVALID_HANDLE, SND_ERR_INVALID_ARGUMENT (NaN / infinite / <= 0),
 * SND_ERR_THREAD_NOT_REGISTERED, SND_ERR_COMMAND_BUFFER_FULL. */
SND_API snd_result SND_CALL snd_set_playback_speed(snd_system* sys, snd_playback_handle playback,
                                                   float speed);
```

- 実装は D-104 の音量・ピッチと同じ「予約 id の Playback スコープのパラメータ」。予約 id を 1 つ足す必要がある:
  `0xFFFFFFFB`（`SND_ID_RESERVED_FIRST` を `0xFFFFFFFB` に下げる。buildtool の名前の衝突検査と `snd_set_param_*` の拒否が
  範囲で書かれていれば自動で効く）。これは契約の値の変更なので統合担当の判断が要る。代わりに既存のコマンドの `variant` を使う案もある。
- C++ 版は `Playback::SetSpeed(float)`。

## 5. バンク（buildtool / bank = C、形式 2.0 のまま足せるもの）

- イベントのパラメータカーブで速度を動かすなら、上の予約 id をカーブの出力先に使えるようにする（新しい列は不要）。
- Clip ごとの既定の速度が欲しいなら、`CLIP` に `speed`（F32、既定 1）の列を足す（新しい列の追加は許されている。bank-format.md の追記）。

## 6. 再生位置、終わり、通知

- ソースの読み位置は「タイムストレッチが消費したフレーム」まで先に進んでいる（先読み S + 2H = 48 kHz で 1824 フレーム = 38 ms）。
  ゲームに見せる再生位置（とマーカーの通知）は、次の出力フレームのソース上の位置
  `TimeStretchConsumedFrames(ts) + TimeStretchPendingOffset(ts)`（タイムストレッチの出力の座標で、さらにリサンプラの
  `ResamplerPendingOffset` × 速度 だけ手前）を、ボイスの開始位置に足したものにする。
- ソースが終端に達しても、タイムストレッチの中に最大で先読みぶんのフレームが残っている。ボイスを止めるのは、
  上の位置がソースの終端を過ぎてから（それまではソースは終端の後ろを 0 で埋める。今のリサンプラの扱いと同じ）。
- 速度を変えても拍の通知（music）は変えない（music のトランスポートはタイムストレッチの対象外）。

## 7. テストの案（core / api）

- 速度 1 のままのボイスの出力が今とビット一致（タイムストレッチを通さない）。
- `snd_set_playback_speed(0.5)` の Sample ボイスで、鳴っている時間が 2 倍、主な周波数が変わらない（DFT）。
- 途中で 1 → 1.5 → 1 と変えたときに継ぎ目に大きな段差が出ない、ストリームのボイスで IO の先読みが足りる。
- 枠が足りないときに速度 1 で鳴り、統計が増える。初期化の後の確保がゼロ（`SND_RT_SCOPE`）。
