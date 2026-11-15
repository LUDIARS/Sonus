# プロジェクト形式（版 1）

設計書 10.1 の「ツール側のテキストのプロジェクト」。`sonus-build`（buildtool）が読み、オーサリングツール（GUI、段階後半）が
読み書きする。バンク形式（`bank-format.md`）の列と 1 対 1 に写せることを優先し、差分を取りやすく安定した形にする。
本書は段階1で buildtool 担当が定めたもので、**凍結対象ではない**（版を上げずに追加はできる。互換を壊す変更は
`formatVersion` を上げる）。実装は `buildtool/src/project.cpp`（読み込み・参照）と `buildtool/src/build.cpp`（変換）。

## 1. ファイルと共通の項目

- プロジェクトはディレクトリ。その下の `*.json`（再帰、名前順。`.` で始まるディレクトリと出力・キャッシュ先は除く）が
  1 ファイル 1 オブジェクト。波形などの素材は同じディレクトリ以下に置き、プロジェクトからの相対パス（`/` 区切り）で指す。
- 文字コードは UTF-8（不正なバイト列はエラー、先頭の BOM は読み飛ばす）、JSON は RFC 8259（同じメンバ名の重複はエラー）。
  診断の桁はバイト単位。
- 未知のメンバは**どの階層でも**警告（綴り間違いが既定値に化けるのを防ぐ。`convert` / `platforms` の機種キーも検査）。
  `--werror` で警告をエラーにできる。ツールは自分が知らないメンバを保存時に残すこと。

| メンバ | 型 | 必須 | 内容 |
| --- | --- | --- | --- |
| `type` | string | 必須 | 種類（2 節） |
| `guid` | string | 必須 | `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`（16 進。`{}` 付きも可）。オブジェクトの不変の識別子。プロジェクト内で一意 |
| `name` | string | 必須（`ducking` は任意）。制御文字（U+0000〜U+001F、U+007F）は不可（`aliases` も） | ランタイム ID の元（`snd_hash_id(name)`、FNV-1a 32、CONTRACTS D-04）。同じ種類で一意 |
| `aliases` | string[] | 任意 | リネーム前の名前（10.1）。ID 表・ゲーム用ヘッダ・`NAME` 表（kind = Alias）に残す |
| `bank` | 参照 | 任意 | 所属バンク。無ければ共有オブジェクト（参照されたバンクに複製される） |
| `notes` | any | 任意 | ツール用のメモ（ビルドは無視）。**どの階層のオブジェクトにも**置け、警告も出ない（材質名などを自由なキーにとる表の中でも `notes` はキーとして扱わない） |

**参照**: 文字列で、GUID（推奨。ツールは GUID を書く）または参照先の種類での `name`。解決できなければ
`ファイル:行:列: unknown <種類> '<文字列>'` のエラー。

**GUID のバンクでの表現**: `GUID` チャンクの `guidHi` = GUID 文字列の先頭 16 桁、`guidLo` = 後半 16 桁。

## 2. 種類

既定値はバンク形式の列の既定値と同じ。数値の単位は bank-format.md §1（ms、dB、セント、フレーム、tick = 1 拍 960）。

### project（0 か 1 個）

