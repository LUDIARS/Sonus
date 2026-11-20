# ライブリンクのメッセージ仕様（プロトコル 1.1）

設計書 11.2 の「バージョン付きのバイナリ形式」。**凍結対象**（`spec/interface/CONTRACTS.md`）。
正本は `contract/internal/include/snd/format/livelink_protocol.hpp`。本書と食い違う場合はヘッダが正。

## 1. 接続

- ゲームに組み込んだランタイムが TCP で待ち受け（`Config.liveLink.port`、既定 27460）、ツールが接続する。同時接続は 1。
- 1 つの `LiveRuntime` で同時に Start できる Server は 1 つ（D-170）。2 つ目の `Server::Start` は SND_ERR_LIMIT_REACHED。
  先の Server を Stop / 破棄すれば Start できる。
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
- **minor**（`kProtocolMinor` = 1）は追加のみ: 新しいメッセージ種別、既存ペイロード末尾へのフィールド追加、リスト要素の拡張。
  受信側は既知部分の大きさ（`FixedPayloadBytes`）と `payloadBytes` の小さい方だけを読み、足りない部分は 0 とみなす。
  リストは `ListHeader { u32 count; u32 elemBytes; }` を先頭に持ち、要素は `elemBytes` 刻みで読む。
- **minor の取り決め（1.1）**: 双方が自分の minor を `Hello.protocolMinor` / `HelloReply.protocolMinor` で名乗る。
  送り手は、相手の minor がそのメッセージの minor（`MinorOf(type)`、§8 の表）以上のときだけ送る。
  - 1.1 のツールと 1.0 のゲーム: ツールは 1.1 の要求（RevertValue、ReadCell、QueryBanks、ReloadBank）を送らない
    （`Client` の送信関数は 0 を返す。`Client::Supports(type)` で前もって分かる）。`SetProfilerMask` は 1.0 のビット
    （`ProfilerMaskOf(0)`）だけに絞って送る（1.0 のゲームは未知のビットを INVALID_ARGUMENT で断るため）。
  - 1.0 のツールと 1.1 のゲーム: 受け付ける（minor は拒否の理由にならない）。ゲームが 1.1 で足した送信は、1.1 の要求への
    返事か、1.0 のツールが立てられないマスクのビット（`kProfStreamList`、`kProfBlockDetail`）の流れだけなので、
    1.0 のツールに 1.1 の種別は届かない。届いても未知の種別として読み飛ばされる。
  - 未知の種別に Ack の bit0 が立っていても、受け手は答えない（読み飛ばす）。新しい種別を相手の minor を見ずに送ると
    Ack を待ち続けることになるので、上の規則で送らない。

## 4. 接続時の照合（11.2）

1. ツール → `Hello`（56 B）: `u16 protocolMajor, u16 protocolMinor, u32 role(=1 Tool), u64 projectBuildHash, u32 contractVersion, u32 capabilities(0), char name[32]`。
2. ゲーム → `HelloReply`（32 B）: `u16 major, u16 minor, u32 status, u64 runtimeBuildHash, u32 contractVersion, u32 sampleRate, u32 blockSize, u32 platform`。
   - `status` 0 Accepted（ビルドハッシュ一致）、1 BuildMismatch、2 RejectedProtocol。
   - `runtimeBuildHash` はロード済みバンクの `projectBuildHash`（複数あれば全部が同じであること。違えば BuildMismatch）。
