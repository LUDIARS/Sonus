# バンクのバイナリ仕様（形式 2.0）

設計書 10.2 の「バイト単位の仕様」。**凍結対象**（`spec/interface/CONTRACTS.md`）。
正本の構造体定義は `contract/internal/include/snd/format/bank_format.hpp`。本書と食い違う場合はヘッダが正で、
食い違い自体を契約の不具合として報告する。列表（§6）はヘッダの `SND_BANK_COLUMNS` から機械的に生成したもの。

## 1. 原則

- **エンディアン**: リトルエンディアンのみ（`endianness = 1`）。ビッグエンディアン機種が対象になった時点で形式 2.0 を検討する。
- **ポインタなし**: 参照はすべて「ファイル先頭からのバイトオフセット（u64）」か「同じバンク内テーブルの行番号（u32）」。
  64 バイト境界に読み込めば補正なしでそのまま使える（10.2）。
- **アライメント**: すべてのチャンクと、テーブルチャンク内のすべての列配列は 64 バイト境界から始まる（`alignLog2 = 6`）。
  隙間は 0 で埋める。
- **「なし」の値**: 行参照・文字列参照の「なし」は `0xFFFFFFFF`（`kNone`）。ID の「なし」は 0。
- **単位**: 時間は ms（f32）、レベルは dB（f32）、ピッチはセント（f32）、位置はフレーム（u64）、音楽位置は tick（i64、1 拍 960）。
- **ID**: ランタイム ID は `snd_hash_id`（FNV-1a 32、`snd/id.h`）。プロジェクト全体で衝突しないことを buildtool が保証する（10.1）。

## 2. ファイル全体の配置

```
offset 0      FileHeader (128 B)
              Dependency[dependencyCount]      (dependencyOffset、16 B 単位)
              ChunkEntry[chunkCount]           (chunkTableOffset、32 B 単位)
64 の倍数     チャンク本体 ...                 (ChunkEntry.offset 順でなくてよい)
fileBytes     終端
```

ヘッダ以外の並び順は自由だが、buildtool は「ヘッダ → 依存表 → チャンク目次 → チャンク（目次順）」で書く。

## 3. FileHeader（128 バイト）

| offset | 型 | 名前 | 内容 |
| --- | --- | --- | --- |
| 0 | u32 | magic | `'SNDB'`（バイト列 `53 4E 44 42`） |
| 4 | u16 | formatMajor | 2。不一致なら読み込み失敗（`SND_ERR_VERSION_MISMATCH`） |
| 6 | u16 | formatMinor | 0。ランタイムより新しくても読める（§7） |
| 8 | u32 | platform | 機種タグ fourcc（`ANY_`, `WIN_`, `LNX_`, `MAC_`, `IOS_`, `AND_`）。`ANY_` 以外はランタイムの機種と一致が必要 |
| 12 | u8 | endianness | 1 = little |
| 13 | u8 | alignLog2 | 6 |
| 14 | u16 | headerBytes | 128 |
| 16 | u32 | flags | bit0 開発ビルド（GUID あり、上書き層用にテーブルを書き込み可能領域へ複製）、bit1 文字列あり、bit2 言語バンク |
| 20 | u32 | bankId | バンク名の ID |
| 24 | u64 | fileBytes | ファイル全体のバイト数 |
| 32 | u64 | projectBuildHash | プロジェクトのビルドハッシュ（ライブリンクの照合、11.2） |
| 40 | u64 | contentHash | このバンク内容のハッシュ（差分ビルドのキャッシュキー、依存の照合、10.3） |
| 48 | u32 | checksum | `[headerBytes, fileBytes)` の CRC-32C |
| 52 | u32 | headerChecksum | ヘッダ全体 `[0, 128)` の CRC-32C。ただしこの欄自身（`[52, 56)`）は 0 とみなす（`bank_format::HeaderChecksum`、2.0） |
| 56 | u32 | dependencyCount | |
| 60 | u32 | chunkCount | |
| 64 | u64 | dependencyOffset | `Dependency` 配列の位置 |
| 72 | u64 | chunkTableOffset | `ChunkEntry` 配列の位置 |
| 80 | u32 | languageId | 言語バンクの言語 ID（10.4）、それ以外 0 |
| 84 | u32 | reserved0 | 0（0 以外は破損） |
| 88 | u8[40] | reserved | 0（0 以外は破損） |

**CRC-32C**: Castagnoli、反射、多項式 `0x82F63B78`、初期値・最終 XOR `0xFFFFFFFF`。`"123456789"` → `0xE3069283`。
参照実装は `bank_format::Crc32c`。検証の順序は headerChecksum → magic/版/機種/エンディアン → checksum。
ヘッダのどのバイトもいずれかのチェックサムで守られる（1.0 では `[52, 128)` が無保護だった）。
`endianness` が 1 以外なら `SND_ERR_VERSION_MISMATCH`。`fileBytes` は読んだ長さと正確に一致すること（末尾のパディングも許さない）。
`languageId` は言語フラグ（bit2）が無ければ 0。

### Dependency（16 バイト）

| offset | 型 | 名前 | 内容 |
| --- | --- | --- | --- |
| 0 | u32 | bankId | 依存先バンクの ID |
| 4 | u32 | reserved | 0 |
| 8 | u64 | buildHash | ビルド時の依存先の `contentHash`。ロード済みの依存先と不一致なら `SND_ERR_BANK_DEPENDENCY` |

依存先が未ロードなら登録しない（10.3）。

## 4. ChunkEntry（32 バイト）

| offset | 型 | 名前 | 内容 |
| --- | --- | --- | --- |
| 0 | u32 | fourcc | チャンク種別（§6） |
| 4 | u16 | version | チャンク内レイアウトの版。現行はすべて 1 |
| 6 | u16 | flags | bit0 テーブル（§5 の形）、bit1 リリースで除去可、bit2 開発ビルドのみ、bit3 アプリケーションチャンク（`snd_get_bank_chunk` で読める） |
| 8 | u32 | reserved0 | 0 |
| 12 | u32 | reserved1 | 0 |
| 16 | u64 | offset | ファイル先頭から、64 の倍数 |
| 24 | u64 | bytes | チャンクのバイト数 |

同じ fourcc のチャンクは 1 バンクに 1 つまで。

## 5. テーブルチャンク（SoA）

```
+0                TableChunkHeader (16 B)
+16               ColumnEntry[columnCount] (16 B each)
64 の倍数         列 0 の配列: rowCount * elemBytes
64 の倍数         列 1 の配列 ...
```

### TableChunkHeader（16 バイト）

