# codec モジュール

設計書 6.4（コーデック）、3.3（デコーダプール）、12.1、契約 D-14・D-23・D-31。依存は `base` だけ（ルート `CMakeLists.txt`）。
段階1 では base がスタブなので、標準ライブラリと契約ヘッダだけで書いている（末尾「codec 内に持った汎用部品」）。

## 公開ヘッダ

| ヘッダ | 中身 | 使う側 | スレッド |
| --- | --- | --- | --- |
| `snd/codec/codec.hpp` | 組み込みデコーダ（`BuiltinCodec`）、ヘッダ検証、パケットの大きさ、シーク表の引き方 | source、bank、buildtool | 純関数は任意。`Init/Decode/Seek` は状態ごとに 1 スレッド（通常ワーカー） |
| `snd/codec/registry.hpp` | `CodecRegistry`: fourcc → `ICodec*`（組み込み＋`Config.plugins.codecs`） | 構成ルート（api）、source | `Init` は所有スレッド、`Find` は任意（RT 可） |
| `snd/codec/decoder_pool.hpp` | `DecoderPool`: 固定スロットの取得・返却・使用量 | 構成ルート、source | `Acquire/Release` はロックフリーで任意（RT 可） |
| `snd/codec/encode.hpp` | エンコーダ、`snd_codec_header` の生成、シーク表の生成 | buildtool だけ | 任意（純関数） |

ランタイムにエンコーダは入らない: `sonus_codec` は静的ライブラリで、エンコーダは `src/encode.cpp` だけにある。
`encode.hpp` の関数を呼ばない実行ファイルにはリンクされない（CTest `codec_runtime_has_no_encoder` が `nm` で確認する）。

### 主な API

```cpp
// デコード（source の先読みジョブ）
snd::ICodec* c = registry.Find(asset.codec.codec, &why);    // nullptr なら why = NotRegistered / OpusUnavailable
std::uint32_t bytes = c->StateBytes(asset.codec);           // バンク準備時に計算して覚えておくのを推奨
snd::codec::DecoderSlot slot;
if (pool.Acquire(bytes, &slot) != snd::codec::DecoderPoolStatus::Ok)
  /* 再生拒否: snd::codec::EndDetailFor(status) == SND_END_DETAIL_DECODER_POOL_FULL */;
c->Init(slot.state, asset.codec);
n = c->Decode(slot.state, chunk, chunkBytes, ringPtrs, ringFree);  // 入力は全部消費
n = c->Decode(slot.state, nullptr, 0, ringPtrs, ringFree);         // 保持分の吐き出し
pool.Release(slot);   // 添字と世代の組。古い所有者からの返却は拒否される

// シーク（D-23）
uint32_t e = snd::codec::FindSeekPoint(asset.seekFrames, asset.seekCount, target, asset.codec.pre_roll,
                                       asset.codec.total_frames);  // target >= total_frames なら kNoSeekPoint
c->Seek(slot.state, asset.seekFrames[e]);
// asset.seekOffsets[e] のバイトから渡し、先頭 target - seekFrames[e] フレームを捨てる
// シーク表の無い PCM / ADPCM は snd::codec::PacketForFrame(header, target - pre_roll, &pos) でも求まる
// （target >= total_frames、またはバイト位置が 64 bit に収まらないときは false）

// ビルド（buildtool）
snd::CodecHeader h = snd::codec::encode::MakeAdpcmHeader(ch, rate, frames);   // または MakePcmHeader
std::vector<uint8_t> data(snd::codec::encode::EncodedBytes(h));
snd::codec::encode::Encode(h, planar, frames, data.data(), data.size(), &written);
uint32_t n = snd::codec::encode::BuildSeekTable(h, intervalFrames, nullptr, nullptr, 0);  // 個数
snd::codec::encode::BuildSeekTable(h, intervalFrames, seekFrames, seekOffsets, n);        // SEEK 行
```

ASST の列との対応: `codec, channels, sampleRate, totalFrames, preSkip, preRoll, framesPerPacket, blockAlign` は
`snd_codec_header` の同名フィールド。組み込みコーデックは `extra` を使わない（`extraBytes = 0`）。

## データ形式（組み込みコーデック）

`bank-format.md` にはコーデックのバイト列の定義が無いので、ここで決めた（`bank-format.md` への追記を契約変更として提案する）。