3. BuildMismatch のとき、ツール → `SyncDecision`（8 B）: `u32 mode`（0 差分をすべて送る、1 切断）、`u32 reserved`。
   差分は `SetValue` / `ReplaceChunk` で送る（バンクごとに作り直しが要るものは `ReloadBank`、§8.5）。
   - 1.1: `SyncDecision` の前に `QueryBanks` を送ってよい（AwaitSync の間でも答える）。返事の `BankList` に、ロード済みの
     各バンクの bankId、言語、`projectBuildHash`、`contentHash`、書き込み可能か（開発ビルドの上書き層があるか）が入る。
     ツールはこれでバンクごとの差を判定し、`SendDiffs` か `Disconnect` を選ぶ。`SyncDecision` はゲームの
     `handshakeTimeoutMs`（既定 5 秒）のうちに送る。`Client` は `ClientDesc::deferSyncDecision` で決定を保留し
     （状態は Handshake のまま、`AwaitingSync()`）、`DecideSync(mode)` で答える。
   - 端の規則（1.1 で確かめて決めた）:
     - バンクが 1 つも無い: `runtimeBuildHash` = 0。ツールのハッシュも 0 のときだけ Accepted、ほかは BuildMismatch
       （差分は無いので SendDiffs でよい。後から読み込まれたバンクは照合し直さない。ツールは QueryBanks で見る）。
     - バンクどうしのハッシュが食い違う: BuildMismatch。`runtimeBuildHash` は最初に登録されたバンク（`LiveBanks` の順）の
       ハッシュ。ツールのハッシュがどれかと一致していても BuildMismatch（どれが合わないかは BankList で分かる）。
     - 言語違いの同じ bankId、上書き層の無いリリースビルドのバンクも照合に数える。
     - 接続の後でバンクが変わっても（読み込み、ホットリロード）照合はやり直さない。
4. 以後はどちらからでも `Ping`（`u64 senderTimeNs`）/ `Pong`（`u64 senderTimeNs, u64 gameClockSample`）。

## 5. ツール → ゲーム

| type | 名前 | ペイロード | 変換先コマンド |
| --- | --- | --- | --- |
| 0x0010 | SetValue（48 B） | `u64 guidLo, u64 guidHi, u32 runtimeId, u32 chunk, u32 column, u32 valueType, u32 index, u32 reserved, u64 value` | `LiveSetValue` |
| 0x0011 | ReplaceChunk（16 B + データ） | `u32 bankId, u32 fourcc, u64 bytes` の後にチャンク本体（バンク内と同じレイアウト） | 通常の読み込み経路で準備 → `LiveReplaceChunk` |
| 0x0012 | Audition（24 B） | `u32 eventId, u32 flags(bit0 全停止, bit1 位置指定), f32 position[3], u32 reserved` | `LiveAudition`（全停止は §8.6） |
| 0x0013 | OverrideParam（16 B） | `u32 paramId, u32 enable, f32 value, u32 reserved` | `LiveOverride(Param)` |
| 0x0014 | OverrideState（16 B） | `u32 musicEventId, u32 stateId, u32 enable, u32 reserved` | `LiveOverride(MusicState)` |
| 0x0015 | ClearOverrides（0 B） | — | `LiveClearOverrides` |
| 0x0016 | SetProfilerMask（8 B） | `u32 mask, u32 meterHz`（mask: bit0 ボイス, 1 時間, 2 メモリ, 3 ストリーム, 4 LOD, 5 イベントログ, 6 メータ） | — |
| 0x0017 | RecordControl（8 B） | `u32 start(1/0), u32 reserved`（11.3 記録の開始・停止） | — |
| 0x0018 | StructEdit（16 B + 引数） | `u32 op(0 AddBus,1 RemoveBus,2 AddEffect,3 RemoveEffect,4 AddSend), u32 targetId, u32 argCount, u32 reserved` + `u64 args[argCount]` | `Config.devReserve` の範囲で反映、超えたら `Error`（再ビルドとホットリロードへ）。引数と結果は §8.4 |

**値の変更の宛先**: `SetValue` はバンクのテーブル列で項目を指定する。`chunk` は `bank_format` のチャンク fourcc（`EVNT`, `BUSS`, `EFPV` …）、
`column` はその列 ID（`bank-format.md` §6）、`runtimeId` は対象オブジェクトの ID、`index` はリスト型の何番目か
（例: エフェクトの 3 番目のパラメータ）。`valueType` は 0 f32, 1 u32, 2 i32, 3 ID。値は `value` の下位ビットに入れる。
GUID は開発ビルドのバンクの `GUID` チャンクとの照合用（ID 衝突やリネームの検出）。

`SetValue` は全部か無しか（D-171）: そのオブジェクトを持つバンクのどれか 1 つでも書けなければ（上書き層の無いリリースビルドの
バンクが同じオブジェクトを持つ、など）、どのセルも書かずに SND_ERR_UNSUPPORTED（ほかの理由はその結果）で答える。
`ReplaceChunk`、`RevertValue` も同じ。

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

