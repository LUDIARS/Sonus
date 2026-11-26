# 組 L（ライブリンク 1.1）独立テストの性質の一覧（indep_s7）

実装（`livelink/src/`）と実装担当のテストを読む前に、仕様と公開ヘッダだけから書き出した。
根拠の略号: **P** = `spec/interface/livelink-protocol.md`（1.1）、**H** = `contract/internal/include/snd/format/livelink_protocol.hpp`、
**W** = `livelink/include/snd/livelink/wire.hpp`、**R** = `runtime.hpp`、**S** = `server.hpp`、**C** = `client.hpp`、
**RP** = `replayer.hpp`、**RF** = `record_format.hpp`、**SE** = `struct_edit.hpp`、**VA** = `value_applier.hpp`、
**D** = 設計書（2.5、11.2、11.3）、**CT** = CONTRACTS.md の D-番号。

期待値はどれも解析的に決まる（件数、順序、ビット、seq、残量の数）。ゴールデンは使わない。

## A. 契約と wire の検査

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| A1 | `kProtocolMajor == 1`、`kProtocolMinor == 1`。`MinorOf` は 1.1 の 9 種別で 1、1.0 の全種別で 0 | H、P §8.1 | 全種別を列挙して比べる |
| A2 | `ProfilerMaskOf(0) == 0x7F`、`ProfilerMaskOf(1) == 0x1FF`、`ProfilerMaskOf(2) == 0x1FF`（後の minor でも 1.1 のビットまで） | H | 定数比較 |
| A3 | `FixedPayloadBytes` は 1.1 の種別で構造体の大きさ（QueryBanks 0、BankList / StreamList は ListHeader 8）。`IsKnown` は 0x0019〜0x001C、0x0108〜0x010C で true、0x001D・0x010D・0xFFFF で false | H | 定数比較 |
| A4 | `CheckSetProfilerMask(m)` は 1.0 の規則（bit7/8 は INVALID）、`CheckSetProfilerMask(m, 1)` は bit0〜8 を受け bit9 以上を INVALID | W、P §3 | 端のビット |
| A5 | `CheckStructArgs`: op ごとの引数の数（AddBus 2〜3、RemoveBus 0、AddEffect 2〜34、RemoveEffect 1、AddSend 2〜3）、ID 0 は INVALID、チャンネル 1..8、f32 は有限で上位 32 ビットが 0、RemoveEffect の番号 < 65536、未知の op は INVALID | W、P §8.4 | 境界の内外（0、1、8、9、65535、65536、NaN、Inf、上位ビット） |
| A6 | `StructCostOf`: AddBus = {1,0,64+8n}、AddEffect = {0,1,64+8n}、AddSend = {0,0,64+8n}、Remove* = {0,0,0} | SE、P §8.4 | n = 0..34 |
| A7 | `CheckReloadBank`: pathBytes 0 と 1025 は INVALID、1 と 1024 は OK、パスに NUL があれば INVALID | W、P §8.1 | 端の値 |
| A8 | `CheckCellAddress`: 未知の表・列は NOT_FOUND、`valueType` と `value` は見ない（何を入れても結果が同じ） | W、P §8.1 | 既知の列で valueType を 0..3・巨大値で比べる |
| A9 | `MessageTypeName` は 1.1 の種別に名前を返す（null でない） | W | 呼ぶだけ |