| offset | 型 | 名前 |
| --- | --- | --- |
| 0 | u32 | rowCount |
| 4 | u32 | columnCount |
| 8 | u32 | reserved0 |
| 12 | u32 | reserved1 |

### ColumnEntry（16 バイト）

| offset | 型 | 名前 | 内容 |
| --- | --- | --- | --- |
| 0 | u16 | columnId | §6 の列 ID（テーブルごとに 0 から連番） |
| 2 | u16 | elemBytes | 列の型の大きさ（U32/F32 = 4、U64/I64 = 8）と一致しなければ破損扱い |
| 4 | u32 | reserved | 0 |
| 8 | u64 | offset | **チャンク先頭から**の位置、64 の倍数 |

- 列の並び順は任意。同じ列 ID が 2 回出たら破損。
- 未知の列 ID は無視する（新しい minor 版が足した列）。
- 必須列が無ければ破損（`SND_ERR_CORRUPT`）。任意列が無ければ全行が既定値。
- ランタイムはテーブルを `schema.hpp` の実行時テーブルにコピーしない。バンク内の列配列をそのまま参照する
  （開発ビルドでは上書き層のために書き込み可能領域へ複製する、11.2）。

## 6. チャンクと列

| fourcc | 種類 | フラグ | 内容 |
| --- | --- | --- | --- |
| `EVNT` | Table | 0 | events (10.1, 4.2) |
| `TRAK` | Table | 0 | tracks of events |
| `CLIP` | Table | 0 | clips on tracks |
| `ACTN` | Table | 0 | action track items (4.2) |
| `BUSS` | Table | 0 | buses in graph order (4.6) |
| `SEND` | Table | 0 | bus sends |
| `EFFX` | Table | 0 | effect instances |
| `EFPV` | Table | 0 | effect initial parameter values |
| `EFBL` | Blob | 0 | effect blobs (impulse responses ...) |
| `PARM` | Table | 0 | parameter definitions (4.3) |
| `CURV` | Table | 0 | curves |
| `CPNT` | Table | 0 | curve points |
| `CATG` | Table | 0 | categories (4.6) |
| `SNAP` | Table | 0 | snapshots (4.6) |
| `SNPV` | Table | 0 | snapshot values |
| `VLGR` | Table | 0 | voice limit groups (4.4) |
| `DUCK` | Table | 0 | ducking rules (4.6) |
| `ASST` | Table | 0 | waveform assets (6.2) |
| `WAVE` | Blob | 0 | resident waveform data, each asset 64-byte aligned |
| `PREL` | Blob | 0 | stream / music entry preload chunks (6.1, 8.3) |
| `SEEK` | Table | 0 | seek tables (6.3) |
| `STRM` | Table | 0 | external stream files |
| `SPTH` | Blob | 0 | stream file paths, NUL-terminated UTF-8 (not strippable) |
| `SYNP` | Table | 0 | synthesised source parameters (5) |
| `PTCH` | Blob | 0 | patch programs (5.1, stage 6) |
| `DSPN` | Blob | 0 | DSP instance pool requirement (3.3): DspPoolNeed |
| `MSEG` | Table | 0 | music segments (8.3) |
| `MPLS` | Table | 0 | music playlists |
| `MPLE` | Table | 0 | music playlist entries |
| `MSTM` | Table | 0 | music stems |
| `MSTA` | Table | 0 | music states |
| `MTRN` | Table | 0 | music transition rules in match order (8.5) |
| `MSTG` | Table | 0 | music stingers |
| `MMRK` | Table | 0 | music markers |
| `MTMP` | Table | 0 | tempo map points |
| `PHYS` | Blob | kChunkApplication | physics material tables (9): PhysChunkHeader |
| `STRS` | Blob | kChunkStrippable | string pool, NUL-terminated UTF-8 |
| `NAME` | Table | kChunkStrippable | id -> name (and rename aliases, 10.1) |
| `GUID` | Table | kChunkDevOnly | id -> GUID (dev builds, 11.2) |
| `ALIS` | Table | 0 | rename aliases: old id -> current id (10.1, not strippable, 2.0) |


#### `EVNT`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | trackFirst | U32 | 必須 | — |  |
| 2 | trackCount | U32 | 必須 | — |  |
| 3 | sequenceKind | U32 | 必須 | — | SequenceKind |
| 4 | priority | U32 | 必須 | 128 | 0..255, higher wins（必須列だが書き手の既定は 128） |
| 5 | bus | U32 | 必須 | — | BUSS row |
| 6 | category | U32 | 任意 | none (0xFFFFFFFF) | CATG row |
| 7 | limitGroup | U32 | 任意 | none (0xFFFFFFFF) | VLGR row |
| 8 | maxInstances | U32 | 任意 | 0 | 0 = unlimited |
| 9 | retriggerMs | F32 | 任意 | 0 |  |
| 10 | flags | U32 | 必須 | — | EventFlags |
| 11 | curveVolume | U32 | 任意 | none (0xFFFFFFFF) | CURV rows: distance curves (4.5) |
| 12 | curveLowpass | U32 | 任意 | none (0xFFFFFFFF) |  |
| 13 | curveSpread | U32 | 任意 | none (0xFFFFFFFF) |  |
| 14 | curveReverbSend | U32 | 任意 | none (0xFFFFFFFF) |  |
| 15 | curveBlend3D | U32 | 任意 | none (0xFFFFFFFF) |  |
| 16 | maxDistance | F32 | 任意 | 0 |  |
| 17 | coneInnerDeg | F32 | 任意 | 0 |  |
| 18 | coneOuterDeg | F32 | 任意 | 0 |  |
| 19 | coneOuterDb | F32 | 任意 | 0 |  |
| 20 | fadeOutMs | F32 | 任意 | 0 |  |
| 21 | musicState | U32 | 任意 | none (0xFFFFFFFF) | MSTA row: initial state of a music event |
| 22 | curveFirst | U32 | 任意 | none (0xFFFFFFFF) | parameter curves on this event |
| 23 | curveCount | U32 | 任意 | 0 |  |
| 24 | comboWindowMs | F32 | 任意 | 0 |  |
| 25 | virtualReturn | U32 | 任意 | 0 | 0 continue, 1 restart, 2 never (4.4) |

#### `TRAK`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | event | U32 | 必須 | — |  |
| 1 | clipFirst | U32 | 必須 | — |  |
| 2 | clipCount | U32 | 必須 | — |  |
| 3 | flags | U32 | 任意 | 0 |  |
| 4 | actionFirst | U32 | 任意 | none (0xFFFFFFFF) |  |
| 5 | actionCount | U32 | 任意 | 0 |  |
| 6 | layerParam | U32 | 任意 | 0 | param id for layer crossfade, 0 none |
| 7 | bus | U32 | 任意 | none (0xFFFFFFFF) |  |

