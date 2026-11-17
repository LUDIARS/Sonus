# codec モジュール

設計書 6.4（コーデック）、3.3（デコーダプール）、12.1、契約 D-14・D-23・D-31。依存は `base` だけ（ルート `CMakeLists.txt`）。
汎用部品は base のものを使う（末尾「汎用部品」）。

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
snd::ICodec* c = registry.Find(asset.codec.codec, &why);    // nullptr なら why = NotRegistered / OpusUnavailable（OFF のビルド）
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
- `SND_CODEC_OPUS` は組み込み（D-56）。`SND_CODEC_OPUS=OFF` のビルドでプラグインも無ければ `nullptr` と `LookupFailure::OpusUnavailable` を返す。

## Opus（CONTRACTS.md D-56〜D-60、bank-format.md §6.5）

同梱の libopus 1.5.2（`third_party/opus/`、`SND_CODEC_OPUS`、既定 ON）で組み込みにした。OFF のビルドでは `opus_decoder.cpp` /
`encode_opus.cpp` はスタブになり、`OPUS` の要求は従来どおり `Find` → `OpusUnavailable`、`ValidateHeader` → `CodecUnavailable`、
`EncodeOpus` → `CodecUnavailable` で失敗する（`OpusAvailable()` / `encode::OpusEncoderAvailable()` で判定できる）。
ゲームが自前の Opus を `Config.plugins.codecs` に fourcc `OPUS` で登録すれば、組み込みより優先される（D-31）。

| ファイル | 中身 | ランタイム |
| --- | --- | --- |
| `src/header.cpp` | `ParseOpusSetup`, `OpusMaxPacketBytes`、`ValidateHeader` の Opus 分（libopus を使わない） | 入る |
| `src/opus_decoder.cpp` | `ICodec` の実装（libopus の `opus_multistream_decode_float`） | 入る |
| `src/encode_opus.cpp` | `encode::EncodeOpus`, `BuildOpusSeekTable`（libopus のエンコーダ） | 入らない（`codec_runtime_has_no_encoder` が libopus のエンコーダの記号も検査） |

形式（bank-format.md §6.5 が正）: 48 kHz、20 ms（960 フレーム）のパケットを `u16 LE の長さ + パケット` で並べる。
setup（`extra`、`4 + ch` バイト）に版・チャンネル数・ストリーム数・結合ストリーム数・マッピング。1〜2ch は 1 ストリーム、
3〜8ch はサラウンドのマルチストリーム。`preSkip` 312（エンコーダの先読み）、`preRoll` 3840（80 ms）。

### デコーダ（D-23 の実装）

- 状態: `[頭 128 B][入力の保持][デコード済み 960 × ch の float][OpusMSDecoder]`（各 64 バイト境界）。`opus_multistream_decoder_init`
  でスロットの中に置き、確保しない。頭の 4 語（印・fourcc・ch・失敗）は他の組み込みと同じ並びなので、別のコーデックの状態を渡されると
  見分けられる（README の 9）。
- 保持: 未デコードの入力は圧縮のまま（上限 `HoldCapacityBytes` = 16 KiB + 2 + ストリーム数 × 1277。D-51 の「16 KiB ＋ 1 パケット」）。
  libopus はパケットの途中から再開できないので、`max_frames` に入りきらなかった 1 パケット分はデコード済みの float で持ち、次の呼び出しで
  先に返す。パケットは出力に空きがあるときだけデコードする（満杯で止まった後にシークされても無駄にならない）。
- 刻みの不変性: パケットが揃った時点で 1 パケットずつデコードするので、入力の刻み方と `max_frames` によらず出力はビット単位で同じ。
- プリスキップと終端: `Init` / `Seek(0)` の後、先頭 `pre_skip` フレームを捨てる。`total_frames` を超える分は出さず、終端より後で始まる
  パケットはデコードせずに消費する。
- シーク: `Seek(f)` は libopus の状態を `OPUS_RESET_STATE` で戻し、次のバイトをデコード位置 `f + pre_skip` のパケットの前置とみなす。
  `f + pre_skip` が 960 の倍数でなければ次の `Decode` が失敗する。シーク表は buildtool が `BuildOpusSeekTable` で作る
  （`PacketForFrame` は固定長用なので Opus では false）。
- 壊れた入力: 長さ 0・上限超え、libopus のエラー、960 フレームでないパケットは `SND_DECODE_ERROR`（`Init` / `Seek` まで継続）。
- `StateBytes`（libopus 1.5.2 の実測）: 1ch 40,192 / 2ch 52,800 / 3ch 76,160 / 4ch 88,768 / 5ch 112,192 / 6ch 135,552 /
  7ch 148,160 / 8ch 171,584 バイト。旧既定の `decoderSlotBytes` 32 KiB には入らないので、D-58 で既定を 64 KiB（1〜2ch が入る）に
  上げた。3ch 以上は 192 KiB 以上のスロットが要る（buildtool が警告する）。足りなければ `DECODER_POOL_FULL`。
- RT: `Init` / `Decode` / `Seek` は確保・ロック・IO をしない（libopus は `VAR_ARRAYS` でビルド、作業領域はスタック）。スタックは
  スレッド全体で 25〜32 KB（1〜8ch）、D-47 の 256 KiB に収まる。デコードは 1ch・20 ms あたり約 0.1 ms（Debug）。