`budgetUs`（BlockTiming / BlockDetail の `budgetMicroseconds`）の定義（1.1 で固定）: 1 ブロックの実時間 ×
`tuning.load_target_percent` / 100 = `1e6 × block_size / sample_rate × load_target_percent / 100`（µs、binary32 に丸めた値）。
負荷 LOD が比べる予算と同じ値（設計書 2.4、core の `snd_stats.block_budget_us`）。間引きは「区間でいちばん重いブロック」で、
`lateChunks` だけは区間の合計。
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

| LiveData（1.1） | `u32 kind=5, u32 fourcc, u64 key, u32 bankId, u32 bytes` + データ（8 バイト境界まで詰め物） | 次の Arena にある `LiveReplaceChunk` のデータ。`key` は記録したプロセスでの `prepared` の値（組み合わせの鍵）。`fourcc` が `kStructEditTag` なら構造の変更（`StructEdit` + 引数） |
| Begin（1.1） | `u32 kind=6, u16 origin, u16 lostState, u64 block`（16 B） | 記録の始まり方。`origin` 1 FromCreation（記録の前にコマンドが 1 つも適用されていない）/ 2 MidRun。`lostState` は書けなかった開始時の状態の数 |
| Cell（1.1） | `u32 kind=7, u32 bankId, u32 fourcc, u32 column, u32 row, u32 reserved, u64 value, u64 original`（40 B） | 開始時の上書き層のセル（今の値とバンクの値） |
| Override（1.1） | `u32 kind=8, u32 overrideKind, u32 id, u32 value, u64 target`（24 B） | 開始時のライブのオーバーライド（`cmd::LiveOverrideKind`、Param は値の float のビット） |

記録はプラットフォーム別（コマンドはポインタを含む）。再生側はポインタを ID で引き直す。

**並び**: 最初は Lod（開始時の段）、次に開始時に登録済みのバンクの BankMap、次に Begin、続けて Cell と Override（1.1）、
その後に Arena（と LiveData、Lod、Stall、記録中に登録されたバンクの BankMap）。1.1 の種別は `RecordKind` の列挙子ではなく
定数（`kRecordLiveData` など）で、`RecordItem::kind` には `static_cast<RecordKind>(値)` で入る（他のモジュールの
`switch` を壊さないため）。1.0 の読み手は 1.1 の種別で止まる（未知の種別）。

**開始時の状態（1.1）**: 記録は「作成時から」か「途中から」かを Begin で区別する。途中からの記録には、ライブリンクが持つ状態
（上書き層のセル、ライブのオーバーライド）を書き出す。再生はセルを先行ブロック（`ReplayInfo::leadBlocks` = 1）で書き、
オーバーライドを同じブロックで `LiveOverride` として当ててから、記録した最初のブロックを再生する（値の変化は先行ブロックの間に
落ち着く）。core が持つ状態（鳴っている音、パラメータの値、エミッタ、トランスポート）は記録に入らない（依頼 C-04）。
途中からの記録は、再生するシステムが同じ core の状態から始めたときだけ記録時と同じ出力になる（再生は `kReplayMidRun` で知らせる）。

**再生での扱い（1.1）**:

| 記録にあるもの | 再生 |
| --- | --- |
| `LiveReplaceChunk`（LiveData あり） | 再生側の写しを指すように書き換えて適用する（`ReplayInfo::replacedChunks`） |
| `LiveReplaceChunk`（LiveData なし: 1.0 の記録、記録時にリングに入らなかった） | 外して数える（`skippedCommands`、`kReplaySkipped`） |
| 構造の変更（LiveData の `kStructEditTag`） | `ReplayDesc::structEditor` に渡す。無ければ数えて `kReplayStructEdits`（2.5 の外） |
| 記録中の BankMap（ホットリロード、読み込み） | 再生側は読み直さない。`bankChanges` と `kReplayBankChanges`（2.5 の外） |
| Lod の段が 0 でない、段の変化 | 再現しない（core に段を強制する手段が無い、依頼 C-05）。`kReplayLodRecorded` で知らせる |
| Stall | 再現しない（同上）。`kReplayStallRecorded` |
| 再生中の LOD の変化・ストール | 数えて `kReplayLodInReplay` / `kReplayStallInReplay` |

