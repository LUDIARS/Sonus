# ライブリンクのメッセージ仕様（プロトコル 1.0）

設計書 11.2 の「バージョン付きのバイナリ形式」。**凍結対象**（`spec/interface/CONTRACTS.md`）。
正本は `contract/internal/include/snd/format/livelink_protocol.hpp`。本書と食い違う場合はヘッダが正。

## 1. 接続

- ゲームに組み込んだランタイムが TCP で待ち受け（`Config.liveLink.port`、既定 27460）、ツールが接続する。同時接続は 1。
- リリースビルドでは `SND_LIVELINK=0` でコードごと外す（11.2）。
- 受信したメッセージはライブリンクのスレッドでコマンド（`snd/internal/commands.hpp` の `Live*`）に変換して投入する。
  ランタイムデータへ直接書き込まない（2 章のスレッド表）。
- 切断しても上書きした値は残る（11.2）。

## 2. フレーム

すべてのメッセージは 16 バイトの `FrameHeader` + `payloadBytes` バイトのペイロード。整数はリトルエンディアン、
浮動小数は IEEE-754 binary32。構造体に暗黙のパディングは無い（`static_assert` で確認）。

| offset | 型 | 名前 | 内容 |
| --- | --- | --- | --- |
| 0 | u32 | magic | `'SNLL'`（`53 4E 4C 4C`） |
| 4 | u16 | type | `MessageType` |
| 6 | u16 | flags | bit0: 受信側は `Ack(seq)` を返す |
| 8 | u32 | payloadBytes | ≤ 16 MiB。超えたら接続を切る |
| 12 | u32 | seq | 送信側の通し番号 |

magic 不一致・上限超えは `Error` を送って切断する。未知の `type` は `payloadBytes` を読み飛ばす。

## 3. 互換性

- **major**（`kProtocolMajor` = 1）が違えば接続を拒否する（`HelloReply.status = RejectedProtocol` を送って切断）。
- **minor**（`kProtocolMinor` = 0）は追加のみ: 新しいメッセージ種別、既存ペイロード末尾へのフィールド追加、リスト要素の拡張。
  受信側は既知部分の大きさ（`FixedPayloadBytes`）と `payloadBytes` の小さい方だけを読み、足りない部分は 0 とみなす。
  リストは `ListHeader { u32 count; u32 elemBytes; }` を先頭に持ち、要素は `elemBytes` 刻みで読む。

## 4. 接続時の照合（11.2）

1. ツール → `Hello`（56 B）: `u16 protocolMajor, u16 protocolMinor, u32 role(=1 Tool), u64 projectBuildHash, u32 contractVersion, u32 capabilities(0), char name[32]`。
2. ゲーム → `HelloReply`（32 B）: `u16 major, u16 minor, u32 status, u64 runtimeBuildHash, u32 contractVersion, u32 sampleRate, u32 blockSize, u32 platform`。
   - `status` 0 Accepted（ビルドハッシュ一致）、1 BuildMismatch、2 RejectedProtocol。
   - `runtimeBuildHash` はロード済みバンクの `projectBuildHash`（複数あれば全部が同じであること。違えば BuildMismatch）。
3. BuildMismatch のとき、ツール → `SyncDecision`（8 B）: `u32 mode`（0 差分をすべて送る、1 切断）、`u32 reserved`。
   差分は `SetValue` / `ReplaceChunk` で送る。
4. 以後はどちらからでも `Ping`（`u64 senderTimeNs`）/ `Pong`（`u64 senderTimeNs, u64 gameClockSample`）。

## 5. ツール → ゲーム

| type | 名前 | ペイロード | 変換先コマンド |
| --- | --- | --- | --- |
| 0x0010 | SetValue（48 B） | `u64 guidLo, u64 guidHi, u32 runtimeId, u32 chunk, u32 column, u32 valueType, u32 index, u32 reserved, u64 value` | `LiveSetValue` |
| 0x0011 | ReplaceChunk（16 B + データ） | `u32 bankId, u32 fourcc, u64 bytes` の後にチャンク本体（バンク内と同じレイアウト） | 通常の読み込み経路で準備 → `LiveReplaceChunk` |
| 0x0012 | Audition（24 B） | `u32 eventId, u32 flags(bit0 全停止, bit1 位置指定), f32 position[3], u32 reserved` | `LiveAudition` |
| 0x0013 | OverrideParam（16 B） | `u32 paramId, u32 enable, f32 value, u32 reserved` | `LiveOverride(Param)` |
| 0x0014 | OverrideState（16 B） | `u32 musicEventId, u32 stateId, u32 enable, u32 reserved` | `LiveOverride(MusicState)` |
| 0x0015 | ClearOverrides（0 B） | — | `LiveClearOverrides` |
| 0x0016 | SetProfilerMask（8 B） | `u32 mask, u32 meterHz`（mask: bit0 ボイス, 1 時間, 2 メモリ, 3 ストリーム, 4 LOD, 5 イベントログ, 6 メータ） | — |
| 0x0017 | RecordControl（8 B） | `u32 start(1/0), u32 reserved`（11.3 記録の開始・停止） | — |
| 0x0018 | StructEdit（16 B + 引数） | `u32 op(0 AddBus,1 RemoveBus,2 AddEffect,3 RemoveEffect,4 AddSend), u32 targetId, u32 argCount, u32 reserved` + `u64 args[argCount]` | `Config.devReserve` の範囲で反映、超えたら `Error`（再ビルドとホットリロードへ） |