| メンバ | 内容 |
| --- | --- |
| `formatVersion` | 1 |
| `languages` | 言語名の配列（例 `["ja", "en"]`）。言語バンクはこの言語ごとに作る。`.` `/` `\` を含まない |
| `platforms` | `{ "default": {...}, "LNX_": {...}, ... }`。波形変換の既定（`codec`, `sampleRate`, `channels`）とストリームの `sectorAlign`（既定 2048） |

### bank

| メンバ | 内容 |
| --- | --- |
| `dependsOn` | バンク参照の配列。ヘッダの `Dependency`（bankId, 依存先の contentHash）になる。循環はエラー。言語バンクへの依存は不可 |
| `language` | true なら言語バンク（10.4）。`languages` の言語ごとに `<name>.<language>.sndb` を出力。bankId は共通、languageId = `snd_hash_id(language)` |

### bus → `BUSS`, `SEND`, `EFFX`, `EFPV`, `EFBL`

`parent`（参照、マスターは無し）, `master`, `output`, `lodBypassable`, `meter`（bool → `BUSS.flags`）, `volumeDb`, `channels`（0 = 親と同じ）,
`lodPriority`, `sends: [{bus, levelDb, preFader}]`, `effects: [{type, params: [数値...], blob: "パス", flags}]`。
`effects[].type` は種別名（`EFFX.effectType = snd_hash_id(type)`。標準エフェクトの名前は dsp が定める）。
`blob` のファイルはそのまま `EFBL` に入る。

### category → `CATG`

`parent`, `ignorePause`, `muteOnFocusLoss`, `volumeDb`。`CATG` の行は子が先・親が後（バンク形式 2.0 §6.0）。親の循環はエラー。

### parameter → `PARM`

`kind`（`continuous` / `switch`）, `scope`（`["global", "emitter", "playback"]` の部分集合、既定は全部）, `min`, `max`, `default`（既定 min）, `rampRate`。

### snapshot → `SNAP`, `SNPV`

`values: [{kind, bus, value, effect, param, send}]`。`kind` は `busVolume`（bus の音量）, `effectParam`（bus の `effect` 番目の
エフェクトの `param` 番目）, `sendLevel`（bus の `send` 番目のセンド）。

### voiceLimitGroup → `VLGR`

`maxVoices`（必須）, `steal`（`oldest` / `quietest` / `lowestPriority` / `never`）。

### ducking → `DUCK`（`bank` 必須の扱い：所属バンクにだけ出る）

`kind`（`sidechain`: `source` はバス / `rule`: `source` はカテゴリ）, `source`, `target`（バス）, `amountDb`（必須）, `attackMs`, `releaseMs`, `thresholdDb`。

### asset → `ASST`, `WAVE`, `PREL`, `SEEK`, `STRM`, `SPTH`

| メンバ | 内容 |
| --- | --- |
| `file` | WAV のパス（PCM 16/24/32bit、IEEE float 32/64bit、WAVE_FORMAT_EXTENSIBLE） |
| `languages` | `{ "ja": "パス", ... }`。言語バンクでだけ使える（各言語のバンクにその言語の素材が入る） |
| `loadForm` | `pcm`（既定）/ `compressed` / `decompressCache` / `stream`（6.2） |
| `loop` | `{start, end}`（元素材のフレーム、end は含まない）。無ければ WAV の `smpl` チャンク |
| `convert` | `{ "default": {codec, sampleRate, channels}, "<機種>": {...} }`。優先順は project.platforms.default < project.platforms[機種] < convert.default < convert[機種]。`codec` 既定 `PC16`、`sampleRate`/`channels` 0 = 元のまま |
| `stallPolicy` | `time`（既定）/ `sound` → `kAssetStallSoundPriority` |
| `virtualReturn` | `continue` / `restart` / `never` → `ASST.flags` |
| `ioPriority` | `normal` / `music` / `voice` → `ASST.flags` |
| `preloadMs` | ストリームのプリロード長（既定 100、0 = 無し → その素材を鳴らす Event に「プライム要」） |
| `seekIntervalMs` | シーク表の間隔（既定: pcm 以外 100、pcm は表なし） |

### event → `EVNT`, `TRAK`, `CLIP`, `ACTN`, `CURV`, `CPNT`, `SYNP`（音楽は 2.x）

| メンバ | 内容 |
| --- | --- |
| `bus` | 参照。無ければ唯一の `master` バス |
| `category`, `limitGroup` | 参照 |
| `priority` | 0..255（既定 128） |
| `sequence` | `polyphonic` / `random` / `randomNoRepeat` / `sequential` / `shuffle` / `switch` / `comboSequential` / `layerCrossfade` |
| `is3D`, `ignorePause`, `stinger` | bool → `EVNT.flags` |
| `maxInstances`, `retriggerMs`, `fadeOutMs`, `comboWindowMs`, `maxDistance` | 数値 |
| `cone` | `{innerDeg, outerDeg, outerDb}` |
| `virtualReturn` | `continue` / `restart` / `never` |
| `distanceCurves` | `{volume|lowpass|spread|reverbSend|blend3D: {points}}`（入力は距離） |
| `curves` | `[{parameter, target, compose, points}]`。`parameter` は参照か `"distance"`、`target` は `volume` `pitch` `lowpass` `highpass` `pan` `send` `effectParam` `spread` `reverbSend` `blend3D`、`compose` の既定は 4.3（音量 dB 加算、ピッチ セント加算、ローパス最小、ハイパス最大、他は乗算）。`points: [[x, y], [x, y, shape]]`（x 昇順、shape 0 = 線形） |
| `tracks` | `[{bus, layerParameter, flags, clips: [...], actions: [...]}]` |
| `music` | 音楽 Event（2.x） |

**clip**: `asset` / `event`（入れ子。同じバンクの Event であること。循環はビルドエラー）/ `synth` のどれか 1 つと、
`startMs`, `weight`, `volumeDb`, `pitchCents`, `random: {volumeDb, pitchCents, pan, startMs, filterHz}`, `switchValue`（名前 → `snd_hash_id`、または整数）,
`layer: [min, max]`, `fadeInMs`, `fadeOutMs`, `loopCount`（0 = 無限）。
`synth`: `{kind: oscillator|noise|granular|patch|wavetable, waveform: sine|saw|square|triangle|pulse, frequencyHz, duty, noiseColor: white|pink|brown, asset, channels}`
（`patch` は段階 6 まで不可）。

**action**: `{timeMs, action: play|stop|setParameter|applySnapshot, target（Event / parameter / snapshot の参照）, value}`。

### 2.x 音楽（event の `music`、segment、playlist）

event の `music`:

```json
"music": {
  "initialState": "Explore",
  "states": [{"name": "Explore", "playlist": "PL/Explore"}],
  "transitions": [
    {"from": "Explore", "to": "Battle", "timing": "nextBeat", "timingCount": 0,
     "fadeOutBeats": 0.5, "fadeInBeats": 0, "bridge": "Seg/Intro", "start": "head",
     "startMarker": "M1", "interrupt": "allowed", "late": "nextGrid", "prepare": false},
    {"from": "*", "to": "*", "timing": "nextBar"}
  ],
  "stingers": [{"name": "Sting/Hit", "event": "SE/Sting", "timing": "nextBeat", "overlap": "replace"}]
}
```

- State は `MSTA`（id = `snd_hash_id(name)`）、遷移ルール表は `MTRN` に**書いた順**で入る（8.5「上から順に照合」）。
  前の行に覆われて一致しえない行は警告。`*` はワイルドカード（列の値 0）。
- `timing`: `immediate` / `nextBeat` / `nextBar` / `barsAfter` / `grid` / `nextMarker` / `segmentEnd`（`snd_when_kind`）。
  `start`: `head` / `sameBar` / `marker` / `lastPosition`。`interrupt`: `allowed` / `afterBridge`。`late`: `nextGrid` / `startLate` / `cancel`。
- スティンガーの Event は同じバンクにあること。`stingers[].timingCount`（`MSTG.timingCount`、`barsAfter` の N や `grid` の分割数）も書ける。
- `MSTA.flags` はバンク形式でビットの意味が未定義のため、プロジェクト形式には項目を設けず常に 0 を書く（意味が決まった時点で項目を足す）。

**segment** → `MSEG`, `MTMP`, `MMRK`, `MSTM`: `asset`（必須）, `bars`（必須）, `tempo: [{tick, bpm, beatsPerBar, beatUnit}]`（既定 120 BPM 4/4）,
`entryFrame`, `exitFrame`, `markers: [{name, frame, kind: user|entry|exit|pickup}]`, `stems: [{channelFirst, channelCount, volumeCurve, asset}]`,
`entryPreload`（bool）。フレーム位置は元素材のフレームで書き、サンプルレート変換に合わせて換算される。

**playlist** → `MPLS`, `MPLE`: `mode`（`sequential` / `random` / `shuffle`）, `loopCount`（0 = 無限）, `entries: [{segment, weight, repeat}]`。

音楽オブジェクト（segment / playlist）は、それを使う音楽 Event のバンクに入る。

### materialTable → `PHYS`（9.1, 9.3）

```json
{"type": "materialTable", "name": "Phys/Default", "bank": "Level1",
 "materials": ["metal", "wood"],
 "single": {"metal": {"impact": "SE/Metal", "slide": "...", "roll": "...", "break": "..."}},
 "pairs": [{"a": "metal", "b": "glass", "event": "SE/MetalGlass"}],
 "footsteps": {"wood": "SE/StepWood"},
 "defaults": {"impact": "SE/Generic"},
 "curves": {"metal": {"impulseToVelocity": [4 数値], "massToPitchCents": [2], "speedToVolumeDb": [2]}}}