### エンコーダ（buildtool）

```cpp
namespace ce = snd::codec::encode;
ce::OpusSettings st;              // bitrate 0 = 64 kbit/s × ch、complexity 10、VBR、signal auto、independentFrames true
std::vector<uint8_t> out(ce::OpusMaxEncodedBytes(ch, frames));
ce::OpusEncoded e;                // e.header（extra は e.setup を指す）、e.setup / e.setupBytes、e.bytes、e.packets
ce::EncodeOpus(st, planar, ch, 48000, frames, out.data(), out.size(), &e);
uint32_t n = ce::BuildOpusSeekTable(e.header, out.data(), e.bytes, intervalFrames, nullptr, nullptr, 0);  // 個数
ce::BuildOpusSeekTable(e.header, out.data(), e.bytes, intervalFrames, seekFrames, seekOffsets, n);
```

- 入力は 48 kHz（それ以外は `BadArguments`。buildtool が変換する）、チャンネルは WAV の順。3ch 以上は Vorbis の順に並べ替えて
  `opus_multistream_surround_encoder` で符号化し、`mapping` を WAV の順で書く。
- `independentFrames`（既定 true）は `OPUS_SET_PREDICTION_DISABLED`。CELT のパケットではシーク点から 2 パケット後以降の出力が
  通しのデコードとビット単位で一致する（80 ms のプリロールでシーク・ループがサンプル単位で正確）。SILK / ハイブリッド（低ビットレートの
  音声）は近似。false にすると予測ありで、80 ms 後の誤差は ~1e-1。
- `Encode()` / `EncodedBytes()` / `BuildSeekTable()` は固定長の組み込み用で、Opus には `BadHeader` / 0 を返す。
- 差分ビルド: `kOpusEncoderVersion`（libopus の版を含む）。libopus を更新したら上げる（`third_party/opus/SONUS_README.md`）。

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
- 結論: PCM と、1 チャンネルあたり 1024 バイト以下のブロックの ADPCM は 8ch まで 32 KiB に収まる（段階1の結論。既定は Opus のため D-58 で 64 KiB に上げた）。
  buildtool は ADPCM のブロックを 1 チャンネルあたり 1024 バイト以下にすること（超えると `DECODER_POOL_FULL` で再生拒否になる）。
- Opus は 32 KiB に収まらない（上の「Opus」の実測。D-58）。

## 汎用部品（統合で base へ移した、A1-12）

整列（`base::AlignUp64`）とリトルエンディアンの読み書き（`base::LoadLE16/32`、`StoreLE16/32`、`snd/base/endian.hpp`）は base のものを使う。
デコーダプールの空きリスト（タグ付き Treiber スタック、`src/decoder_pool.cpp`）は世代つきの語と一体なので codec に残した。

## テスト

`codec/test/`（`codec_test`、`codec_opus_test`、`codec_runtime_test`、`codec_runtime_has_no_encoder`）。

- Opus（`codec_opus_test`、全構成でビルド。OFF では「無い」ことだけを確かめる）: 往復（100〜8000 Hz の正弦波で基本波 ±0.5 dB・
  THD < −40 dB・THD+N < −30 dB・位相から遅延ゼロ（エンコーダの 3 Hz の DC 除去の位相進みを差し引く）、白色雑音の帯域エネルギー ±1.5 dB と
  相互相関の遅延 0、無音、2/3/6/8ch のチャンネルごとの別信号と漏れ）、刻みと `max_frames` の不変性、終端、シーク（サンプル単位の一致）、
  保持の上限、別コーデックの状態、壊れた入力のファズ、RT スコープ、スタック使用量と処理時間、`StateBytes` とプール。

- 往復: PCM f32 はビット一致、s16 は ±0.5 LSB、ADPCM は 1/2/6/8ch × 64〜1024 B/ch で SNR > 25 dB（ステレオ既定で約 29 dB）。
- 刻みの不変性: 3 コーデック × 1/2/3/8ch × 刻み（1 バイト、2,3,5、パケット±1、パケット×3+7、7000、乱数）× `max_frames`（1, 7, 256, 4096）で、
  一括デコードとビット一致。吐き出さずに積む使い方も確認。
- シーク: シーク表と `PacketForFrame` の両方で、`pre_roll` 0 / 300、各種の目標位置について連続デコードとビット一致。プリスキップ付きも。
- 壊れた入力: 無効なヘッダ 14 種、途中で切れたストリーム、ステップ添字の破損、未初期化の状態、NULL 引数、ブロック境界でないシーク、
  保持の上限超え、乱数のヘッダとバイト列 300 回。状態・出力の後ろのガードが壊れないこと（ASan でも確認）。
- プール: 満杯・大きすぎ・二重返却・古い世代の返却（並行中も）・NULL 出力・使用量・呼び出し側メモリ・0 個・確保失敗、4 スレッドの同時取得返却（TSan）、スロット内でのデコード。
- RT: デコード経路（`StateBytes`/`Init`/`Decode`/`Seek`）、`Acquire`/`Release`、`Find` を `SND_RT_SCOPE` の中で実行して検出ゼロ。
- ゴールデン: `golden/adpcm_stereo_256.sndg`（ADPCM のエンコード→デコード結果、許容差 0）。デコーダかブロック配置が変わると落ちる。