| fourcc | 1 パケット | 内容 |
| --- | --- | --- |
| `PCMF` | 1 フレーム（`4 × ch` バイト） | float32 リトルエンディアン、インターリーブ。有限値はそのまま（±1 を超えても切らない）。**NaN / ±Inf は 0 にする**（壊れたバンクの NaN がリバーブなどのフィードバック系を汚さないため） |
| `PC16` | 1 フレーム（`2 × ch` バイト） | int16 リトルエンディアン、インターリーブ。`x / 32768` で展開 |
| `ADPC` | 1 ブロック（`block_align` バイト） | WAV の IMA ADPCM と同じ配置。チャンネルごとに 4 バイトのヘッダ `{s16 予測値, u8 ステップ添字 0..88, u8 0}`、続いて 4 バイト × ch の組を繰り返す（各組がそのチャンネルの 8 サンプル、下位ニブルが先）。1 ブロックのフレーム数は `1 + (block_align − 4ch) / (4ch) × 8`。最後のブロックは詰め物で満たし、`total_frames` で切る |

- PCM の 1 パケットは 1 フレーム。`frames_per_packet` は 1（0 は「指定なし」としてバンクの列の既定値を受け入れる）、それ以外は
  `BadFramesPerPacket`。`block_align` はフレームの大きさか 0（指定なし）、それ以外は `BadBlockAlign`。エンコーダは 1 とフレームの大きさを書く。
- ADPCM の `block_align` は `4ch × (1 + k)`（k ≥ 1）かつ 64 KiB 以下。既定は 512 バイト／ch（1017 フレーム、48 kHz で 21 ms）。
  `frames_per_packet` は 0 か計算値。
- エンコーダ: PCM s16 は最近接丸めで `[-32768, 32767]` に切る。ADPCM は標準の IMA 量子化で、各ブロック・チャンネルの最初のステップ添字を
  （前ブロックの終値と 0, 4, …, 88 から）誤差最小で選ぶ。NaN / Inf の入力は `NonFiniteInput` で拒否。
- シーク表（`SEEK` 行）: `frame` は再生フレーム（プリスキップ後）、`byteOffset` はコーデックデータ先頭から。先頭は必ず `{0, 0}`
  （ストリームの最初のバイト）。プリスキップの中や終端以降で始まるパケットは載せない。`intervalFrames` 以上の間隔でパケット境界を選ぶ。

## D-23 の実装で決めたこと

`snd/ext.h` のコメントが正。そこに書かれていない点を次のように決めた（`snd/codec/codec.hpp` の冒頭にも同じ文）。

1. **保持は圧縮のまま**: 書ききれないフレームは展開済み float ではなく、未デコードの入力バイトとして状態に持つ。ADPCM は 1 ブロックの途中から
   再開できる（チャンネルごとの予測値・添字とブロック内の位置だけ持つ）。よって状態の大きさは `max_frames` に依存しない（A0-2）。
2. **バイトが揃った分から出す**: フレームはその分のバイトが届いた時点で出す（ADPCM もブロックの完成を待たない）。刻み方によらず出力列が同じになる。
   途中で切れたストリームは、届いた分のフレームだけ出る。
3. **吐き出し（`src = NULL`）は終端の意味を持たない**: 保持分を出すだけ。終端はヘッダの `total_frames` で決まり、それ以降の入力は消費して捨てる。
4. **保持の上限**: 1 回の呼び出しの後に状態に残せる未デコードのバイトは `HoldCapacityBytes(h) = 16 KiB + 1 パケット`。超えると
   `SND_DECODE_ERROR`（`Seek` / `Init` まで継続）。呼び出し側の約束: 「`Decode` が `max_frames` を返したら `src = NULL` で `max_frames`
   未満が返るまで吐き出してから次を渡す」「1 回に渡すのは 16 KiB まで」。出力の空きが十分なら何バイト渡してもよい（保持が起きない）。
5. **プリスキップ**: `Init` と `Seek(0)` の後、先頭 `pre_skip` フレームを捨てる。`Seek(f > 0)` は「次のバイトは再生フレーム f
   （デコード位置 `f + pre_skip`）から始まるパケットの先頭」。ADPCM で `f + pre_skip` がブロック境界でなければ次の `Decode` が失敗する。
6. **プリロール**: コーデックは何もしない。呼び出し側が `target − pre_roll` 以前のシーク点を選び（`FindSeekPoint`）、余分を捨てる。
   PCM・ADPCM はブロックが独立なのでビルド時のプリロールは 0。