#### `CLIP`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | kind | U32 | 必須 | — | ClipKind |
| 1 | target | U32 | 必須 | — | ASST / EVNT (nested) / SYNP row |
| 2 | startMs | F32 | 任意 | 0 |  |
| 3 | weight | F32 | 任意 | 1 |  |
| 4 | volumeDb | F32 | 任意 | 0 |  |
| 5 | pitchCents | F32 | 任意 | 0 |  |
| 6 | randVolumeDb | F32 | 任意 | 0 |  |
| 7 | randPitchCents | F32 | 任意 | 0 |  |
| 8 | randPan | F32 | 任意 | 0 |  |
| 9 | randStartMs | F32 | 任意 | 0 |  |
| 10 | randFilterHz | F32 | 任意 | 0 |  |
| 11 | switchValue | U32 | 任意 | 0 |  |
| 12 | layerMin | F32 | 任意 | 0 |  |
| 13 | layerMax | F32 | 任意 | 0 |  |
| 14 | fadeInMs | F32 | 任意 | 0 |  |
| 15 | fadeOutMs | F32 | 任意 | 0 |  |
| 16 | loopCount | U32 | 任意 | 1 | 0 = infinite |

#### `ACTN`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | timeMs | F32 | 必須 | — |  |
| 1 | action | U32 | 必須 | — | ActionKind |
| 2 | target | U32 | 必須 | — | runtime id |
| 3 | value | F32 | 任意 | 0 |  |

#### `BUSS`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | parent | U32 | 必須 | — | row; kNone only for the master bus |
| 2 | sendFirst | U32 | 任意 | none (0xFFFFFFFF) |  |
| 3 | sendCount | U32 | 任意 | 0 |  |
| 4 | effectFirst | U32 | 任意 | none (0xFFFFFFFF) |  |
| 5 | effectCount | U32 | 任意 | 0 |  |
| 6 | volumeDb | F32 | 任意 | 0 |  |
| 7 | channels | U32 | 任意 | 0 | 0 = parent's |
| 8 | flags | U32 | 任意 | 0 | BusFlags |
| 9 | lodPriority | U32 | 任意 | 0 |  |

#### `SEND`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | targetBus | U32 | 必須 | — |  |
| 1 | levelDb | F32 | 任意 | 0 |  |
| 2 | preFader | U32 | 任意 | 0 |  |

#### `EFFX`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | effectType | U32 | 必須 | — | HashId of the kind (standard or plugin) |
| 1 | paramFirst | U32 | 任意 | none (0xFFFFFFFF) | EFPV row |
| 2 | paramCount | U32 | 任意 | 0 |  |
| 3 | blobOffset | U64 | 任意 | 0 | within EFBL |
| 4 | blobBytes | U64 | 任意 | 0 |  |
| 5 | stateBytes | U32 | 任意 | 0 | computed by buildtool for DSPN |
| 6 | flags | U32 | 任意 | 0 |  |

#### `EFPV`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | value | F32 | 必須 | — |  |

#### `PARM`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | kind | U32 | 必須 | — | 0 continuous, 1 switch |
| 2 | scopeMask | U32 | 任意 | 7 | bit0 global, bit1 emitter, bit2 playback |
| 3 | minValue | F32 | 任意 | 0 |  |
| 4 | maxValue | F32 | 任意 | 1 |  |
| 5 | defaultValue | F32 | 任意 | 0 |  |
| 6 | rampRate | F32 | 任意 | 0 | units per second, 0 = immediate |

#### `CURV`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | pointFirst | U32 | 必須 | — |  |
| 1 | pointCount | U32 | 必須 | — |  |
| 2 | input | U32 | 必須 | — | param id, or kInputDistance |
| 3 | target | U32 | 必須 | — | CurveTarget |
| 4 | compose | U32 | 任意 | 0 | CurveCompose (4.3) |

#### `CPNT`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | x | F32 | 必須 | — |  |
| 1 | y | F32 | 必須 | — |  |
| 2 | shape | F32 | 任意 | 0 | 0 linear, >0 exponent |

#### `CATG`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | parent | U32 | 任意 | none (0xFFFFFFFF) |  |
| 2 | flags | U32 | 任意 | 0 | CategoryFlags |
| 3 | volumeDb | F32 | 任意 | 0 |  |

#### `SNAP`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | valueFirst | U32 | 必須 | — |  |
| 2 | valueCount | U32 | 必須 | — |  |

#### `SNPV`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | targetKind | U32 | 必須 | — | 0 bus volume, 1 effect param, 2 send level |
| 1 | targetRow | U32 | 必須 | — |  |
| 2 | paramIndex | U32 | 任意 | 0 |  |
| 3 | value | F32 | 必須 | — |  |

#### `VLGR`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | maxVoices | U32 | 必須 | — |  |
| 2 | stealMode | U32 | 任意 | 0 | 0 oldest, 1 quietest, 2 lowest priority, 3 never |

#### `DUCK`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | kind | U32 | 必須 | — | 0 sidechain (source = bus row), 1 rule (source = category row) |
| 1 | source | U32 | 必須 | — |  |
| 2 | targetBus | U32 | 必須 | — |  |
| 3 | amountDb | F32 | 必須 | — |  |
| 4 | attackMs | F32 | 任意 | 10 |  |
| 5 | releaseMs | F32 | 任意 | 200 |  |
| 6 | thresholdDb | F32 | 任意 | 0 |  |

#### `ASST`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | loadForm | U32 | 必須 | — | LoadForm (6.2) |
| 2 | codec | U32 | 必須 | — | SND_CODEC_* fourcc |
| 3 | channels | U32 | 必須 | — |  |
| 4 | sampleRate | U32 | 必須 | — |  |
| 5 | totalFrames | U64 | 必須 | — |  |
| 6 | preSkip | U32 | 任意 | 0 |  |
| 7 | preRoll | U32 | 任意 | 0 |  |
| 8 | framesPerPacket | U32 | 任意 | 0 |  |
| 9 | blockAlign | U32 | 任意 | 0 |  |
| 10 | dataOffset | U64 | 任意 | 0 | within WAVE (resident forms) |
| 11 | dataBytes | U64 | 任意 | 0 |  |
| 12 | loopStart | U64 | 任意 | 0 |  |
| 13 | loopEnd | U64 | 任意 | 0 | ループ終端（**含まない**、再生フレーム）。0 = ループ無し。サンプル精度（6.3） |
| 14 | preloadOffset | U64 | 任意 | 0 | within PREL |
| 15 | preloadBytes | U32 | 任意 | 0 |  |
| 16 | preloadFrames | U32 | 任意 | 0 |  |
| 17 | seekFirst | U32 | 任意 | none (0xFFFFFFFF) |  |
| 18 | seekCount | U32 | 任意 | 0 |  |
| 19 | stream | U32 | 任意 | none (0xFFFFFFFF) | STRM row (Stream form) |
| 20 | extraOffset | U64 | 任意 | 0 | codec setup data within WAVE |
| 21 | extraBytes | U32 | 任意 | 0 |  |
| 22 | flags | U32 | 任意 | 0 | AssetFlags |

