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

## 8. 読み込み時の検証（bank モジュールの完了条件）

1. ファイル長 ≥ 128、`headerChecksum`（ヘッダ全体）、`magic`、`formatMajor`、`endianness`、`alignLog2`、`headerBytes`、`platform`、
   予約バイト（`reserved0`、`reserved`）が 0、`languageId` と言語フラグの整合。
2. `fileBytes` が実際の長さと一致（パディング不可）、`checksum`。
3. 依存表と目次が範囲内、各チャンクが範囲内かつ 64 の倍数、チャンク同士が重ならない。
4. テーブルチャンク: 列ディレクトリが範囲内、`elemBytes` が型と一致、各列配列が範囲内かつ 64 の倍数、必須列がある。
5. 行参照（`kNone` 以外）が対象テーブルの行数未満、ブロブ内オフセット＋長さがチャンク内。
   6.0 の並びの規則（バス・入れ子 Event・カテゴリの親が後ろの行、シーク表の単調性）。
6. 失敗はすべて `SND_ERR_CORRUPT`（版・機種は `SND_ERR_VERSION_MISMATCH`）。壊れた入力で落ちないことをファズで確認する（14 章）。