```

材質番号は `materials` の順。組の上書きは `table[min(a,b) * N + max(a,b)]`。Event は ID（`snd_hash_id`）で入る。

## 3. バンクへの割り当て

- `bank` を持つ event / snapshot / ducking / materialTable はそのバンクに入る。
- バス・カテゴリ・ボイスリミットグループ・パラメータ・アセット・音楽オブジェクトは、そのバンクの Event 等が参照するものが
  **そのバンクに複製**される（形式 1.0 の行参照は同じバンク内に限るため）。バスは親とセンド先まで閉包をとる。
  同じ ID の行が複数バンクに現れうるので、ランタイム（core）は ID で同一視する。
- `bank` を持つオブジェクトを別のバンクから使うには、使う側がそのバンクに（推移的に）依存していること。違反はエラー。
- 入れ子の Event とスティンガーの Event は同じバンクに置く。`EVNT` の行は入れ子にする側が先・される側が後
  （バンク形式 2.0 §6.0。同順位は名前順）。
- アクションの対象（Event / スナップショット / パラメータ）と材質表の Event のように**ランタイム ID で参照するもの**も同じ規則に従う
  （別バンクの Event なら、そのバンクへの依存が必要）。
- `bank` の無い ducking はどのバンクにも入らない（警告）。

## 4. バスの並び（graph order）

`BUSS` は「入力が先、出力が後」の依存順（子とセンド元が先、マスターが最後）。同順位は名前順。
バスの木の**ルート（`parent` の無いバス）はちょうど 1 つ**で、それがマスター（`kBusMaster`）。2 つ目のルートはエラー。
`bus` を書かない Event はマスターに出る。
`BUSS.parent` と `SEND.targetBus` は必ず自分より後ろの行を指す（bank の検証もこれを確かめる）。循環はビルドエラー。

## 5. ID（10.1）

- ランタイム ID = `snd_hash_id(name)`。衝突検出は**プロジェクト全体**: 名前・別名・音楽 State / スティンガー / マーカー名・
  `switchValue` の名前・エフェクト種別名・言語名のうち、**異なる文字列が同じ ID** になったらビルドエラー
  （`ファイル:行:列: error: id collision: 'A' and 'B' (...) both hash to 0x...`）。同じ文字列を別の種類で使うのは可。
- 別名は `ALIS` チャンク（旧 ID → 現在の ID、除去不可。バンク形式 2.0）と `NAME`（kind = Alias、除去可）の両方に入る。
- `sonus_ids.h` の識別子: 固定の接頭辞 `id_` ＋ 名前（英数字と `_` 以外は `_`、`_` の連続は 1 つ、名前先頭の `_` は落とす）。
  例 `SE/Explosion` → `id_SE_Explosion`、`SIZE_MAX` → `id_SIZE_MAX`、`new` → `id_new`。接頭辞があるので予約語・数字始まり・
  `_` 始まりの予約名・マクロ（標準と Sonus のマクロは全部大文字、`SND_` 始まり、`_` 始まり、または `assert` などの少数の小文字名で、
  どれも `id_` で始まらない）とぶつからない。加工後に同じ識別子になった名前は、名前順で 2 つ目から `_2`, `_3` … を付けて
  区別する（ビルドエラーにはしない。値は各定数のコメントの元の名前で確かめられる）。名前空間は種類名（`event`, `bus` …）。
- 別名にする旧名は、どの種類の現在の名前とも同じであってはならない（エラー）。同じ旧名を 2 つのオブジェクトの別名にもできない。
- 出力: `ids.json`（kind, name, id, guid, aliasOf, banks）と `sonus_ids.h`（`namespace snd_ids::<kind>` の定数と
  `static_assert(... == snd::HashId("..."))`。別名は現在の ID を指す定数）。

## 6. 出力

| ファイル | 内容 |
| --- | --- |
| `<bank>.sndb`, `<bank>.<language>.sndb` | バンク（bank-format.md）。開発ビルドは `GUID` チャンクと `kFileDevBuild`、`--strip-strings` で `STRS`/`NAME` を除く |
| `streams/<名前>_<id8桁>[.<language>].snds` | ストリーム波形（下記）。`SPTH` のパスは出力ディレクトリからの相対 |
| `ids.json`, `sonus_ids.h` | ID 表、ゲーム用ヘッダ |

**ストリームファイル**: 形式は bank-format.md §6.3（`'SNDS'`、`bank_format::StreamFileHeader`）。

**PCM の詰め方**（codec 担当への申し送り）: `PCMF` はインターリーブの float32 LE、`PC16` はインターリーブの int16 LE、
`framesPerPacket = 1`、`blockAlign = channels × バイト数`。

## 7. 決定論と差分ビルド

- 同じ入力から同じバイト列: ファイルは名前順、行は名前順（バスは 4 節、遷移ルールは記述順）、時刻・パス・ポインタを含めない。
- `contentHash` = FNV-1a 64（本体 `[128, fileBytes)` ＋ bankId・機種・フラグ・言語）。`projectBuildHash` = 全バンクの
  （ファイル名, contentHash）の FNV-1a 64。
- 変換結果（波形のエンコード）は `--cache <dir>` に内容ハッシュ（WAV のバイト列 ＋ 変換設定 ＋ エンコーダの版）で保存し、
  次回は再エンコードしない。中身が変わらない出力ファイルは書き直さない。キャッシュの各エントリは末尾に中身の FNV-1a 64 を持ち、
  合わなければ（壊れていれば）黙って捨てて再エンコードする。
- ビルドは書き出す各バンクを bank-format.md §8 と同じ規則で自己検査し、通らなければエラーにする（ビルドが成功したバンクは
  必ずランタイムの `ParseBank` を通る）。