## B. 接続時の照合と minor の取り決め（P §3、§4）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| B1 | 1.1 のゲームは 1.0 の Hello（minor 0）を受け付け、HelloReply.protocolMinor = 1 で答える | P §3 | 手で組んだ 1.0 のツール |
| B2 | 1.0 のツール（mask 0x7F）には 1.1 の種別（0x0108〜0x010C）が届かない | P §3 | 一定時間読み続けて type を数える |
| B3 | 1.0 のツールが bit7 を立てた SetProfilerMask は 1.1 のゲームでは受け付けられる（ゲームの minor の規則）。届く StreamList は未知の種別として読み飛ばせる | P §3 | 手で組んだツール |
| B4 | 1.1 の Client と 1.0 のゲーム: `PeerMinor() == 0`、`Supports(1.1 の種別) == false`、RevertValue / ReadCell / QueryBanks / ReloadBank は 0 を返し何も送らない。`SetProfilerMask(0x1FF)` はゲームに `0x7F` で届く | P §3、C | 手で組んだ 1.0 のゲーム（Hello を読み HelloReply minor 0 を返す） |
| B5 | major の不一致 → `RejectedProtocol` で切断、`handshakesRejected` が増える | P §4 | 手で組んだツール |
| B6 | バンクが無い: tool hash 0 → Accepted、≠0 → BuildMismatch、`runtimeBuildHash == 0` | P §4 端の規則 | 本物の core |
| B7 | 2 つのバンクのハッシュが違う: ツールがどちらと一致しても BuildMismatch、`runtimeBuildHash` は最初に登録したバンクのもの | P §4 | 本物の core |
| B8 | `deferSyncDecision`: BuildMismatch で `AwaitingSync()`、状態は Handshake。その間 QueryBanks に BankList が返る（全バンク、hash と writable）。`DecideSync(SendDiffs)` → Ready、`DecideSync(Disconnect)` → Rejected。待っていないときの `DecideSync` は false | P §4、C | 本物の core |
| B9 | AwaitSync のまま `handshakeTimeoutMs` を過ぎるとゲームが切断する（timeouts が増える） | P §4 | 短い handshakeTimeoutMs |
| B10 | 順序の違反: Hello の前の SetValue / QueryBanks、Accepted の後の SyncDecision、SyncDecision の二重 → 落ちない、`protocolErrors` か Error で扱い、適用しない | P §2、S | 手で組んだツール |

## C. セルの Revert と読み返し（P §8.2）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| C1 | SetValue → ReadCell: `current` は書いた値、`original` はバンクの値、`kCellOverridden`、`cells` ≥ 1、`requestSeq == seq` | P §8.2 | バスの volumeDb |
| C2 | RevertValue → Ack OK、その後の ReadCell は `current == original`、overridden なし。出力のレベルも元に戻る | P §8.2 | 読み返しと BussVolume |
| C3 | 上書きされていないセルの RevertValue は SND_OK、値は変わらない | P §8.2 | |
| C4 | Revert の Revert（2 回目）も SND_OK で何も変えない。Revert の後の SetValue → Revert でまた元に戻る（覚えが消えて再び覚える） | P §8.2 | |
| C5 | 宛先の表・列が無い RevertValue は受け付けの時点で NOT_FOUND（Ack）。行が無い（存在しない runtimeId）は NOT_FOUND | P §8.2 | |
| C6 | 宛先の形が悪い ReadCell は `CellValue{result=NOT_FOUND}`（受け付けの時点）、セルが無い ReadCell は `result NOT_FOUND, cells 0` | P §8.2 | |
| C7 | ReadCell に bit0: CellValue と Ack がそれぞれちょうど 1 回、Ack の result は CellValue の result と同じ | P §8.1、CT D-86 | |
| C8 | ReadCell は書かない: 同じセルを 1000 回読んでも値と overridden が変わらない | P §8.2 | |

## D. ReplaceChunk（P §8.3）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| D1 | 行数が同じ既知の表で値だけ変える → OK、ReadCell が新しい値を返す、overridden | P §8.3 | BUSS の volumeDb |
| D2 | 行数が違う → UNSUPPORTED、値は変わらない | P §8.3 | |
| D3 | bankId のバンクが無い → NOT_FOUND | P §8.3 | |
| D4 | データ > `chunkBytes` → OUT_OF_BUDGET | P §8.3 | chunkBytes を小さく |
| D5 | 同じ列 ID が 2 回 / 列がはみ出す → INVALID_ARGUMENT、値は変わらない（一部だけ書かない） | P §8.3 | |
| D6 | 差し替えた値も RevertValue で元に戻る | P §8.3 末尾 | |
| D7 | ホットリロード中の ReplaceChunk は NOT_READY | P §8.3、§8.5 | |