#### `SEEK`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | frame | U64 | 必須 | — |  |
| 1 | byteOffset | U64 | 必須 | — | **データ先頭からの**位置（ストリームは `STRM.dataOffset`、常駐は `ASST.dataOffset` が 0）。`frame` は再生フレーム。各アセットの先頭行は `{0, 0}`、`frame` は狭義増加、`byteOffset` は非減少 |

#### `STRM`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | pathOffset | U32 | 必須 | — | within SPTH |
| 1 | fileBytes | U64 | 必須 | — |  |
| 2 | dataOffset | U64 | 任意 | 0 | first packet, sector aligned |
| 3 | sectorAlign | U32 | 任意 | 2048 |  |

#### `SYNP`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | kind | U32 | 必須 | — | SynthKind |
| 1 | waveform | U32 | 任意 | 0 | Waveform |
| 2 | frequencyHz | F32 | 任意 | 440 |  |
| 3 | duty | F32 | 任意 | 0.5 |  |
| 4 | noiseColor | U32 | 任意 | 0 | 0 white, 1 pink, 2 brown |
| 5 | asset | U32 | 任意 | none (0xFFFFFFFF) | wavetable / granular source ASST row |
| 6 | programOffset | U32 | 任意 | 0 | within PTCH |
| 7 | programBytes | U32 | 任意 | 0 |  |
| 8 | channels | U32 | 任意 | 1 |  |

#### `MSEG`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | asset | U32 | 必須 | — | ASST row (interleaved stems recommended, 8.4) |
| 2 | tempoFirst | U32 | 必須 | — | MTMP rows |
| 3 | tempoCount | U32 | 必須 | — |  |
| 4 | bars | U32 | 必須 | — |  |
| 5 | entryFrame | U64 | 任意 | 0 |  |
| 6 | exitFrame | U64 | 任意 | 0 |  |
| 7 | markerFirst | U32 | 任意 | none (0xFFFFFFFF) |  |
| 8 | markerCount | U32 | 任意 | 0 |  |
| 9 | flags | U32 | 任意 | 0 | bit0: entry preload chunks present (8.3) |

#### `MPLS`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | entryFirst | U32 | 必須 | — |  |
| 2 | entryCount | U32 | 必須 | — |  |
| 3 | mode | U32 | 任意 | 0 | 0 sequential, 1 random, 2 shuffle |
| 4 | loopCount | U32 | 任意 | 0 | 0 = infinite |

#### `MPLE`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | segment | U32 | 必須 | — |  |
| 1 | weight | F32 | 任意 | 1 |  |
| 2 | repeat | U32 | 任意 | 1 |  |

#### `MSTM`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | segment | U32 | 必須 | — |  |
| 1 | channelFirst | U32 | 必須 | — |  |
| 2 | channelCount | U32 | 必須 | — |  |
| 3 | volumeCurve | U32 | 任意 | none (0xFFFFFFFF) | CURV row (param -> volume) |
| 4 | asset | U32 | 任意 | none (0xFFFFFFFF) | separate-file stem, synchronised (8.4) |

#### `MSTA`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | playlist | U32 | 必須 | — |  |
| 2 | musicEvent | U32 | 必須 | — | EVNT row of the owning music event |
| 3 | flags | U32 | 任意 | 0 |  |

#### `MTRN`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | musicEvent | U32 | 必須 | — | EVNT row |
| 1 | fromState | U32 | 必須 | — | state id, 0 = wildcard |
| 2 | toState | U32 | 必須 | — | state id, 0 = wildcard |
| 3 | timing | U32 | 必須 | — | snd_when_kind |
| 4 | timingCount | U32 | 任意 | 0 |  |
| 5 | fadeOutBeats | F32 | 任意 | 0 |  |
| 6 | fadeInBeats | F32 | 任意 | 0 |  |
| 7 | bridgeSegment | U32 | 任意 | none (0xFFFFFFFF) | MSEG row |
| 8 | startPosition | U32 | 任意 | 0 | 0 head, 1 same bar, 2 marker, 3 last position |
| 9 | startMarker | U32 | 任意 | 0 |  |
| 10 | interrupt | U32 | 任意 | 0 | 0 allowed, 1 after bridge completes |
| 11 | lateRule | U32 | 任意 | 0 | ReservationLateRule |
| 12 | flags | U32 | 任意 | 0 | bit0: prepare (pre-prime) target |

#### `MSTG`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | event | U32 | 必須 | — | EVNT row |
| 2 | timing | U32 | 必須 | — |  |
| 3 | timingCount | U32 | 任意 | 0 |  |
| 4 | overlap | U32 | 任意 | 0 | 0 replace, 1 stack |

#### `MMRK`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | frame | U64 | 必須 | — |  |
| 2 | kind | U32 | 任意 | 0 | 0 user, 1 entry, 2 exit, 3 pickup |

#### `MTMP`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | tick | I64 | 必須 | — | position of the change in ticks |
| 1 | bpm | F32 | 必須 | — |  |
| 2 | beatsPerBar | U32 | 任意 | 4 |  |
| 3 | beatUnit | U32 | 任意 | 4 |  |

#### `NAME`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | stringOffset | U32 | 必須 | — | within STRS |
| 2 | kind | U32 | 任意 | 0 | NameKind; aliases use kind Alias |

#### `GUID`

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | id | U32 | 必須 | — |  |
| 1 | guidLo | U64 | 必須 | — |  |
| 2 | guidHi | U64 | 必須 | — |  |
| 3 | kind | U32 | 任意 | 0 |  |

#### `ALIS`（2.0）

| id | 列 | 型 | 必須 | 既定値 | 備考 |
| --- | --- | --- | --- | --- | --- |
| 0 | oldId | U32 | 必須 | — | `snd_hash_id(旧名)` |
| 1 | id | U32 | 必須 | — | 現在のランタイム ID |
| 2 | kind | U32 | 任意 | 0 | NameKind（対象の種類） |

リネームの別名（10.1）。`NAME`/`STRS` と違って除去できないので、文字列表を外した配布ビルドでも旧 ID を実行時に解決できる。