述語（D-175）: `Replayer::BitExactExpected()` は同じ記録の 2 回の再生どうしがビットまで一致すると期待できるとき（2.5 を壊す条件
`kReplayBreaksBitExact` が 1 つも無い）。`ReproducesRecordedRun()` は、それに加えて再生が記録した実行と一致すると期待できるとき:
作成時からの記録、または途中からでも開始時の状態が全部入っている（`lostState` 0。開始時に core で生きていた再生は lostState に
数える）。1.0 の記録（Begin なし）は false。開始時に見分けられない core の状態（それまでに設定したパラメータの値、エミッタ、
トランスポート、リスナー。C-04）は既定値と見なす。`kReplayMidRun` は情報として `conditions` に出るだけ。
`Replayer::Finished()` は最後の記録ブロックを描き終えた時点（Start の後の `Blocks()` 個目、先行ブロックを含む）で true。比べる材料として、再生は
決定のログ（`ReplayDecision`: 適用した Play / LiveAudition / Stop、再生の終わりと理由、イベントリングのマーカー・拍・エラー・警告、
ボイスの一覧の要約。ブロックは再生の始まりからの相対）と、出力のハッシュ（`OutputDigest`: サンプルのビットの FNV-1a）を出す。


## 8. プロトコル 1.1 の追加（段階5 組 L）

### 8.1 新しいメッセージ

| type | 名前 | 方向 | ペイロード | 答え |
| --- | --- | --- | --- | --- |
| 0x0019 | RevertValue（48 B） | ツール → ゲーム | `SetValue` と同じ形（宛先だけ使う。`valueType` と `value` は見ない） | 適用した結果の Ack（§5 の順序） |
| 0x001A | ReadCell（48 B） | ツール → ゲーム | 同上 | `CellValue`（bit0 なら続けて Ack） |
| 0x001B | QueryBanks（0 B） | ツール → ゲーム | — | `BankList`（bit0 なら続けて Ack）。AwaitSync の間も可 |
| 0x001C | ReloadBank（16 B + パス） | ツール → ゲーム | `u32 bankId, u32 languageId, u32 flags(bit0 無ければ読み込む), u32 pathBytes` + パス（UTF-8、NUL なし、1〜1024 B） | `ReloadStatus`（途中経過）と最後の Ack |
| 0x0108 | CellValue（32 B） | ゲーム → ツール | `u32 requestSeq, i32 result, u32 cells, u32 flags(bit0 上書き中, bit1 バンク間で値が違う), u64 current, u64 original` | — |
| 0x0109 | BankList | ゲーム → ツール | `ListHeader` + `BankEntry`（32 B: `u32 bankId, u32 languageId, u64 projectBuildHash, u64 contentHash, u32 writable, u32 slot`） | — |
| 0x010A | StreamList | ゲーム → ツール | `ListHeader` + `StreamInfo`（16 B: `u32 slot, u32 eventId, u32 state(0 Idle, 1 Active), u32 stalls`） | `kProfStreamList`（bit7）、メータの頻度 |
| 0x010B | BlockDetail（48 B） | ゲーム → ツール | `u64 blockIndex, f32 totalUs, f32 logicUs, f32 renderUs, f32 budgetUs, u32 renderedVoices, u32 chunks, u32 lateChunks, u32 heldVoices, u32 parallel, u32 lodLevel` | `kProfBlockDetail`（bit8）、タイミングの頻度（区間でいちばん重いブロック） |
| 0x010C | ReloadStatus（24 B） | ゲーム → ツール | `u32 requestSeq, u32 phase(1 Unloading, 2 Loading, 3 Done, 4 Failed), i32 result, u32 bankId, u64 bank`（新しい登録の snd_bank_handle） | — |

返事のフレームの `seq` は要求の `seq`（`CellValue.requestSeq` / `ReloadStatus.requestSeq` も同じ値）。`MinorOf(type)` はどれも 1。

### 8.2 項目単位の Revert と読み返し

- `RevertValue`: 宛先のセル（そのオブジェクトを持つ全バンク）を、上書き層が覚えている元の値に戻し、覚えを消す。上書きされて
  いなければ何もせず SND_OK。セルが無ければ SND_ERR_NOT_FOUND、表・列が無ければ受け付けの時点で SND_ERR_NOT_FOUND。
  パラメータ・State のオーバーライドの解除は従来どおり `OverrideParam` / `OverrideState` の `enable = 0`。