## E. 試聴の全停止（P §8.6）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| E1 | 試聴を 2 つ、ゲームの再生を 1 つ鳴らして全停止 → 試聴だけが止まり（短いフェード後）、ゲームの再生は鳴り続ける | P §8.6 | `snd_get_playback_state` 相当と VoiceList |
| E2 | 止めるものが無い全停止も SND_OK（Ack） | P §8.6 | |
| E3 | 全停止の後に送った試聴は止まらない（全停止より前のものだけ） | P §8.6 | |
| E4 | `maxAuditions` を超える試聴は LIMIT_REACHED。終わると枠が戻り（`ActiveAuditions()` が 0 へ）また鳴らせる | P §8.6、R | maxAuditions = 2 |
| E5 | 切断しても試聴は止まらない | P §8.6 | |

## F. 構造の変更と予備容量（P §8.4）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| F1 | 編集器なし → UNSUPPORTED、予備容量は返る（`ReserveUsage()` が 0 に戻る） | P §8.4 | |
| F2 | 編集器あり（自前の偽物）: devReserve.buses = N のとき AddBus を N 回 OK、N+1 回目は OUT_OF_BUDGET で Error の文 "dev reserve exhausted: rebuild and hot reload"（bit0 なしのとき）。ちょうど一杯で usage = 上限 | P §8.4 | |
| F3 | bytes の残量: 64 + 8 × argCount の和で数え、ちょうど一杯は OK、1 バイト超えで OUT_OF_BUDGET | P §8.4、SE | devReserve.bytes を計算して設定 |
| F4 | 編集器が断った（NOT_FOUND）→ その結果、予備容量は返る | P §8.4 | |
| F5 | ライブで足したバスを RemoveBus → 予備容量が戻る。戻したあとにまた AddBus できる | P §8.4 | |
| F6 | 引数の規則に合わない → INVALID_ARGUMENT、予備容量は取らない | P §8.4 | |
| F7 | revertOnDisconnect の切断: そのセッションの構造の変更を新しい順に Undo、予備容量は 0 に戻る | P §8.4 | 偽の編集器が Undo の順を覚える |
| F8 | 予備容量は Server を作り直しても続く（新しい Server でも usage は同じ） | P §8.4 | |
| F9 | どの経路でも二重に返さない・返し忘れない: 拒否・成功・Remove・切断の戻しを混ぜた後、`ReserveUsage()` = 生きている Add の費用の和 | P §8.4、L-test.md | |
| F10 | ホットリロード中の StructEdit は NOT_READY、予備容量は取らない | P §8.5 | |

## G. ホットリロード（P §8.5）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| G1 | 成功: ReloadStatus が Unloading → Loading → Done の順、すべて `requestSeq == seq`、Done の `bank` ≠ 0、Ack SND_OK は Done の後 | P §8.5 | |
| G2 | ロードされていない bankId: Ack NOT_FOUND。`kReloadLoadIfAbsent` なら読み込んで Done | P §8.5 | |
| G3 | 読めないパス: Failed、Ack SND_ERR_IO、バンクは外れたまま（QueryBanks に無い） | P §8.5 | |
| G4 | 最中の SetValue / RevertValue / StructEdit は NOT_READY、ReadCell / QueryBanks は答える | P §8.5 | |
| G5 | 同時に 2 つ目は LIMIT_REACHED | P §8.5 | |
| G6 | リロード後、上書き層の覚えは捨てられ、ReadCell は新しいバンクの値（overridden なし） | P §8.5 | |
| G7 | 最中に切断しても読み込みは完了する（再接続後の QueryBanks にある） | P §8.5 | |
| G8 | 1 つの ReloadBank の Ack はちょうど 1 回 | CT D-86 | |