- 行は `oldId` の昇順で、`oldId` は**狭義増加**（重複なし）。確保なしに O(n) で検証でき、引くときは二分探索。
- `oldId` ≠ 0、`id` ≠ 0、`oldId` ≠ `id`。
- `id` は常に**現在の** ID（別名の連鎖 A→B→C は書き手が A→C に畳む）。ランタイムの解決は 1 段だけ。
- 未知の `kind` の値は無視してよい（将来の追加のため）。
- 違反は `SND_ERR_CORRUPT`（§8 の 5）。

### 6.0 行の並びの規則（2.0）

参照をたどるだけで必ず終わる（循環しない）ことを、追加の作業領域なしに O(n) で確かめられるよう、次の並びを要求する。

- `BUSS`: 入力が先、出力が後。`parent` と、その行のセンドの `SEND.targetBus` は自分より**後ろ**の行。
- `EVNT`: Event の各トラックの入れ子クリップ（`CLIP.kind` = 1）の `target` は、その Event より**後ろ**の行（自己参照も不可）。
- `CATG`: `parent` は自分より**後ろ**の行。
- `SEEK`: 各アセットの範囲は `{0, 0}` で始まり、`frame` 狭義増加・`byteOffset` 非減少。

### 6.1 列挙値

| 列 | 値 |
| --- | --- |
| `EVNT.sequenceKind` | 0 Polyphonic, 1 Random, 2 RandomNoRepeat, 3 Sequential, 4 Shuffle, 5 Switch, 6 ComboSequential, 7 LayerCrossfade（4.2） |
| `EVNT.flags` | bit0 音楽 Event（トランスポートを所有）, bit1 3D, bit2 ポーズ無視, bit3 プライム要（プリロード無しのストリームを含む）, bit4 Stinger |
| `CLIP.kind` | 0 Asset（`ASST` 行）, 1 入れ子 Event（`EVNT` 行）, 2 合成（`SYNP` 行） |
| `ACTN.action` | 0 Event 再生, 1 Event 停止, 2 Parameter 設定, 3 スナップショット適用（4.2 アクショントラック） |
| `BUSS.flags` | bit0 マスター, bit1 出力バス（コントローラースピーカー・振動等）, bit2 負荷 LOD でバイパス可, bit3 メータ公開 |
| `CATG.flags` | bit0 ポーズ無視, bit1 フォーカス喪失でミュート |
| `CURV.input` | Parameter ID、または `0xFFFFFFFE` = 距離 |
| `CURV.target` | 0 音量, 1 ピッチ, 2 ローパス, 3 ハイパス, 4 パン, 5 センド, 6 エフェクト値, 7 スプレッド, 8 リバーブセンド, 9 3D ブレンド |
| `CURV.compose` | 0 dB 加算, 1 セント加算, 2 最小, 3 最大, 4 乗算（4.3） |
| `ASST.loadForm` | 0 PCM 常駐, 1 圧縮常駐, 2 展開キャッシュ, 3 ストリーム（6.2） |
| `ASST.codec` | `PCMF`, `PC16`, `ADPC`, `OPUS`、またはプラグイン fourcc |
| `ASST.flags` | bit0 ストールは音優先, bit1 仮想復帰は先頭から, bit2 仮想復帰しない, bit3 IO 優先度=音楽, bit4 IO 優先度=ボイス |
| `SYNP.kind` | 0 Oscillator, 1 Noise, 2 Granular, 3 Patch, 4 Wavetable |
| `SYNP.waveform` | 0 Sine, 1 Saw, 2 Square, 3 Triangle, 4 Pulse |
| `MTRN.timing` / `MSTG.timing` | `snd_when_kind` の値 |
| `MTRN.lateRule` | 0 次のグリッドへ送る, 1 遅れて開始, 2 取り消し（8.5「間に合わない時」） |
| `NAME.kind` | 0 Event, 1 Bus, 2 Param, 3 Category, 4 Snapshot, 5 State, 6 別名（リネーム前の名前, 10.1）, 7 その他 |

### 6.2 ブロブチャンク

- `WAVE`: 常駐波形とコーデック設定データ。各アセットの開始は 64 バイト境界。`ASST.dataOffset` 等は **WAVE チャンク先頭から**。
- `PREL`: ストリームと音楽の入口のプリロードチャンク（6.1, 8.3）。`ASST.preloadOffset` は PREL 先頭から。
- `SPTH`: ストリームファイルのパス（NUL 終端 UTF-8）。ランタイムで必要なので除去不可。`STRM.pathOffset` は SPTH 先頭から。
  ストリーム本体は別ファイルで、パケットは `STRM.sectorAlign` 境界から置く（10.2）。形式は下の「ストリームファイル」。
- `EFBL`: エフェクト用データ（IR 等）。`EFFX.blobOffset` は EFBL 先頭から。
- `PTCH`: パッチのプログラム（段階 6 で形式を定める。minor 版で追加）。
- `DSPN`: `DspPoolNeed { u64 instanceBytes; u32 instanceCount; u32 reserved; }`。`instanceBytes` は各 `EFFX.stateBytes` を
  64 に切り上げた和。DSP インスタンスプールの残りが足りなければ読み込み失敗（3.3）。
- `PHYS`: 物理マテリアル表（9 章）。アプリケーションチャンク。
  `PhysChunkHeader { u32 tableCount; u32 reserved; u64 tablesOffset; }` の後に `PhysTable[tableCount]`（64 B）:
  `u32 id, u32 materialCount(N), u64 singleOffset（u32 EventID[N][4 接触種別]）, u64 pairOffset（u32 EventID[N*N]、0 = 上書きなし、
  添字 min*N+max）, u64 footstepOffset（u32 EventID[N]）, u32 defaultEvent[4], u64 curveOffset（PhysCurves[N]、32 B）, u64 reserved`。
  オフセットはすべて PHYS チャンク先頭から。
- `STRS`: 文字列プール（NUL 終端 UTF-8）。リリースで除去可。`NAME.stringOffset` は STRS 先頭から。

### 6.3 ストリームファイル（`'SNDS'`、`bank_format::StreamFileHeader`）

| offset | 型 | 名前 | 内容 |
| --- | --- | --- | --- |
| 0 | u32 | magic | `'SNDS'`（`kStreamMagic`） |
| 4 | u32 | version | 1（`kStreamVersion`） |
| 8 | u32 | assetId | `ASST.id` |
| 12 | u32 | codec | fourcc（`ASST.codec`） |
| 16 | u32 | channels | |
| 20 | u32 | sampleRate | |
| 24 | u64 | totalFrames | |
| 32 | u64 | dataOffset | 最初のパケットの位置。`sectorAlign` の倍数で `STRM.dataOffset` と同じ |
| 40 | u64 | dataBytes | 符号化データのバイト数（埋めを含まない） |
| 48 | u32 | sectorAlign | `STRM.sectorAlign` |
| 52 | u32 | reserved0 | 0 |
| 56 | u64 | reserved1 | 0 |