- `ReadCell`: オーディオスレッドが適用の位置で読む（書かない。原則 1 のとおり確保・ロックなし）。`current` は最初のセルの今の値、
  `original` は覚えている元の値（上書きされていなければ `current` と同じ）、`cells` はセルを持つバンクの数、`kCellsDiffer` は
  バンク間で値が違うとき。f32 の列は値のビット、u32 の列はその値。セルが無ければ result NOT_FOUND、cells 0。
  宛先の形が悪いもの（表・列が無い）は受け付けの時点で `CellValue{result}` を返す。
- セッション（D-83、D-87）、背圧（D-86）は `SetValue` と同じ。古いセッションの ReadCell には答えない（結果を捨てる）。

### 8.3 ReplaceChunk が受け付けるもの

| 内容 | 結果 |
| --- | --- |
| 既知の表（`kChunks` の Table）、行数がロード済みと同じ、列の値の変更 | 受け付け。全セルを検査してから書く（一部だけ書くことはない）。ブロック境界で反映 |
| 未知の列 ID | 読み飛ばす（bank-format.md の互換） |
| 列が 0 本（行数は同じ） | SND_OK、何も変えない（D-173） |
| 置き換えに無い列（列が減った） | その列は今の値のまま |
| 行数が違う（行の増減） | SND_ERR_UNSUPPORTED（再ビルドとホットリロード） |
| 参照・配置の列（`parent`、`effectFirst` など core が登録時に組み立てたもの）が変わる | SND_ERR_UNSUPPORTED（同上）。同じ値なら可 |
| 文字列・可変長の表、ブロブ（`WAVE` など） | SND_ERR_UNSUPPORTED（同上） |
| 列の要素の大きさが型と違う、64 バイト境界でない、データからはみ出す、同じ列 ID が 2 回 | SND_ERR_INVALID_ARGUMENT |
| bankId のバンクが無い | SND_ERR_NOT_FOUND |
| 上書き層の無い（リリースビルドの）バンク | SND_ERR_UNSUPPORTED |
| データが `RuntimeDesc::chunkBytes` より大きい | SND_ERR_OUT_OF_BUDGET |
| 差し替えの枠（`chunkSlots`）が空いていない | SND_ERR_LIMIT_REACHED（送り直す） |
| ホットリロードの最中 | SND_ERR_NOT_READY |

受け付けた差し替えはセルの書き込みと同じに覚える（切断時の戻し、項目単位の Revert、ReadCell がそのまま効く）。
記録中は差し替えのデータを LiveData で記録に入れる（§7）。

### 8.4 構造の変更（StructEdit）

引数（`args[]`、`wire.hpp` の `CheckStructArgs`）:

| op | targetId | args | 予備容量（`Config.dev_reserve`）から取るもの |
| --- | --- | --- | --- |
| AddBus | 新しいバスの ID | `{親バス ID, チャンネル数 1..8 [, volumeDb f32]}` | buses 1、bytes 64 + 8 × argCount |
| RemoveBus | バス ID | なし | なし（ライブで足したバスを外すと、その Add の費用（buses・bytes）を全部返す。そのバスの上にライブで足したエフェクトと、そのバスから出るライブのセンドも一緒に外れ、それぞれの費用（effects・bytes）を返す。D-172） |
| AddEffect | バス ID | `{エフェクト種別 ID, 挿す位置（~0u = 末尾）, パラメータ f32 …（32 個まで）}` | effects 1、bytes 64 + 8 × argCount |
| RemoveEffect | バス ID | `{エフェクトの番号（< 65536）}` | なし（同じバスにライブで足した最後のエフェクトを外すと、その Add の費用（effects・bytes）を全部返す） |
| AddSend | 送り元バス ID | `{送り先バス ID, レベル dB f32 [, 1 = プリフェーダー]}` | bytes 64 + 8 × argCount |

ID は 0 でない 32 ビット、f32 は有限で下位 32 ビット。`events` の予備は、イベントを足す op がまだ無いので使わない。

結果（Ack、bit0 が無ければ失敗のときだけ Error と理由の文）:

| 結果 | 意味 | ツールの次の手 |
| --- | --- | --- |
| SND_OK | 反映した（ブロック境界） | — |
| SND_ERR_INVALID_ARGUMENT | 引数の規則に合わない | 直して送る |
| SND_ERR_OUT_OF_BUDGET | 予備容量を超える（Error の文 "dev reserve exhausted: rebuild and hot reload"） | 再ビルドと ReloadBank |
| SND_ERR_UNSUPPORTED | core に構造を変える手段が無い（今はこれ。依頼 C-01） | 再ビルドと ReloadBank |
| SND_ERR_NOT_READY | ホットリロードの最中 | 終わってから送る |
| SND_ERR_LIMIT_REACHED | 戻しの記録（`RuntimeDesc::maxStructLog`、既定 64）か差し替えの枠（`chunkSlots`）が一杯（D-172）。予備容量は取らず、編集器も呼ばない | 戻すか、再ビルドと ReloadBank |
| ほか（NOT_FOUND など） | 編集器が断った | — |

戻しの記録（D-172）: どの構造の変更も、受け付けの時点で戻しの記録の枠を 1 つ取る（取れなければ LIMIT_REACHED）。適用された
編集はその枠に残り、適用されなかった・ライブの Add を打ち消した Remove・戻した編集の枠はオーディオスレッドが返す。よって
「適用されたが戻せない」編集は無い。ライブで足したバスを外したとき、一緒に外れたエフェクト・センドの Undo は編集器に渡さない。
予備容量はゲーム（`LiveRuntime`）ごとに数え、Server を作り直しても続く。反映されなかった編集（断られた、古いセッション、
Server の破棄で取り消し）の分はオーディオスレッドが返す。切断時の戻し（revertOnDisconnect）は、そのセッション以前の構造の
変更を新しい順に元に戻し（編集器の Undo）、予備容量を返す。記録中は LiveData（`kStructEditTag`）で記録に入る。
core に渡す最後の一歩は livelink の中の継ぎ目（`snd/livelink/struct_edit.hpp` の `IStructEditor`、
`RuntimeDesc::structEditor`）。既定（null）は SND_ERR_UNSUPPORTED。

### 8.5 ホットリロード（ReloadBank）

1. ゲームはロード済みの（bankId, languageId）のバンクを `UnloadBank(STOP_AND_UNLOAD)` で外す（そのバンクの音は短いフェードで止まる。
   設計書 10.3）。依存元がロードされていれば外せず、その結果（SND_ERR_BANK_IN_USE）で答える。
2. 同じ呼び出しの中で、指定のパス（ゲームのファイルシステムの根からの相対、D-103 の規則）を `LoadBankAsync` で読み込む。
   バンク管理は、古い写しが解放されてから（エポック、3.3）新しい写しを登録する。
3. `ReloadStatus` を Unloading → Loading → Done / Failed の順に送り、最後に Ack（Done は SND_OK、読めなかった・壊れていたは
   SND_ERR_IO、新しい登録が外されたら SND_ERR_INVALID_HANDLE）。読み込みに失敗すると、バンクは外れたままになる。
4. 読み込んだファイルのバンクが要求と違う（bankId か言語）ときは Failed（SND_ERR_INVALID_ARGUMENT、Error の文
   "ReloadBank: the file holds another bank"）。読み込んだ別のバンクは登録が済み次第外す（残さない）。要求したバンクは外れたまま
   （読み込みの失敗と同じ。D-173）。外す前にファイルのヘッダを読んで確かめることはしていない: ライブリンクのスレッドから
   ゲームのファイルシステムを読むと IO スケジューラと読み込みの識別子（IoToken）を取り合うため（C-03 の読み込みの入口で直す）。
   Done は新しい登録がオーディオ側に公開されてから送る（その後の BankList / SetValue は新しい登録を見る）。
5. ロードされていなければ SND_ERR_NOT_FOUND。flags bit0（`kReloadLoadIfAbsent`）があれば読み込むだけ。同時に 1 つまで
   （2 つ目は SND_ERR_LIMIT_REACHED）。

- 最中の扱い: `SetValue`、`ReplaceChunk`、`RevertValue`、`StructEdit` は SND_ERR_NOT_READY（ツールが送り直す）。`ReadCell`、
  `QueryBanks`、オーバーライド、試聴は受け付ける。