## H. Client の更新番号（P §8.8）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| H1 | `Sequences()` の各値は単調に増える（減らない） | P §8.8 | 何度も読む |
| H2 | `Copy*Since(after)`: 返す項目は番号が after より後。`after = Sequences().x` なら 0 件 | P §8.8 | |
| H3 | 取りこぼし: 履歴が押し出されたあと `first > after + 1`、`first - after - 1` = 押し出された数 | P §8.8 | historySeconds を短く |
| H4 | UI スレッドからの Sequences / Copy*Since と通信スレッドが並行しても競合しない（TSan） | C 冒頭 | |

## I. 記録と再生（P §7、D 2.5、11.3）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| I1 | 並び: 最初は Lod、次に開始時の BankMap、次に Begin、続けて Cell / Override、その後に Arena | P §7 | RecordReader で並びを検査 |
| I2 | 作成時から（コマンドの適用前に開始）→ origin FromCreation。途中から（何か適用後）→ MidRun、Cell の数 = 上書きされたセルの数、Override の数 = 生きているオーバーライドの数 | P §7 | |
| I3 | 同じ記録を 2 回再生: 決定のログが全く同じ、`OutputDigest` が同じ、`BitExactExpected()` true | D 2.5、RP | |
| I4 | 記録した実行と再生の一致: 作成時からの記録で、記録中に描いたブロックの出力のハッシュ = 再生の出力のハッシュ | RP 冒頭「the replay renders what the recorded run rendered」 | |
| I5 | 差し替え（ReplaceChunk）を含む記録: 再生で `replacedChunks` ≥ 1、`skippedCommands` 0、値が再生で反映される | P §7 | |
| I6 | 構造の変更を含む記録: 編集器なしの再生 → `kReplayStructEdits`、BitExactExpected false。編集器ありなら Apply が呼ばれる | P §7 | |
| I7 | LOD の段が 0 でない・ストールのレコードを含む記録 → `kReplayLodRecorded` / `kReplayStallRecorded`、BitExactExpected false | P §7 | 手で組んだ記録 |
| I8 | 記録中の BankMap → `bankChanges`、`kReplayBankChanges` | P §7 | 手で組んだ記録 |
| I9 | 1.0 の記録（Begin なし）→ `kReplayNoBegin`、origin 0 | RP | 手で組んだ記録 |
| I10 | `Blocks() = last - first + 1 + leadBlocks`、開始時の状態が無い記録で leadBlocks = 0 | RP | |

## J. 互換性・ファズ（P §2、§3、14 章）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| J1 | 短いペイロード（固定部より短い）は 0 埋めで読む: 1.1 の要求の短い形でも落ちない | P §3 | |
| J2 | 長いペイロード（末尾に追加）: 固定部の後ろの余りは無視。ReloadBank のパスは「最後の pathBytes バイト」 | P §3、W | |
| J3 | 未知の種別（0x001D、0x7FFF）に bit0 があっても答えない、接続は続く | P §3 | |
| J4 | 長さの嘘: StructEdit の argCount = 0xFFFFFFFF（× 8 の桁あふれ）、ReloadBank の pathBytes > 残り、BankList の count × elemBytes の桁あふれ → 落ちない、適用しない | P §2 | ASan/UBSan |
| J5 | RecordReader: 1.1 のレコードの途中切れ、LiveData の bytes の嘘、Begin の origin が範囲外 → エラー、落ちない | RF | |
| J6 | 乱数のファズ: 新しい要求と返事のフレームを壊してサーバ・クライアントに送り続けても落ちない | 14 章 | |

## K. RT 安全性と budget

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| K1 | オーディオスレッドの経路（適用、ReadCell、RevertCell、タップ、構造の変更の適用）で確保・ロックが無い | R 冒頭、VA、SE | tests/framework の RT 検出の範囲で |
| K2 | `budgetUs = 1e6 × block_size / sample_rate × load_target_percent / 100`（binary32） | P §6 | BlockTiming と BlockDetail |