ヘッダの後は `dataOffset` まで 0 で埋め、データの後もファイル長が `sectorAlign` の倍数になるまで 0 で埋める
（`STRM.fileBytes` は埋めた後の長さ）。`SEEK.byteOffset` と `PREL` のプリロードはデータ先頭（`dataOffset`）から数える。

ランタイムの検査（段階2、source）: ストリームを鳴らす前にヘッダ（64 バイト）を読み、次のどれかが成り立たなければ IO エラーとして
そのボイスを終える（データは鳴らさない）。

- 64 バイト読めること、`magic` = `'SNDS'`、`version` = 1
- `codec`・`channels`・`sampleRate`・`totalFrames`・`assetId` がバンクの `ASST` 行と同じ
- `dataOffset` が `STRM.dataOffset` と同じ
- `dataBytes` がコーデックの期待する大きさ: PCM は `totalFrames × フレームの大きさ`、ADPCM は
  `ceil((preSkip + totalFrames) / framesPerPacket) × blockAlign` に等しい。Opus は同じパケット数 P に対して
  `3P ≤ dataBytes ≤ P × (2 + ストリーム数 × 1277)`。それ以外のコーデックは 1 以上

データの途中（`dataOffset + dataBytes` より前）で読み出しが短く終わったとき（ファイルが切り詰められている）は、
データの終端ではなく IO エラーとして扱う。

### 6.4 標準エフェクトの種別 ID（契約 1.1 で追記）

`EFFX.effectType` は種別名の `snd_hash_id`（D-04）。標準エフェクトの種別は dsp が定める次の 14 種
（`dsp/include/snd/dsp/effects.hpp` の `kEffect*` と一致する。一致は統合テストが確認する）。
これ以外の ID はプラグイン（`Config.plugins.effects`、D-31）として扱う。
各パラメータの番号・単位・範囲・既定値（`EFPV` の並び）は `effects.hpp` の冒頭の表が正。
範囲外の値はランタイムが範囲に切り詰める（NaN は既定値）。ビルドツールは範囲外の既定値を警告、パラメータの数の超過をエラーにする。

| 種別名 | ID | dsp の定数 | パラメータ数 | 内容 |
| --- | --- | --- | --- | --- |
| `snd.fx.reverb` | `0x542529D2` | `kEffectReverb` | 14 | アルゴリズムリバーブ（8 本の FDN、I3DL2 / Unity SFX Reverb の項目） |
| `snd.fx.echo` | `0x3D8A2D0F` | `kEffectEcho` | 5 | エコー（Unity Echo） |
| `snd.fx.delay` | `0x46D3ABA1` | `kEffectDelay` | 7 | 減衰・ピンポン付きのフィードバックディレイ |
| `snd.fx.chorus` | `0x1E3C9EA8` | `kEffectChorus` | 6 | コーラス（Unity Chorus） |
| `snd.fx.flanger` | `0x0C99B3D3` | `kEffectFlanger` | 6 | フランジャー（Unity Flange） |
| `snd.fx.distortion` | `0x1C3DACE1` | `kEffectDistortion` | 4 | ソフトクリップのディストーション |
| `snd.fx.compressor` | `0xE20BFE6B` | `kEffectCompressor` | 8 | コンプレッサー（サイドチェーン入力あり、4.6 のダッキング） |
| `snd.fx.limiter` | `0x9D417A6E` | `kEffectLimiter` | 4 | 先読みのリミッター |
| `snd.fx.parameq` | `0x1F6C15FD` | `kEffectParamEq` | 16 | 4 バンドのパラメトリック EQ |
| `snd.fx.lowpass` | `0x596CFAAD` | `kEffectLowpass` | 2 | 12 dB/oct のローパス |
| `snd.fx.highpass` | `0x7E5CD7BF` | `kEffectHighpass` | 2 | 12 dB/oct のハイパス |
| `snd.fx.pitchshift` | `0x6AE27268` | `kEffectPitchShifter` | 3 | 2 タップのピッチシフター |
| `snd.fx.normalize` | `0xFBDFA555` | `kEffectNormalize` | 3 | ノーマライズ（Unity Normalize） |
| `snd.fx.surround` | `0x2072E34C` | `kEffectSurround` | 5 | 疑似サラウンド・ステレオ拡幅 |
| `snd.fx.convolution` | `0x8837ECB0` | `kEffectConvolution` | 7 | 一様分割のコンボリューションリバーブ（IR は準備済み IR の登録表を `IrSlot` で引く、D-212〜D-215。段階 7-D で追加。上の本文の「14 種」は統合のときに 15 種に直す） |

### 6.5 組み込みコーデックのバイト配置（契約 1.1 で追記）

`ASST.codec` が組み込みコーデックのとき、`WAVE`（常駐）・ストリームファイルのデータ（6.3）は次の配置。
`ASST` の `codec, channels, sampleRate, totalFrames, preSkip, preRoll, framesPerPacket, blockAlign` は `snd_codec_header` の同名フィールドで、
`extra` / `extra_bytes` は `ASST.extraOffset` / `extraBytes` が指す WAVE の範囲。PCM・ADPCM は `extra` を使わない（`extraBytes = 0`）。
Opus は使う（下の「Opus」）。codec モジュールの実装（`codec/README.md`）と一致する。

| fourcc | 1 パケット | 内容 |
| --- | --- | --- |
| `PCMF` | 1 フレーム（`4 × ch` バイト） | float32 リトルエンディアン、インターリーブ。有限値はそのまま（±1 を超えても切らない）。デコーダは NaN / ±Inf を 0 にする |
| `PC16` | 1 フレーム（`2 × ch` バイト） | int16 リトルエンディアン、インターリーブ。`x / 32768` で展開。エンコーダは最近接丸めで `[-32768, 32767]` に切る |
| `ADPC` | 1 ブロック（`blockAlign` バイト） | WAV の IMA ADPCM と同じ配置。チャンネルごとに 4 バイトのヘッダ `{s16 予測値, u8 ステップ添字 0..88, u8 0}`、続いて 4 バイト × ch の組を繰り返す（各組がそのチャンネルの 8 サンプル、下位ニブルが先）。1 ブロックのフレーム数は `1 + (blockAlign − 4ch) / (4ch) × 8`。最後のブロックは詰め物で満たし、`totalFrames` で切る |