7. **壊れた入力**: 無効なヘッダ（`ValidateHeader`）でも `StateBytes` は `kMinStateBytes`（128）を返し、`Init` は落ちずに失敗状態を作る。
   以後の `Decode` はすべて `SND_DECODE_ERROR`。ADPCM のステップ添字 > 88 も `SND_DECODE_ERROR`。初期化していない状態（印の不一致）、
   `src = NULL` で `src_bytes > 0`、`max_frames > 0` で `dst` が NULL も同じ。状態の外と `dst[c][max_frames]` 以降には書かない（ASan とガードで確認）。
8. ヘッダの `codec` がオブジェクトの fourcc と違うときも無効なヘッダとして扱う。
9. 状態は `Init` したコーデックを覚えている。別のコーデックの `Decode` に渡すと `SND_DECODE_ERROR`（状態は変えない）、別のコーデックの
   `Seek` に渡すと状態を失敗にする（以後の `Decode` はエラー、正しいコーデックの `Seek` / `Init` で戻る）。
10. `PacketForFrame` と `FindSeekPoint`（5 引数版）は `target >= total_frames` で失敗する（false / `kNoSeekPoint`）。4 引数の
   `FindSeekPoint` は終端を見ない（範囲を自分で確かめる呼び出し側向けに残した）。source は 5 引数版を使う。

## デコーダプール（3.3、D-14）

- `Config.memory.maxDecoders × decoderSlotBytes`（64 に切り上げ）＋管理領域（スロットごとに 4 バイトのリンクと 8 バイトの {世代, 使用中} 語、それぞれ 64 に切り上げ）を初期化時に一括で取る。
  `Init(IAllocator&, …)` は `MemTag::DecoderPool` で 1 回だけ確保、`Init(void*, bytes, …)` は呼び出し側の領域（64 バイト境界）を使う。
- `Acquire(stateBytes, &slot)`: `stateBytes > スロット` なら `StateTooLarge`、空きが無ければ `Full`。どちらも `EndDetailFor` で
  `SND_END_DETAIL_DECODER_POOL_FULL`。`slot` が NULL なら `InvalidArgument` でスロットを消費しない。
- **世代**: `DecoderSlot` は `{index, generation, state}`。取得のたびにそのスロットの世代が 1 進む（0 は使わない）。`Release(slot)` /
  `Release(index, generation)` は使用中かつ世代が一致するときだけ返却する。返却して別の所有者が再取得した後の古い返却は拒否して
  `badReleases` に数える（スロットごとの 64 bit 語の CAS、ロックフリー）。添字だけの `Release(index)` も残したが、世代を確かめないので
  古い返却を見分けられない（添字しか持てない呼び出し側向け。source は世代つきを使うこと）。
- **呼ぶスレッド**: `Acquire` はオーディオスレッド（RunEventLogic で要求を受理し、ソース種別の `Start` がデコードするボイスを始めるとき）、
  `Release` はオーディオスレッド（`Release` 時に実行中のジョブが無い）か先読みレーンの通常ワーカー（解放済みボイスの最後のデコードジョブが
  終わったとき、3.3 のエポック）。両方とも世代タグ付きの Treiber スタック（64 bit CAS）でロックフリー、確保・ロック・システムコールなし。
  何スレッドから同時に呼んでもよい（TSan で確認）。`Init` / `Shutdown` は所有スレッド。
- 使用量: `Usage()` が容量・スロットの大きさ・プール全体のバイト数・使用中・最大使用数・使用中の `stateBytes` 合計・取得回数・
  満杯／大きすぎによる拒否回数・不正な返却回数を返す（relaxed アトミック。他スレッドが動いている間は近似値）。
  `memory_used[SND_MEM_DECODER_POOL]` には `poolBytes` を入れればよい（初期化時に全部確保するので）。
- 二重返却・範囲外の返却・世代の合わない返却は無視して `badReleases` に数える。
- `StateBytes` は「純関数」だが RT 安全は約束されていない（プラグイン）。source はアセットの準備時（バンク読み込み、通常ワーカー）に
  計算して覚え、オーディオスレッドではその値で `Acquire` することを勧める。

## 登録表（D-31）

- `CodecRegistry::Init(const snd_codec*, count)` は `Config.plugins.codecs` をそのまま受け取る（最大 32 = `kMaxPluginsPerKind`）。
  fourcc は `snd_codec.codec`。同じ fourcc の組み込みよりユーザー側を優先する。