- 上書き層は古い登録の覚えを捨てる（新しいバンクの値が正）。ツールは新しいビルドの値をバンクに入れて送る。
- 切断・Server の作り直し: 外すと読み込むは同じ呼び出しで記録するので、途中で切れても読み込みは最後まで進む（Server が生きて
  いれば数える。答えは送らない）。
- 鳴ったまま差し替える（継ぎ目のない差し替え）は、core / bank に「同じ ID のバンクを並べて登録し、参照を差し替える」手段が要る
  （依頼 C-03）。バイト列を分割して送る形も、ゲームのファイルシステムを通さない読み込みが要るので依頼に書いた（C-03）。

### 8.6 試聴（Audition）

- 再生: `LiveAudition` はゲームの Playback スロットを 1 つ使う（`RecordLive` が取る。空きが無ければ SND_ERR_LIMIT_REACHED）。
  one-shot なので `on_end` はゲームに出ない。Audition の Ack は記録の結果（D-171）: 存在しない Event の試聴も SND_OK で、
  その再生は始まらずに終わり、試聴の枠は戻る。同時に鳴らせる試聴は `RuntimeDesc::maxAuditions`（既定 32）まで、超えると
  SND_ERR_LIMIT_REACHED。
- 全停止（flags bit0）: ライブリンクが始めた試聴だけを止める。順序は記録の順: 全停止より前に送った試聴が対象。オーディオスレッドが
  そのハンドルをライブリンクのスレッドに渡し、ライブリンクのスレッドが `System::Stop`（`tuning.short_fade_ms` のフェード）を記録する。
  ゲームの再生は止めない。Ack は止める指示を記録した後。止めるものが無くても SND_OK。
- 切断しても試聴は止めない（オーバーライドと同じく残る）。試聴ごとの停止は、ツールがハンドルを知らないので無い（依頼 C-02）。

### 8.7 プロファイラの追加

- StreamList（`kProfStreamList`）: `Config.max_streams` のスロットごとに、読んでいる再生のイベント（`LiveStreamEvent`）、状態、
  ライブリンクを作ってからのストールの数。先読みの残り・読み込み中かなどは core から取れない（依頼 C-06）。
- BlockDetail（`kProfBlockDetail`）: 1 ブロックの合計・ロジック・レンダーの時間、予算、描いたボイス、チャンク数、遅れたチャンク、
  保留したボイス、並列で描いたか、LOD の段（core の `BlockProfile`）。ワーカーごとの内訳は取れない（依頼 C-06）。
- どちらもほかのプロファイラと同じ帯域の上限と送信待ちの空きに従い、入らなければ捨てて数える（D-86）。

### 8.8 Client の更新番号と保持数

`Client::Sequences()` は受け取った数（履歴の流れは項目の数、最新値の流れはメッセージの数）を返す。`Copy*Since(after, …, &first)`
は番号が `after` より後の項目のうち残っているものを返し、`first > after + 1` なら `first - after - 1` 個を取りこぼした
（履歴から押し出された）。既存の `Copy*` はそのまま。

保持数（D-170、`client.hpp` の `kClient*`）: Ack 1024、CellValue 256、Error 256、ReloadStatus 64、StreamStall 1024、LodChange 1024、
BlockTiming と BlockDetail は historySeconds × 120、メータは historySeconds × 60 × バス数、EventLog は historySeconds × 2000。
最新値だけのもの（VoiceList、BusMeters、MemoryUsage、StreamList、BankList）は 1 つ。押し出した数は `ClientStats` の
`*Lost`（Ack と CellValue は受け取られないまま押し出された答えだけ）。

### 8.9 1.1 での変更の一覧

- `kProtocolMinor` 0 → 1。メッセージ種別 0x0019〜0x001C、0x0108〜0x010C。構造体 `ReloadBank`、`CellValue`、`BankEntry`、
  `StreamInfo`、`BlockDetail`、`ReloadStatus`、`RecordLiveData`、`RecordBegin`、`RecordCell`、`RecordOverride`。
  列挙 `ReloadFlags`、`CellFlags`、`StreamState`、`ReloadPhase`、`RecordOrigin`。`ProfilerMask` の末尾に bit7・bit8。
  定数 `kMaxReloadPathBytes`、`kRecordLiveData`〜`kRecordOverride`、`kStructEditTag`。関数 `MinorOf`、`ProfilerMaskOf`。
- 既存の値・大きさ・`static_assert` は変えていない。