- PCM: `framesPerPacket` は 1（0 は指定なし）、`blockAlign` はフレームの大きさ（0 は指定なし）。
- ADPCM: `blockAlign` は `4ch × (1 + k)`（k ≥ 1）。**1 チャンネルあたり 1024 バイト以下**（`blockAlign ≤ 1024 × ch`）。
  これを超えるとデコーダの状態が 32 KiB（段階1までの既定の `decoderSlotBytes`。今の既定は 64 KiB、D-58）を 8ch で超える。
  既定は 512 バイト／ch（1017 フレーム、48 kHz で 21 ms）。`framesPerPacket` は 0 か計算値。ブロックは独立なので `preRoll` は 0。
- `SEEK`: `frame` は再生フレーム（プリスキップ後）、`byteOffset` はデータ先頭から。先頭は `{0, 0}`。PCM はフレーム、ADPCM はブロックの境界。
- プリスキップ（`preSkip`）の後の最初のフレームが再生フレーム 0。PCM・ADPCM のエンコーダは `preSkip = 0` を書く。

#### Opus（`OPUS`、CONTRACTS.md D-56〜D-60 で追記）

ランタイムが `SND_CODEC_OPUS=ON`（既定）でビルドされていれば組み込み（同梱の libopus 1.5.2）。OFF のランタイムでは
`OPUS` のアセットはプラグインが無い限り鳴らない（バンクの読み込みは失敗しない。再生要求がコーデック無しで終わる）。

| 列 | 値 |
| --- | --- |
| `sampleRate` | **48000 だけ**（他の値は無効なヘッダ）。ビルドツールは他のレートの素材を 48 kHz に変換して符号化する |
| `channels` | 1〜8。1〜2 は 1 ストリーム（RFC 7845 のマッピングファミリ 0）、3〜8 はサラウンドのマルチストリーム（ファミリ 1） |
| `framesPerPacket` | 960（1 パケット = 20 ms）。0 も 960 とみなす。それ以外は無効 |
| `blockAlign` | 0（可変長）。0 以外は無効 |
| `preSkip` | エンコーダの先読み（libopus 1.5.2 は 312）。再生フレーム 0 はデコード位置 `preSkip` |
| `preRoll` | 3840（80 ms、RFC 7845 §4.6）。シークはシーク表で `target − preRoll` 以前の点から始める（D-23） |
| `totalFrames` | 元素材のフレーム数を 48 kHz に換算したもの: 元が 48 kHz ならそのまま、それ以外は `ceil(元のフレーム数 × 48000 / 元のレート)`（例: 44.1 kHz の 44100 フレーム → 48000、1 フレーム → 2）。ビルドツールのレート変換は全コーデック共通で同じ式。符号化は `ceil((totalFrames + preSkip) / 960)` パケットで、末尾は無音で埋める |
| `extraOffset` / `extraBytes` | **必須**。下の setup。`extraBytes` は**ちょうど** `4 + channels`（短くても長くても無効なヘッダ）。WAVE の中、64 バイト境界 |

データ（`WAVE` の常駐データ・ストリームファイルのデータ）はパケットを順に並べたもの:

```
packet := u16 length (LE, 1 <= length <= streams × 1277) , u8 payload[length]
```

- `payload` は libopus の `opus_multistream_encode` の出力（1 ストリームなら通常の Opus パケット、複数なら先頭 streams−1 個が
  自己区切り形式。RFC 6716 Appendix B / RFC 7845 §5.1.1）。各パケットはちょうど 960 フレームにデコードされる。
  長さ 0、上限超え、960 フレームでないパケットは壊れたデータ（`SND_DECODE_ERROR`）。
- 1277 = 20 ms・1 フレームの最大 1275 バイト + 自己区切りの長さ 2 バイト。

setup（`extra`）:

| offset | 型 | 内容 |
| --- | --- | --- |
| 0 | u8 | 版 = 1（それ以外は無効） |
| 1 | u8 | チャンネル数（`ASST.channels` と一致） |
| 2 | u8 | ストリーム数 N（1〜8） |
| 3 | u8 | 結合（ステレオ）ストリーム数 M（0〜N） |
| 4 | u8[channels] | 出力チャンネル i ← デコードされたストリームのチャンネル `mapping[i]`（< N + M、255 = 無音）。RFC 7845 §5.1.1 のチャンネルマッピング表と同じ意味 |

- **デコーダの検証規則**は上の表のとおりで、ストリーム数は問わない: 1〜2ch でも N ≠ 1 の setup（例: 2ch を 2 本のモノラルストリームで）は
  RFC 7845 として有効なので受け付ける。「1〜2ch は 1 ストリーム、3〜8ch はファミリ 1」はビルドツールの出力の説明。
- **チャンネル順**: 出力（= `mapping` の並び）は WAV の順（RFC 7845 のファミリ 1 と、WAVE_FORMAT_EXTENSIBLE の一般的なチャンネルマスクに合わせる）。
  ビルドツールはサラウンドの符号化のために Vorbis の順（RFC 7845 §5.1.1.2）へ並べ替えて符号化し、その逆を `mapping` に書く。
  ランタイムは `mapping` に従うだけで並べ替えない。ビルドツールが入力（WAV）のチャンネルとみなす並び:

  | ch | WAV の順（= 出力の順） | Vorbis の順（符号化の内部） |
  | --- | --- | --- |
  | 1 | M | M |
  | 2 | L R | L R |
  | 3 | L R C | L C R |
  | 4 | L R BL BR | L R BL BR |
  | 5 | L R C BL BR | L C R BL BR |
  | 6 | L R C LFE BL BR | L C R BL BR LFE |
  | 7 | L R C LFE BC SL SR | L C R SL SR BC LFE |
  | 8 | L R C LFE BL BR SL SR | L C R SL SR BL BR LFE |
- `SEEK`: `frame` は再生フレーム、`byteOffset` はデータ先頭からの、そのパケットの**長さの前置**の位置。先頭は `{0, 0}`。
  パケット k（デコード位置 960k）は `960k > preSkip` かつ `960k − preSkip < totalFrames` のとき候補になり、`frame = 960k − preSkip`。
  よって `frame + preSkip` は 960 の倍数。`Seek(f > 0)` で `f + preSkip` が 960 の倍数でなければ（パケット境界でなければ）次の `Decode` が失敗する
  （`SND_DECODE_ERROR`、`Seek` / `Init` まで継続。ADPCM の規則と同じ）。