- ユーザー側どうしの fourcc の重複、fourcc 0、関数ポインタ NULL は `Init` が失敗を返す（`DuplicateFourcc` / `InvalidEntry`）。
  失敗したら組み込みだけが残る。構成ルートはこれを `snd_create` の失敗（`SND_ERR_INVALID_ARG` 相当）にすることを想定。
- `snd::ToC(ICodec&)` で作った表（C++ API 経由のプラグイン）は元の `ICodec*` に戻して登録する（二重の間接を避ける、D-06）。
  ただの C の表は契約の `snd::CCodec`（1.1 で既定構築可、D-52）越し。
- `Find` は固定配列の線形探索で、どのスレッドからでも呼べる。`SourceServices::findCodec` には `&CodecRegistry::FindForSource`、
  `codecCtx` に登録表を渡す。
- `SND_CODEC_OPUS` はプラグインが無ければ `nullptr` と `LookupFailure::OpusUnavailable` を返す。

## Opus（未実装）

この環境に libopus が無く、サードパーティを取り込まない方針なので段階1では実装しない（A0-3）。`OPUS` の要求は
`Find` → `OpusUnavailable`、`ValidateHeader` → `CodecUnavailable`、`encode::Encode` → `BadHeader` で、どれも理由が分かる形で失敗する。
ゲームが自前の Opus を `Config.plugins.codecs` に fourcc `OPUS` で登録すれば、今の登録表のまま使える。

後から組み込みにするときの場所と注意:

- **差し込み方**: `codec/src/opus.cpp` に `BuiltinDecoder` と同じ形の `ICodec` を置き、`BuiltinCodec()` の switch と
  `IsBuiltinCodec` / `ValidateHeader` に 1 行ずつ足す。ライブラリは `codec/CMakeLists.txt` の任意のオプション
  （例 `SND_CODEC_OPUS=ON` で `find_package` / ベンダー化）で入れ、見つからなければ今の振る舞い（`OpusUnavailable`）のまま。
  エンコーダは `encode.cpp` と同じく別オブジェクトにしてランタイムに入れない。
- **プリスキップ**: 標準はエンコーダ遅延 312 フレーム（48 kHz、OpusHead の pre-skip）。ヘッダの `pre_skip` に入れ、`Init` / `Seek(0)` の後に
  捨てる（今の駆動部の `pos < preSkip` の処理がそのまま使える）。`total_frames` はプリスキップ後の長さ。ループ点はサンプル精度（6.4）。
- **プリロール**: RFC 7845 の推奨は 80 ms（3840 フレーム）。ヘッダの `pre_roll` に入れ、`FindSeekPoint(…, pre_roll)` で前のパケットから
  デコードして捨てる。シーク表はパケット境界（可変長なのでビルド時に実際の位置を記録する。`BuildSeekTable` の固定長用の計算は使えない）。
- **余りの保持（D-23）とスロット**: libopus はパケットを丸ごと 1 回で展開するので、ADPCM のように途中から再開できない。書ききれない
  フレームは展開済みの float で持つ必要がある。見積もり（libopus が無いので実測ではない）:
  - libopus の状態: `opus_decoder_get_size` はモノラル約 18 KB、ステレオ約 26 KB（版で変わる）。3ch 以上は multistream で
    ストリーム数ぶん増える（5.1 で 4 ストリーム ≒ 80 KB 超）。
  - 展開結果の保持: 1 パケットの最大は 120 ms = 5760 フレーム。ステレオで 46 KB、20 ms フレームに限れば 960 × 2 × 4 = 7.5 KB。
  - 入力の保持: 1 パケット最大 1275 バイト × フレーム数＋上の 16 KiB。
  - 合計はステレオ 20 ms でも 26 + 7.5 + 17 ≒ 50 KB で、**既定の `decoderSlotBytes` 32 KiB に収まらない**。対応案: (a) buildtool で
    20 ms 単一フレームのパケットに限り、Opus 用の保持を「1 パケットの展開結果」に固定（入力の 16 KiB 保持は小さくする）、(b) Opus を使う
    プロジェクトは `decoderSlotBytes` を 64 KiB にする、(c) スロットの大きさを 2 種類持つ（契約変更）。3ch 以上の Opus は (c) か
    チャンネル数の上限が要る。
- `extra`（バンクの `extraOffset/extraBytes`）に OpusHead 相当（チャンネル写像、ストリーム数）を入れる。

## A0-2: `StateBytes` の実測