## L. セッション（D-83、D-87）と背圧（D-86）を新しいメッセージに

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| L1 | 各 seq にちょうど 1 回の Ack（1.1 の要求すべて） | CT D-86 | Ack を数える |
| L2 | ReadCell / QueryBanks を Ack 付きで大量に（数千）送っても切断されない | CT D-86 | |
| L3 | 古いセッションの RevertValue・ReplaceChunk は適用しない（Server の作り直しで捨てる）、`staleEdits` に数える | CT D-87、P §8.2 | 作り直しの前に適用を止めた状態で送る（試せる範囲） |
</content>
</invoke>

## M. 再確認（受け入れ側の決定 D-170〜D-175 と差し戻しの修正。決定と公開ヘッダから書いた）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| M1 | 構造の変更は受け付けで戻しの記録の枠を 1 つ取る。入らなければ LIMIT_REACHED、予備容量を取らない、編集器を呼ばない。枠が空けばまた入る | D-172、struct_edit.hpp | maxStructLog = 2 で 3 つ目 |
| M2 | 記録の枠の不変条件: 落ち着いたとき `StructLogPlaces()` = 記録に残っている編集の数（適用されて残る Add と、バンクのものを外した Remove）、`ReserveUsage()` = 残っている Add の費用の和。断られた・編集器が断った・ライブの Add を打ち消した Remove・戻し・切断と再接続のどの経路でも二重に返さず返し忘れない。`RuntimeDrops::structLog` は 0 | D-172、runtime.hpp | 乱数の操作列と、自前で持つ模型との比較 |
| M3 | ライブで足したバスを外すと、その上のライブのエフェクトとそのバスから出るライブのセンドの費用も全部返り、記録から外れる。戻しでそれらの Undo は編集器に渡らない | D-172、補足 | 偽の編集器の呼び出しの記録 |
| M4 | 述語: `BitExactExpected()` は再生どうし（LOD・ストール・外したコマンド・構造の変更・バンクの変化で false）、`ReproducesRecordedRun()` はそれに加えて lostState・Begin なしで false。MidRun は情報だけ。開始時に生きていた再生は lostState に数える | D-175 | 手で組んだ記録の組み合わせの表、本物の記録（開始時に鳴っている音あり／なし） |
| M5 | `Finished()` は Start の後の `Blocks()` 個目（先行ブロックを含む）を描いた時点で true、その前は false | D-175 | leadBlocks 0 と 1 |
| M6 | Client の保持数: ちょうど上限なら全部取れて `*Lost` 0、1 つ超えると最も古い 1 つが消えて `*Lost` 1、取ったあとの答えは押し出されても数えない | D-170、client.hpp | CellValue 256 / 257、Ack 1024 / 1025 |
| M7 | 同じ runtime で 2 つ目の Server の Start は LIMIT_REACHED。先のものを Stop / 破棄するとまた Start できる。Start / Stop の繰り返しと並行に呼んでも同時に 2 つ動かない | D-170、server.hpp | 順と並行（スレッド） |
| M8 | 別のバンクのファイルの ReloadBank は Failed（INVALID_ARGUMENT）、読んだ別のバンクは残らない、要求したバンクは外れたまま。その後の QueryBanks、正しいファイルでの次の ReloadBank（loadIfAbsent）、編集が動く | D-173 | |
| M9 | 決定で決まった振る舞い: 試聴の Ack は記録の結果（存在しない Event も OK、枠は戻る）、リリースバンクと共有するオブジェクトの SetValue は全体が UNSUPPORTED、列 0 本の ReplaceChunk は OK、RemoveBus / RemoveEffect は Add の費用を全部返す | D-171、D-172、D-173 | |
| M10 | 新しい経路のファズ: 記録の枠が一杯の状態で壊れた StructEdit を流しても枠・予備容量が漏れない | D-172、14 章 | |