- 終端: `totalFrames` を出し終えた後の入力（残りのパケット、ストリームファイルの最後のセクタの 0 の埋め、その他のバイト）は解釈せずに消費して捨てる。
- 保持の上限（D-51）の「1 パケット」は、最大のパケット = `ストリーム数 × 1277 + 2`（長さの前置込み）。未デコードの入力は `16 KiB + ストリーム数 × 1277 + 2` まで。
- プリロード（`PREL`）はシーク点で切った先頭の部分（ADPCM 以外の可変長コーデックと同じ扱い）。
- ビルドツールは既定で「パケット間の予測なし」（libopus `OPUS_SET_PREDICTION_DISABLED`）で符号化する。CELT のパケット
  （既定のビットレート）では、シーク点から 2 パケット後以降の出力が通しのデコードとビット単位で一致するので、`preRoll` = 80 ms で
  シーク・ループの再開がサンプル単位で正確になる。SILK / ハイブリッド（低ビットレートの音声）は 80 ms 後も近似（12〜32 kbit/s の音声で最大誤差 1.5e-2〜3.4e-2 を観測）。

## 7. 版と互換性

| 変更 | 扱い |
| --- | --- |
| 新しいチャンク種別、既存テーブルへの列追加、列挙値の末尾追加、予約領域の意味付け（0 の意味は不変） | **minor** を上げる。古いランタイムは未知のチャンク・列を無視し、新しいランタイムは無い任意列を既定値で埋める |
| 列の型・意味・必須性の変更、列 ID の付け替え、ヘッダ・ChunkEntry・テーブル形式の変更 | **major** を上げる。読み込みは `SND_ERR_VERSION_MISMATCH` |
| 1 チャンクの内部レイアウト変更（ブロブ） | そのチャンクの `ChunkEntry.version` を上げる。ランタイムは知らない版のチャンクを持つバンクを拒否する（必須チャンク）か無視する（任意チャンク） |

- 既存列を必須から任意へ変えるのは minor、任意から必須は major。
- バンク形式の版は契約の版（`SND_CONTRACT_VERSION`）とは独立に持つ。どちらの変更も `CONTRACTS.md` の手順に従う。

| 版 | 内容 |
| --- | --- |
| 1.0 | 段階0で凍結 |
| 2.0 | headerChecksum をヘッダ全体（自身の欄は 0 とみなす）に拡大、予約バイト 0 の検証、`ALIS` チャンク追加、行の並びの規則（6.0: 入れ子 Event・カテゴリの親・シーク表）、`loopEnd`・`SEEK.byteOffset`・ストリームファイル（6.3）・エンディアン・`fileBytes` の明文化 |
| 2.0（契約 1.1） | 文書の追記のみ（版は 2.0 のまま）: 6.4 標準エフェクトの種別 ID、6.5 組み込みコーデックのバイト配置 |
| 2.0（Opus、D-57） | 文書の追記のみ（版は 2.0 のまま）: 6.5 に Opus のバイト配置と setup（`ASST.extra*`）。チャンク・列・列挙値は変えない（`OPUS` の fourcc は 1.0 から列挙にある）。setup に自前の版（1）を持つ |

## 8. 読み込み時の検証（bank モジュールの完了条件）

1. ファイル長 ≥ 128、`headerChecksum`（ヘッダ全体）、`magic`、`formatMajor`、`endianness`、`alignLog2`、`headerBytes`、`platform`、
   予約バイト（`reserved0`、`reserved`）が 0、`languageId` と言語フラグの整合。
2. `fileBytes` が実際の長さと一致（パディング不可）、`checksum`。
3. 依存表と目次が範囲内、各チャンクが範囲内かつ 64 の倍数、チャンク同士が重ならない。
4. テーブルチャンク: 列ディレクトリが範囲内、`elemBytes` が型と一致、各列配列が範囲内かつ 64 の倍数、必須列がある。
5. 行参照（`kNone` 以外）が対象テーブルの行数未満、ブロブ内オフセット＋長さがチャンク内。
   6.0 の並びの規則（バス・入れ子 Event・カテゴリの親が後ろの行、シーク表の単調性）と `ALIS` の制約（昇順・0 でない・自己参照でない）。
6. 失敗はすべて `SND_ERR_CORRUPT`（版・機種は `SND_ERR_VERSION_MISMATCH`）。壊れた入力で落ちないことをファズで確認する（14 章）。

## 9. 仮の任意チャンク（未凍結。段階5 ツール、セッション C）

形式の版は 2.0 のまま。ここのチャンクは凍結対象ではなく、ランタイムは読まない（未知のチャンクは無視、§5・§7、D-32）。
正式な定義が決まったら差し替える。

### `XPCH`（波形生成パッチのプログラム、仮）

`PTCH`（§6.2、段階 6 で形式を定める）の形式がまだ無いため、ビルドツールはパッチ（project-format.md の `patch`）を
この任意チャンクに出す。ランタイムの Patch ソースとバンクでの表現はセッション D が決める
（`spec/handoff/parallel/requests/C-10.md`）。そのときに `PTCH` へ移し、このチャンクはやめる。

- fourcc `'XPCH'`、ブロブ。ChunkEntry.flags 0、version 1。整数はリトルエンディアン、浮動小数は binary32。
- `SYNP` の行は `kind` = Patch（3）、`programOffset` = `programBytes` = 0（`PTCH` は書かない）。どの `SYNP` 行がどのプログラムかは
  このチャンクの対応表で引く。

| 部分 | 大きさ | 内容 |
| --- | --- | --- |
| ヘッダ | 16 B | `u32 version (1), u32 programCount, u32 linkCount, u32 nodeBytes (128)` |
| プログラム | 16 B × programCount | `u32 patchId (snd_hash_id(name)), u32 nodeFirst, u32 nodeCount, u32 reserved` |
| ノード | 128 B × 全ノード | `u32 kind, u32 parameterId, u32 pointCount, u32 reserved, u32 inputs[4], f32 params[8], f32 points[8][2]` |
| 対応表 | 8 B × linkCount | `u32 synpRow, u32 program` |

- ノードは依存順（入力が先、出力ノードが最後。同順位はプロジェクトのファイルでの順）。`inputs[i]` は同じプログラムの中の
  ノード番号（0 から）、無ければ 0xFFFFFFFF。`kind` と `inputs` / `params` の並びはビルドツールのノードの表
  （`buildtool/include/snd/buildtool/patch.hpp` の `NodeKinds`、追加は末尾）。列挙のパラメータは選択肢の番号を浮動小数で持つ。
- `parameterCurve` ノードは `parameterId`（パラメータの ID）と `points`（x 昇順、`pointCount` 個、最大 8）を持つ。
- 出力ノードに届かないノードは入れない（ビルドの警告）。