**値の変更の宛先**: `SetValue` はバンクのテーブル列で項目を指定する。`chunk` は `bank_format` のチャンク fourcc（`EVNT`, `BUSS`, `EFPV` …）、
`column` はその列 ID（`bank-format.md` §6）、`runtimeId` は対象オブジェクトの ID、`index` はリスト型の何番目か
（例: エフェクトの 3 番目のパラメータ）。`valueType` は 0 f32, 1 u32, 2 i32, 3 ID。値は `value` の下位ビットに入れる。
GUID は開発ビルドのバンクの `GUID` チャンクとの照合用（ID 衝突やリネームの検出）。

**応答と背圧**（段階2 livelink の受け入れ側の決定）:

- `flags` bit0 のメッセージには、各 seq にちょうど 1 回 `Ack(seq, result)` で答える。bit0 が無いものは失敗のときだけ `Error` を返す。
- `SetValue` / `ReplaceChunk` の Ack は、ゲームのオーディオスレッドが適用した結果で答える。適用された結果の Ack は seq の順に届く。受け付けの時点で拒否したもの（検査の失敗、`SND_ERR_COMMAND_BUFFER_FULL` など）の Ack は、それより前に送った適用待ちの Ack より先に届くことがある。
- 背圧: ゲームは、適用待ちの結果と応答の送信待ちに空きがある分だけ受信する。空きが無い間は読まず、ツールのメッセージは転送路（TCP）に溜まる。読む量の上限はコマンドバッファの空きを見ない。`SND_ERR_COMMAND_BUFFER_FULL` で拒否された `SetValue` などを送り直すのはツールの責任。
- ゲームがツールを切断するのは、心拍のタイムアウトの間ツールから何も届かないとき、またはツールが送信をまったく読まないときだけ。
- セッション（D-83, D-87）: ゲームのライブリンク（Server）を作り直すと新しいセッションになる。Server を作り直したあと、前の Server が受け付けて未適用だった編集（`SetValue`、`ReplaceChunk`、`OverrideParam` / `OverrideState`、`ClearOverrides`、切断時の戻し）は適用されない（捨てて数える）。その結果に対する Ack は誰にも送られない（前のツールの接続は既に無い）。新しいセッションは Server の開始（`Server::Start`）で始まる。例外として、前の Server の切断時の戻し（revertOnDisconnect）は適用する。ただし戻すのは、前のセッションが最後に書いた値と最後に設定したオーバーライドだけで、新しい Server が書いた値・設定したオーバーライドは戻さない（最新のセッションの戻しと ClearOverrides は全部を消す）。

## 6. ゲーム → ツール

| type | 名前 | ペイロード | 頻度 |
| --- | --- | --- | --- |
| 0x0100 | VoiceList | `ListHeader` + `VoiceInfo`（32 B: `u32 eventId, u32 playbackSlot, u32 playbackState, f32 audibility, u32 isVirtual, u32 busId, f32 gain, u32 sourceKind`） | メータと同じ |
| 0x0101 | BlockTiming（32 B） | `u64 blockIndex, f32 logicUs, f32 renderUs, f32 budgetUs, u32 lodLevel, u32 lateChunks, u32 underruns` | 間引いて送る |
| 0x0102 | MemoryUsage（192 B） | `u64 used[12], u64 budget[12]`（添字は `snd_mem_tag`） | 1 Hz 程度 |
| 0x0103 | StreamStall（16 B） | `u32 eventId, u32 streamSlot, u64 clockSample` | 発生時 |
| 0x0104 | LodChange（16 B） | `u32 newLevel, u32 oldLevel, u64 blockIndex` | 発生時 |
| 0x0105 | EventLog | `ListHeader` + `LogEntry`（24 B: `u64 clockSample, u32 kind, u32 id, u32 handleSlot, u32 detail`） | まとめて |
| 0x0106 | BusMeters | `ListHeader` + `MeterEntry`（72 B: `u32 busId, u32 channels, f32 peak[8], f32 rms[8]`） | 約 30 Hz（`meterHz`） |
| 0x0107 | RecordData（16 B + データ） | `u64 offset, u64 bytes` + 記録データ（11.3） | 記録中 |
| 0x0006 | Ack（8 B） | `u32 seq, i32 result(snd_result)` | flags bit0 のとき |
| 0x0007 | Error（64 B） | `i32 code, u32 seq, char message[56]` | 失敗時 |

送信はライブリンクのスレッドが、オーディオスレッドの公開データ（トリプルバッファ、イベントリング、統計）を読んで行う。
オーディオスレッドはソケットに触れない。

## 7. 記録データ（11.3）

`RecordData` の中身は記録ファイルと同じバイト列で、次のレコードの連続（すべて 8 バイト境界）:

| レコード | 内容 |
| --- | --- |
| Arena | `u32 kind=1, u32 producer, u64 appliedBlock, u32 bytes, u32 reserved` + アリーナの正規領域・予約領域・フォールバック（`commands.hpp` の順） |
| Lod | `u32 kind=2, u32 level, u64 block` |
| Stall | `u32 kind=3, u32 streamSlot, u64 block` |
| BankMap | `u32 kind=4, u32 bankId, u64 contentHash`（`BankRegister.prepared` のポインタを ID に置き換えるため） |

記録はプラットフォーム別（コマンドはポインタを含む）。再生側はポインタを ID で引き直す。

記録は開始時点の状態（バンクの読み込み、鳴っている音、上書き、パラメータ）を持たない。再生は、記録を始めたときと同じ状態から始めたときに記録時と同じ出力になる（段階2 livelink の受け入れ側の決定。開始時の状態を記録に書き出すことは契約 1.2 の候補）。