`Encode.StateBytesFitDefaultDecoderSlot` が出力する表（gcc 13、clang 18 とも同じ値）。単位はバイト、スロットの既定は 32768。
`StateBytes = 128（状態の頭）+ 64 に切り上げた保持容量`、保持容量 = 16384 + 1 パケット。

| コーデック | ch | block_align | StateBytes | 保持容量 |
| --- | --- | --- | --- | --- |
| PCM_F32 / PCM_S16 | 1〜8 | 4ch / 2ch | 16576 | 16388〜16416 |
| ADPCM 256 B/ch | 1 / 2 / 6 / 8 | 256 / 512 / 1536 / 2048 | 16768 / 17024 / 18048 / 18560 | |
| ADPCM 512 B/ch（既定） | 1 / 2 / 6 / 8 | 512 / 1024 / 3072 / 4096 | 17024 / 17536 / 19584 / 20608 | |
| ADPCM 1024 B/ch | 1 / 2 / 6 / 8 | 1024 / 2048 / 6144 / 8192 | 17536 / 18560 / 22656 / 24704 | |
| ADPCM 2048 B/ch | 1 / 2 / 6 / 8 | 2048 / 4096 / 12288 / 16384 | 18560 / 20608 / 28800 / **32896（超過）** | |

- `max_frames`（1、256、4096、65536）では変わらない（保持を圧縮のまま持つため）。テストは `StateBytes` ちょうどの領域＋ガードで
  各 `max_frames` のデコードを行い、はみ出しが無いことを確かめている。
- 結論: PCM と、1 チャンネルあたり 1024 バイト以下のブロックの ADPCM は 8ch まで既定の 32 KiB に収まる。**既定値の見直しは不要**。
  buildtool は ADPCM のブロックを 1 チャンネルあたり 1024 バイト以下にすること（超えると `DECODER_POOL_FULL` で再生拒否になる）。
- Opus は上の見積もりのとおり 32 KiB に収まらない見込み。組み込むときに再測定する。

## codec 内に持った汎用部品（base へ移す候補）

| 部品 | 場所 | 内容 |
| --- | --- | --- |
| `detail::AlignUp` | `src/detail.hpp` | 2 の冪への切り上げ |
| `detail::LoadLE16/32`, `StoreLE16/32` | `src/detail.hpp` | エンディアン非依存のリトルエンディアン読み書き |
| タグ付き Treiber スタック | `src/decoder_pool.cpp`（`DecoderPool::Acquire/Release` 内） | 添字の MPMC ロックフリー空きリスト（`atomic<u64>` の頭＋`atomic<u32>` のリンク）。base の固定ブロックプールが同じ性質（任意スレッド・ロックフリー）を持てば置き換える |

## テスト

`codec/test/`（`codec_test`、`codec_runtime_test`、`codec_runtime_has_no_encoder`）。

- 往復: PCM f32 はビット一致、s16 は ±0.5 LSB、ADPCM は 1/2/6/8ch × 64〜1024 B/ch で SNR > 25 dB（ステレオ既定で約 29 dB）。
- 刻みの不変性: 3 コーデック × 1/2/3/8ch × 刻み（1 バイト、2,3,5、パケット±1、パケット×3+7、7000、乱数）× `max_frames`（1, 7, 256, 4096）で、
  一括デコードとビット一致。吐き出さずに積む使い方も確認。
- シーク: シーク表と `PacketForFrame` の両方で、`pre_roll` 0 / 300、各種の目標位置について連続デコードとビット一致。プリスキップ付きも。
- 壊れた入力: 無効なヘッダ 14 種、途中で切れたストリーム、ステップ添字の破損、未初期化の状態、NULL 引数、ブロック境界でないシーク、
  保持の上限超え、乱数のヘッダとバイト列 300 回。状態・出力の後ろのガードが壊れないこと（ASan でも確認）。
- プール: 満杯・大きすぎ・二重返却・古い世代の返却（並行中も）・NULL 出力・使用量・呼び出し側メモリ・0 個・確保失敗、4 スレッドの同時取得返却（TSan）、スロット内でのデコード。
- RT: デコード経路（`StateBytes`/`Init`/`Decode`/`Seek`）、`Acquire`/`Release`、`Find` を `SND_RT_SCOPE` の中で実行して検出ゼロ。
- ゴールデン: `golden/adpcm_stereo_256.sndg`（ADPCM のエンコード→デコード結果、許容差 0）。デコーダかブロック配置が変わると落ちる。
