# セッション C 組 P 独立テスト（live-indep）メモ

組 P（ツール側のライブ機能・プレビューエンジンと試聴・記録と再生）の独立テスト担当のメモ。ブランチ `c/live`（ローカルコミットのみ）。
性質の一覧は実装を読む前に書いた `tool/test/indep_s7_live_properties.md`（58fda89）。実装・実装担当のテスト・既存のテストは変えていない。
足したもの: `tool/test/indep_s7_live_*`（下の表）と `tool/test/CMakeLists.txt` への登録だけ。

## 作ったもの

| ファイル | 内容 |
| --- | --- |
| `indep_s7_live_properties.md` | 性質の一覧（A 照合と選択、B 編集のライブ反映、C ホットリロードと予備容量、D プレビュー、E 記録と再生、F スレッド） |
| `indep_s7_live_fixture.hpp` | 自前のプロジェクト（2 バンク Main / Aux、DC の素材で解析的なレベル）、プレビューエンジン＋Connection＋LiveSync の `Rig`（ゲーム側だけ別ビルドにできる）、ReadCell / QueryBanks でゲームの値を読む補助、文書のビルドの全 live セル（`LocateField`）、音楽のプロジェクト（3/4・90 BPM）と `Engine`、手で動かすゲーム `MiniGame`（minor・Hello の結果・BankList・Ack の結果と保留を決められる）、何でも OK の構造の編集器 |
| `indep_s7_live_sync_test.cpp` | A・B・C の性質（20 本） |
| `indep_s7_live_preview_test.cpp` | D の性質（6 本） |
| `indep_s7_live_replay_test.cpp` | E の性質（9 本）。sonus-replay の終了コードのテスト用に不具合報告のフォルダを残す |
| `indep_s7_live_threads_test.cpp` | F の性質（1 本、TSan / Release で 20 回） |
| `indep_s7_live_revert_test.cpp` | 項目単位の Revert と読み返し（Session）、プロファイラの CPU の budget と負荷（2 本） |
| `indep_s7_live_defects_test.cpp` | `Defect_*` 9 本（CTest は 1 本ずつ、実行ファイルそのものは DISABLED） |
| `indep_s7_live_hang_test.cpp` | 巨大なブロック番号の記録（fork で子プロセスに再生させ、20 秒で打ち切る。POSIX のみ）。`Defect_*` 1 本と基準の 1 本 |
| `indep_s7_live_exit.cmake` | sonus-replay の終了コードを確かめる CTest の補助 |

CTest の名前: `tool_indep_s7_live_{sync,preview,replay,threads,revert}_test`、`tool_indep_s7_live_Defect_*`（10 本）、`tool_indep_s7_live_cli_{ok,mismatch,broken,missing,usage,no_recording}`、`tool_indep_s7_live_hang_small`。

## 性質の一覧と結果

| # | 性質 | テスト | 結果 |
| --- | --- | --- | --- |
| A1 | 一致なら質問なしで Live、送信 0、全バンク Same | `IndepS7Live.MatchingBuildIsLiveAndSendsNothing` | 通過 |
| A2 / A5 | 不一致は決めるまで何も送らない（待つ間の編集も）、拒否で何も変わらない・以後も送らない・2 度目の答えは断る | `MismatchSendsNothingUntilAnsweredAndRefuseChangesNothing` | 通過 |
| A3 / A4 | バンクごとの判定（Differs / ToolOnly / GameOnly、質問中と送った後）、差分をすべて送るとゲームの全 live セル（ReadCell）が文書のビルドと一致、ツールだけのバンクが読み込まれる | `VerdictsPerBankAndSendDiffsMakesTheGameTheDocument` | 通過 |
| A6 | 1.0 のゲームの不一致: バンクの一覧を待たずに聞く、1.1 の機能は理由つきで無効、1.1 の要求を送らない、SetValue は届く | `OneZeroGameMismatchAsksWithoutABankList` | 通過 |
| A7 / B8 | 切断中の編集（Undo を含む）が再接続のあと届く（ゲームが切断で戻す場合） | `EditsWhileOfflineReachTheGameAfterReconnect` | 通過 |
| A7 | 同上、ゲームが上書きを残す場合（11.2 の既定） | `Defect_ReconnectKeepsCellsTheToolPutBackWhileOffline` | **欠陥** |
| A8 | 再接続での不一致の答え | `Record_MismatchAnswerIsRememberedOnReconnect` | 記録（覚えている）。仕様の穴 G2 |
| B1 / B2 / B11 | 値の編集がレベル（-6.0206 dB で 0.5 倍）と ReadCell に、Undo / Redo、保存で送信なし | `ValueEditUndoRedoReachTheGame` | 通過 |
| B3 | グループ 50 手の最後の値、1 回の Undo で戻る | `GroupSendsTheLastValueAndOneUndoGoesBack` | 通過 |
| B4 | 間引き（200 手 → 送信 < 200）、FlushNow なしでも最後の値が届く | `ThinningKeepsTheLastValue` | 通過 |
| B5 | イベントの追加 → リロードでゲームの Main の contentHash が文書のビルドと一致、新しいイベントが鳴る | `NewEventReachesTheGameByReload` | 通過 |
| B6 / C3 | 編集器なし: バス・エフェクトの追加はリロード（問題なし）。編集器あり: エフェクトの追加は StructEdit、予備容量 effects 1・bytes 64 + 8 × 引数、UndoStructEdit で 0、2 度目・範囲外は false、限度の既定 16 / 32 / 1 MiB | `StructuralEditsTakeTheReserveAndUndoGivesItBack` | 通過 |
| B6 | バスの追加は編集器があってもリロード | `Record_AddedBusGoesByReloadEvenWithAnEditor` | 記録。仕様の穴 G3 |
| C3 | 文書の Undo でライブのエフェクトを外したときの予備容量 | `Defect_ReserveStaysTakenAfterDocumentUndoOfALiveEffect` | **欠陥** |
| B7 | 断られた値は問題に（revision・GUID・メンバ・経路・結果）、項目は Failed | `RefusedValueIsAProblemWithItsEdit` | 通過 |
| B9 | ミキサーが持つセルは送らない、離して Resync で届く、ほかのセルは届く | `MixerHeldCellWaitsUntilResync` | 通過 |
| B10 | 固定シードの乱数 160 手（値 4 種、Undo、Redo、グループ）の後、ゲームの全 live セル = 文書のビルド | `RandomEditSequenceEndsWithTheGameEqualToTheDocument` | 通過 |
| B12 | NaN / Inf は文書が断り送らない、同じ値は送信なし、1e6 dB は届くか問題に出る、その後の正常な値は届く | `NonFiniteAndUnbuildableValues` | 通過 |
| B13 | 未対応（PHYS / XPCH）を文書の編集から一覧に | — | 未検証（マテリアル表・パッチのオブジェクトを作る手間を省いた。B14 の判定表だけ） |
| B14 | `LiveSupportOf` の表（blocking は必ず unsupported、unsupported には理由） | `LiveSupportTable` | 通過 |
| — | 断られた古い値の送り直しが新しい値を追い越さない | `Defect_ResentBusyValueOvertakesANewerValue` | **欠陥** |
| — | ライブで足したエフェクトのパラメータの編集 | `Record_ValueEditOfALiveAddedEffect` | 記録（NOT_FOUND の問題として出る）。仕様の穴 G7 |
| C1 | リロード中の 2 バンクへの値の編集が順序どおり（最後の値）、問題なし | `EditsDuringAReloadKeepTheirOrder` | 通過 |
| C2 | ゲームで失敗したリロードの後の phase | `Defect_PhaseIsLiveAfterAFailedReload` | **欠陥** |
| C2 | 書き出しに失敗したリロードの phase と理由 | `Defect_ReloadWriterFailureIsNotAFailedPhase` | **欠陥** |
| C4 | 書き出し先が無いと要るリロードは理由つきで一覧、features のホットリロードは理由つきで無効 | `ReloadWithoutWriterIsListed` | 通過 |
| D1 | バスの音量でちょうど dB 倍、2 音の和、中央は左右同じ、絶対値 = DC × バス × 1/√2、全停止の数と 2 度目は 0 | `IndepS7LivePreview.BusVolumesScaleTheOutputExactly` | 通過 |
| D2 | コーン（内 60°・外 120°・-12 dB）とリスナーの向き、6 点の経路で毎回 SpatialPreview と 2e-5 で一致 | `ConeAndListenerYawFollowTheModel` | 通過 |
| D4 | 小節の途中の要求（0.3 / 1.7 / 3.5 s）は MusicEditor::Preview のサンプルで切り替わり、レベルが 3 倍 | `TransitionSwitchesAtTheComputedSample` | 通過 |
| D4 | 小節線ちょうど・直前の要求 | `Defect_TransitionAuditionNearABarLineDisagreesWithThePreview` | **欠陥** |
| D4 | 時刻 0 の要求 | `Defect_TransitionAuditionAtTimeZeroSkipsTheFromState` | **欠陥** |
| D5 | 作って壊す 20 回（Manual / Silent、鳴ったまま、接続あり、2 度目の LoadBuild） | `CreateAndDestroyRepeatedly` | 通過（ASan の結果は下） |
| D6 | Render(0)、知らないハンドル、2D の MoveEmitter、0 の Stop、NaN / Inf の位置・リスナー・パラメータで出力は有限、エミッタの枠を超える Play、全停止で枠が戻る | `EdgeValuesAreRefusedOrHarmless` | 通過 |
| D7 | 文書の編集がプレビューの音に効く | `ValueEditUndoRedoReachTheGame`（ゲーム = プレビューエンジン） | 通過 |
| D8 | 宛先なしの router は理由つきで断る | `RouterWithoutDestinationSaysWhy` | 通過 |
| D | スナップショット・ダッキングのレベル | — | 未検証（プレビューエンジンにスナップショットを動かす口が無い。ゲームの API を直接呼ぶ必要があり省いた） |
| E1 | 保存と読み込みの往復（バイト一致）、2 回の再生で決定とビットが一致、digest は記録したブロック × フレーム × チャンネル、extraBlocks で digest は変わらない | `IndepS7LiveReplay.RecordSaveLoadReplayTwiceAgree` | 通過 |
| E2 / F3 | 3 スレッド同時の再生が単独と同じ | `ParallelReplaysAgree` | 通過 |
| E3 | CompareRuns（違う項目の番号、前方一致、digest 違い、2.5 の外は理由つきで ok、失敗した再生は ok でない） | `CompareRunsByHand` | 通過 |
| E4 | 期待のファイルの往復（u64 の端、空）、壊れた 8 種を断る、CheckExpectation の不一致 | `ExpectationRoundTripAndMismatch` | 通過 |
| E5 | Config の全メンバを変えた往復 | `ConfigTextRoundTripsEveryMember` | 通過 |
| E5 | 負の数・型に入らない数 | `Defect_ConfigLineAcceptsNegativeAndOutOfRangeIntegers` | **欠陥** |
| E6 | 不具合報告の往復（記録、バンク、Config、版、期待）、読み戻して再生 ok、題の 2 行目は落とす、`../` のバンク名は書かない | `BugReportRoundTripReplays` | 通過 |
| E6 | notes・比較の文・要約の往復 | `Defect_BugReportRoundTripLosesNotesAndComparison` | **欠陥** |
| E7 | 切り詰め（全長さ）、recordBytes・headerBytes の嘘、ビット反転 200 回、壊れた report.txt 10 種、切れた記録・無いバンク、短いバンク、バンクなし・壊れたバンクでの再生 | `BrokenFilesAreRefused` | 通過 |
| E7 | 巨大なブロック番号（1e15）の記録の再生 | `Defect_ReplayOfAHugeBlockNumberDoesNotEnd` | **欠陥** |
| E8 | 変異させたレコード列と任意のバイト列の DescribeRecording / Summarize | `DescribeArbitraryBytes` | 通過 |
| E9 | WAV: RIFF / fmt（float 32、チャンネル、レート）/ data がサンプルそのもの、半端なフレームは落とす、0 チャンネル・0 レート・書けない場所は false | `WavFileIsExact` | 通過 |
| E10 | sonus-replay: 一致 0、期待と不一致 1、壊れた報告 2、無いフォルダ 2、引数不足 2、無い記録 2 | `tool_indep_s7_live_cli_*` | 通過 |
| — | Session::RevertOnly でゲームの上書きが消える（ReadCell の kCellOverridden が 0、値はバンクの値）、2 度目は false、Undo で上書き中に戻る、ReadBack で全セルの read* が埋まる | `IndepS7LiveRevert.RevertOnlyForgetsTheOverrideAndUndoSetsItAgain` | 通過 |
| — | D-174: BlockTiming / BlockDetail の budget = 1e6 × block / rate × load_target / 100、load = (logic + render) / budget、BlockDetail は 1.1 で利用可、取りこぼし 0、メモリの合計 > 0 | `IndepS7LiveRevert.ProfilerCpuBudgetAndLoad` | 通過 |
| F1 / F2 | 実時間（Silent）のエンジン＋ライブリンク＋ホットリロード＋切断の中で UI スレッドが編集・Undo・試聴・移動・全停止・読み出し、別スレッドが RecentOutput / connector()、別スレッドが再生を繰り返す。最後に Live、出力は有限、再生は毎回同じ digest | `IndepS7LiveThreads.OwnerEditsWhileAudioReaderAndReplayRun` | 通過（TSan / Release の 20 回は下） |

## 欠陥の一覧

再現はどれも `ctest -R tool_indep_s7_live_<名前>`（実行ファイル `tool_indep_s7_live_defects_test --filter <名前>`）。

| テスト | 期待と実際 | 根拠 | 重さ |
| --- | --- | --- | --- |
| `Defect_ReconnectKeepsCellsTheToolPutBackWhileOffline` | 接続中に Sfx を -9 dB → 切断（ゲームは revertOnDisconnect なし）→ 文書を Undo（バンクの値 -1）→ 再接続。期待: ゲームの Sfx = -1。実際: -9 のまま。LiveSync は「切断で上書きは消えた」と仮定し、ゲームのバンクのファイルの contentHash と文書のビルドを比べるだけなので、差が無く何も送らない（phase は Live） | 設計 11.2「切断時: 上書きした値はそのまま残る」、live_sync.hpp 1「the document's unsaved state is sent again」、P-impl 1 再接続 | 高（ゲームの音が文書と違うまま Live と表示） |
| `Defect_ResentBusyValueOvertakesANewerValue` | 同じセルに -5（受け付けで COMMAND_BUFFER_FULL）→ -8（OK）の順の答え。期待: ゲームの最後の値 -8。実際: LiveSync が -5 を列の先頭に戻して送り直し、-8 の後に -5 が適用される | D-86（受け付けの拒否の Ack は先に届くことがある）、P-impl 2「最後の値は必ず届く」 | 中（負荷が高い時だけ。最後の手が負ける） |
| `Defect_ReserveStaysTakenAfterDocumentUndoOfALiveEffect` | 編集器ありでエフェクトを足す（AddEffect OK、effects 1）→ 文書の Undo（RemoveEffect を送り OK）。期待: reserve() 0。実際: effects 1・bytes 136 のまま（足した手の applied が残り、Remove は費用 0 の別の行） | §8.4「最後のエフェクトを外すと Add の費用を全部返す」、live_sync.hpp 3 | 中（予備容量の棒が嘘を出し続ける） |
| `Defect_PhaseIsLiveAfterAFailedReload` | ゲームが読めないパスでリロード → Failed・detached。期待: phase は Live でない（ゲームは文書のビルドを持たない）。実際: Live | live_sync.hpp「Live: the game holds the document's build」 | 中 |
| `Defect_ReloadWriterFailureIsNotAFailedPhase` | 書き出し（writer）が "disk full" で失敗。期待: reloads() の phase Failed、reason に失敗の理由。実際: finished・result IO だが phase は Unloading のまま、reason はリロードに回した理由 | live_sync.hpp 3「reloads() shows the phases ... Failed」 | 低 |
| `Defect_BugReportRoundTripLosesNotesAndComparison` | WriteBugReport → ReadBugReport。期待: 同じ BugReport（notes、comparison、summary）。実際: notes と comparison は空、summary は summary.txt 全体（題・notes・比較を含む） | recording_report.hpp「It reads back into the same BugReport」 | 低〜中（読み戻した報告を書き直すと notes が消える） |
| `Defect_ConfigLineAcceptsNegativeAndOutOfRangeIntegers` | `max_banks = -1` / `4294967296` / `99999999999999999999999` を true で受け、-1 は 4294967295、範囲外は切り詰め（0 など）にする | recording_report.hpp「false: ... a malformed value」 | 低（壊れた不具合報告で巨大な容量のランタイムを作る） |
| `Defect_TransitionAuditionNearABarLineDisagreesWithThePreview` | 3/4・90 BPM（1 小節 2 s）。要求 2.000 s: MusicEditor::Preview は 4.0 s、エンジンは 2.0 s で切り替える。要求 3.999 s: Preview は 6.0 s（先読み 1 ブロック 5.33 ms を足す）、エンジンは 4.0 s。エンジン（AuditionRouter → SetMusicState(atBlock)）は要求の時刻を含むブロックの頭で要求するので、要求が最大 1 ブロック早まり、かつランタイムの先読みの扱いがモデルと違う | P-impl 4・9「遷移の試聴が計算どおりのブロックで切り替わる」、D-179（ツールの計算 = ランタイム） | 中（遷移の試聴が画面の予告と 1 小節ずれる）。モデル（組 M）とエンジン（組 P）のどちらを直すかは受け入れ側 |
| `Defect_TransitionAuditionAtTimeZeroSkipsTheFromState` | 要求 0 s: Preview は Calm で始まり 2.0 s で Fight。エンジンは最初から Fight（遷移の記録なし、Calm は鳴らない）。Play と同じブロックで SetMusicState を記録するため | P-impl 4「From の State で鳴らし、要求の時刻に To へ切り替える」 | 低〜中 |
| `Defect_ReplayOfAHugeBlockNumberDoesNotEnd` | 形の正しいレコード列（Lod + 空の Arena）でブロック番号が 1e15。期待: 有限時間で失敗か打ち切り。実際: 最後のブロックが 1e15 なら `Blocks()` 個を描き続け、最初のブロックが 1e15 なら Start の前に 1e15 ブロック描こうとして終わらない（20 秒で打ち切った）。sonus-replay も同じ | P-test「壊れたファイル（巨大な数）」、11.3（不具合報告は外から来る入力） | 中（壊れた・悪意のあるファイルでツールが固まる） |

## 仕様の穴の一覧（選択肢と推奨）

| # | 穴 | 選択肢 | 推奨 |
| --- | --- | --- | --- |
| G1 | 再接続のとき、ゲームが上書きを残しているか（revertOnDisconnect）をツールは知らない | a. 戻ったと仮定（今）／ b. 前のセッションで送ったセルを全部送り直す（CellSync の everSent と同じ）／ c. 1.1 なら ReadCell で読んで差だけ送る | b（1.0 でも効く）。1.1 では c で送る量を減らしてよい。欠陥 1 はこれで直る |
| G2 | 不一致の答えを再接続で覚えるか（案 3） | a. 同じ LiveSync の間は覚える（今）／ b. 毎回聞く／ c. 同じゲーム（runtimeBuildHash とバンクの一覧が前と同じ）なら覚え、違えば聞き直す | c。ゲームを別のビルドで起動し直したときに黙って全部送るのは危ない。答えを取り消す操作（`SetDecision(Ask)`）も画面に |
| G3 | バスの追加は StructEdit で送れるか | BUSS の行は子が先・マスターが最後なので、バスを足すと必ずマスターの行番号が動き、マスターを使うイベント（EVNT.bus は行番号）が変わる → 「バスだけの変更」にならずリロード。編集器があっても AddBus は選ばれない | 案 5 のままなら「バスの追加・削除は実際にはリロード」と文書に書く。StructEdit で送るなら、行番号の付け替えだけによる参照列の変化を構造の変更の一部として扱う（ランタイムは ID で引き直す）必要がある（C-01 と一緒に） |
| G4 | 遷移の要求が小節線ちょうど・先読みの内側に来たとき「次の小節」はどれか | a. その小節線（ランタイム）／ b. 次の小節線（MusicEditor::Preview） | ランタイムの規則を正にし（11 章「ツールで聞いた音とゲームで鳴る音を一致」）、MusicEditor::Preview（組 M）を合わせる。試聴は要求の時刻ちょうど（ブロック内のオフセット）で記録できないなら、要求を次のブロックの頭に送る（早めない）のが保守的 |
| G5 | LivePhase::Live の意味（Failed・HeldByMixer・Unsupported の項目があるとき） | a. 送るものが無ければ Live（今）／ b. 失敗・外れたバンクがあれば別の段（例 `Degraded`）か Live に印 | b。欠陥 C2 の 1 本目 |
| G6 | 再生で受ける記録の大きさ・ブロック番号の上限 | a. 上限なし（今）／ b. 最初のブロックが大きすぎればブロック番号をそろえない（blockNumbersKept = false）、Blocks() が上限（例 2^24 ≒ 24 時間分）を超えれば CORRUPT | b |
| G7 | ライブで足したオブジェクト（StructEdit、C-01 の後）のセルへの値の編集 | 今は SetValue → NOT_FOUND の問題 | 足したオブジェクトの値の編集は、core の編集器がそのセルを上書き層に作るまで（C-01）リロードに回す |
| G8 | ミキサーがセルを離したとき（Revert）文書の値を送るか（案 8） | a. 送らない（Resync か次の編集、今）／ b. 離したことを Tick で見て送る | b。a では文書とゲームが違うまま Live になる |
| G9 | 1e6 dB のような極端な値 | テストは「届くか問題に出る」だけ確かめた。文書（buildtool）の範囲検査がバスの音量に無いならゲームまで届く | 文書側（組 M）で範囲を決める（D-85 の音量 +60 dB に合わせる） |

## 実装担当の「決定の案」への賛否

1. プレビューエンジンの置き場所（`Sonus::tool_preview`、依存表の外）: **賛成**。
2. プロセス内の接続（MemoryLink、普通の Connection）: **賛成**。同じ経路で試験できた（欠陥もこの経路で見つかった）。
3. 照合（Hello のハッシュ、既定 Ask、答えを覚える）: 既定 Ask は**賛成**。答えを覚えるのは**条件付き賛成**（G2: 同じゲームのときだけ）。Hello のハッシュは Open の時点のビルドのまま再接続でも使われる（`ConnectionDesc` は Open で固定）ので、再接続の Accepted / BuildMismatch は古いビルドとの比較になる。今は QueryBanks で比べ直すので害は小さいが、1.0 のゲームではハッシュだけで照合するので、再接続のたびにハッシュを今のビルドにする口（`Connection` 側）を推奨。
4. 差分の基準: **反対（部分）**。基準をゲームのバンクのファイル（contentHash）だけにすると、前のセッションで書いた上書きが残る場合（11.2 の既定）に戻した値を送らない（欠陥 1、G1）。前のセッションで送ったセルを基準に足す。
5. 経路の順（SetValue → ReplaceChunk → StructEdit → ホットリロード）: 順そのものは**賛成**。断られたときの回し方は 2 点直す: (a) COMMAND_BUFFER_FULL などの送り直しは、同じセルの新しい値が後に積まれている・送られているなら捨てる（欠陥 2）。(b) 送り直しは回数（1000 回）でなく時間で諦める（背圧が続く間に 1000 回はすぐ尽きる）。AddBus はこのプロジェクトの形では選ばれない（G3）ことを文書に。
6. ホットリロード: **賛成**。ただし書き出しの失敗の phase（欠陥）と、失敗の後の LivePhase（欠陥、G5）。
7. 間引き（33 ms、最後の状態は次の Tick で必ず）: **賛成**。FlushNow なしで最後の値が届くことを確かめた。
8. ミキサーとの優先（監視の値が勝つ）: **賛成**。離したときに自動で送らないのは**反対**（G8）。
9. 予備容量をツールで数える: 暫定として**賛成**（プロトコルに読む手段が無い）。ただし文書の Undo での Remove が返らない（欠陥 3）、再接続（revertOnDisconnect）・ホットリロードで実際の使用量と合わなくなる。新しいセッションとリロードで structs_ を見直すこと、livelink への要望（使用量のメッセージ）を推奨。
10. 項目単位の Revert: **賛成**（RevertOnly → RevertValue で上書きが消え、Undo は SetValue で上書き中に戻ることを確かめた）。
11. プロファイラの購読: **賛成**（budget と負荷の式だけ確かめた）。
12. 試聴（ゲームでは移動・リスナーなし、遷移はブロックの粒度）: 方針は**賛成**。ブロックの粒度の丸め方（要求を含むブロックの頭 = 早める）は**反対**: 次のブロックの頭（遅らせる）か、モデル側で同じ丸めを使う（G4、欠陥 2 本）。時刻 0 の要求は Play の次のブロックに送る。
13. 再生の Config とブロック番号: Config の選び方は**賛成**。ブロック番号をそろえるために最初のブロックまで描く方式は、上限なしだと巨大な番号で終わらない（欠陥、G6）。上限を付ける。
14. 不具合報告の形（フォルダ、key = value）: 形は**賛成**。notes と比較の文が往復しない（欠陥）ので `notes.txt`・`comparison.txt` を分けるか key に入れる。Config の数値の解析は範囲を確かめる（欠陥）。
15. sonus-replay の終了コード（0 / 1 / 2）: **賛成**。不具合報告の形で再生そのものが失敗した（バンクが読めないなど）ときは今は 1（不一致）になる。入力の誤りとして 2 を推奨。
16. 未対応の判定を 1 か所（`LiveSupportOf`）: **賛成**。

## 実装担当のテストの弱い箇所

- `LiveSync.ReconnectSendsTheUnsavedStateAgain` は `revertOnDisconnect = true` のゲームだけ。11.2 の既定（上書きが残る）で、切断中に Undo した場合を試していない（欠陥 1 が隠れた）。
- `ValueEditUndoRedoAndThinningReachTheGame` は最後に `FlushNow()` を呼ぶので「間引きでも最後の値が必ず届く」を確かめていない（独立テストで FlushNow なしを確認、通過）。
- `FailedReloadIsAProblemAndTheBankIsDetached` は phase を見ていない（欠陥 C2）。書き出しの失敗（writer が false）を試していない。
- StructEdit のテストはエフェクトだけ。AddBus はこのプロジェクトの形では起きない（G3）のに README・メモは「バス・エフェクト・センドの追加と削除 → StructEdit」とだけ書いている。文書の Undo での予備容量（欠陥 3）を見ていない。
- `MixerMonitoringValueWinsOverTheDocument` は持っている間だけ。離した後（Resync、G8）を見ていない。
- 1.0 のゲームのテストは Accepted だけ。1.0 の不一致（バンクの一覧なしで聞く）を試していない（独立テストで通過）。
- 遷移の試聴は要求 1.1 s の 1 点だけ（小節の途中）。小節線・先読みの内側・時刻 0 で食い違う（欠陥 2 本）。
- `ToolReplay.RecordSaveLoadReplayTwiceAndReport` の不具合報告の往復は「summary に題が含まれる」だけを見ており、notes と比較の文が消えることを見逃した。Config の往復は 3 メンバだけ。
- 壊れた入力に、長さの嘘・巨大なブロック番号・範囲外の数が無い（欠陥 2 本）。
- 乱数の操作列（ゲーム = 文書のビルド）が無い（独立テストで通過）。

## 机上で検証した並行部分と結論

- `LiveSync`・`AuditionRouter`・`Session`・`ProfilerStreams` は UI スレッドだけが触る（状態はメンバだけ、ロックなし）。`livelink::Client` は内部の mutex で守られ、ネットワークのスレッドとは Client の関数を通してだけ共有する。`heldByMixer` / `writer` のコールバックは Tick（UI スレッド）の中で呼ばれる。→ 問題なし。
- `PreviewEngine`: 所有スレッドの関数（Play、Stop、StopAll、MoveEmitter、SetListener、SetParam、SetMusicState、Pump、Render、LoadBuild、PutRebuiltBank、Status）は core の記録関数（登録済みのスレッド）と所有スレッドだけのメンバ（playing_、scheduled_）を触る。`RecentOutput` はオーディオスレッドが書く `std::atomic<float>` の環（32 ブロック）と release の書いた数を acquire で読み、コピーの後に書いた数の差が 16 未満なら採用（書き手が書いている途中のブロックは `w..w+15` の範囲で、コピーした `w-16..w-1` と重ならない）。→ 正しい。デバイスの形が違う経路（`RenderThunk` の else）は `Keep` を呼ばないので RecentOutput が古いまま（スレッドの問題ではない、低）。
- `MemoryFs`: 全関数が mutex。ホットリロードの書き出し（所有スレッドの `Put`）と IO スレッドの読み込みが同時でも、開いたファイルは shared_ptr で持つ。→ 問題なし。
- `Replay`: 呼んだスレッドがランタイムを 1 つ作って描く。共有するのは引数（const）だけ。→ 同時に複数でよい（3 スレッドで確認）。
- `OutputPlayer`: `Start` は `Stop` で前のドライバを止めてからサンプルを入れ替え、オーディオスレッドは `at`（原子的）とサンプル（Start の後は不変）だけを読む。→ 問題なし（実デバイスでは未検証）。
- 結論: 組 P の並行部分にデータ競合は見つからなかった。TSan と Release の 20 回の結果は下。

## 回した構成と結果

ビルドは `$S/live-indep/b-*`、-j2、1 構成ずつ（終わったら消した）。「欠陥 10 本」= 上の `tool_indep_s7_live_Defect_*`（失敗するのが正しい）。

| 構成 | コミット | 実行したテスト | 結果 |
| --- | --- | --- | --- |
| 8: TOOL_CORE、gcc Debug | a4d256f | 全部（288 本、うち無効 4・スキップ 1） | 失敗 11: 欠陥 10 本と `platform_indep_audio_driver_test`（`underruns == 0` が 1、-j2 の全体実行の負荷。単独で 3 回通過。組 P の範囲外）。組 P の独立テストの通常の本はすべて通過 |
| 8 + ASan/UBSan | 9bcce25 | `tool\|livelink`（104 本） | 失敗は欠陥 10 本だけ。サニタイザの報告 0 件（欠陥のテストの中を含む） |
| 8 + TSan | 9bcce25 | `tool`（71 本） | 欠陥 10 本のほか 2 本: `tool_indep_s7_live_threads_test`（テストの不備: TSan で遅く、フレーム番号の手順が 2 秒の間に起きなかった → 201c2b4 で直した）、`tool_profiler_streams_test`（実装担当のテスト、60 秒待って満たされず。同時に走っていた欠陥のテスト（巨大なブロック番号、子プロセスが CPU を使い切る）と重なった。単独で 3 回通過、下）。ThreadSanitizer の報告 0 件 |
| 8 + TSan、20 回（-j1） | 9bcce25 | `tool_indep_s7_live_{threads,replay,sync}_test` | sync・replay は 20 回通過。threads は上の不備で失敗 |
| 8 + Release | 201c2b4 | `tool`（71 本） | 失敗は欠陥 10 本だけ |
| 8 + Release、20 回（-j1） | 201c2b4 | 同上 | threads・sync は 20 回通過。replay が 20 回のうち 1 回失敗（テストの不備: 試聴の Ack を待たずに記録を止め、試聴が記録の外に落ちた → 96a17c0 で直した） |
| 8 + clang | 201c2b4 | `tool`（71 本） | 失敗は欠陥 10 本だけ |
| 8 + TSan、20 回（-j1、再確認） | 96a17c0 | `tool_indep_s7_live_{threads,replay,sync,revert}_test` 20 回、`tool_profiler_streams_test` 単独 3 回 | 全部通過、ThreadSanitizer の報告 0 件 |
| 8 + Release、20 回（-j1、再確認） | 96a17c0 | 同上 | 全部通過 |

MinGW（`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror -Wall -Wextra -fno-exceptions -fno-rtti`）: `indep_s7_live_{sync,defects,preview,replay,threads,revert}_test.cpp`（とヘッダ）通過。`indep_s7_live_hang_test.cpp` は fork / sys/wait.h を使う POSIX 専用で対象外（CMake も `if(UNIX)`）。

回していない構成: 1・6・7・9（組 P の独立テストは `SND_BUILD_TOOL_CORE` の下にだけあり、実装を変えていないため）。96a17c0 の最終形での全体実行（構成 8 の全部、ASan、clang）は回していない（96a17c0 までの変更はテストの待ち方だけ。直したテストは TSan / Release の 20 回で確認）。

## 未検証の箇所

- ProfilerStreams はマスク・CPU の budget と負荷だけ（ストリーム、LOD、イベントログの絞り込み、取りこぼしが 0 でない場合は実装担当のテストだけ）。項目単位の Revert は RevertOnly だけ（RevertItem、1.0 のゲームでの SetValue への切り替えは実装担当のテストだけ）。
- 未対応（PHYS / XPCH）を文書の編集から一覧に出す経路（B13）。スナップショット・ダッキングの出力のレベル。
- 実デバイスのプレビュー出力と OutputPlayer（コンテナにデバイスが無い）。TCP の本物のゲーム。
- 言語バンク、依存のあるバンクのホットリロード。予備容量を超える（OUT_OF_BUDGET）StructEdit。
- Windows / macOS での実行（新しいファイルは MinGW の構文検査だけ。`indep_s7_live_hang_test.cpp` は fork を使うので POSIX だけで、MinGW の構文検査の対象外。CMake も `if(UNIX)`）。

## 再確認（差し戻しの決定 D-182〜D-185 の後、先頭 d1fc165 から）

性質は `indep_s7_live_properties.md` の R1〜R18（8372eba で、テストより先に追記）。テストは新しい `indep_s7_live_recheck_test.cpp`（14 本）と、
`indep_s7_live_defects_test.cpp` の新しい `Defect_*` 4 本。

### 更新したテスト（決定と食い違っていたもの）

| テスト | 変えた点（期待はランタイム music `transition.cpp` の規則から自分で導いた） | コミット |
| --- | --- | --- |
| `tool_indep_s7_compute_test` `IndepS7Calc.F16_TransitionPreview`（組 M の独立テスト、旧 695〜720 行） | D-184: 要求は「時刻以降で最初のブロックの頭（最低 1 ブロック）」で取り、そこから規則の時刻（その点がブロックの頭ちょうどなら含む）、遅れの規則は「ブロックの頭 + 最初の素材のプライム」より前のときだけ（先読みの 1 ブロックは足さない）。0.498 s → 適用 94 ブロック目 0.50133 s → 次の拍 1.0 s・遅れなし。遅れの規則の検査はブリッジを外し Seg/B（プリロードなしのストリーム、30 ms）で 0.47 s → 適用 0.474667 s、拍 0.5 s は 0.504667 s より前 → nextGrid 1.0 s／startLate 0.474667 + 0.030 s／cancel。0.4 s は遅れなし 0.5 s。小節線ちょうど 2.0 s → 2.0 s（前は 4.0 s）、2.001 s → 4.0 s、0 s → 適用 1 ブロック目・2.0 s | 8372eba |
| `tool_music_editor_test` `MusicEditor.MinimumLookaheadAndTransitionPreview` の 306・307 行（例外として許可） | 10 ms ブロックで 1.495 s → 適用 150 ブロック目 = 1.5 s、その拍 1.5 s を含む、ブリッジはプリロード付き（プライム 0）→ 遅れなし・1.5 s | 8372eba |
| 自分の `Record_*` 3 本 → 決定を期待値に | `AddedBusGoesByReloadEvenWithAnEditor`（G3: リロード、Done、contentHash 一致）、`MismatchAnswerIsRememberedForTheSameGame`（G2: 同じゲームは聞かない）、`ValueEditOfALiveAddedEffectGoesByReload`（G7: 問題なし、リロードで contentHash 一致） | d29a0d5 |
| sonus-replay の終了コード | 報告からの再生そのものの失敗（バンクが読めない）= 2 を追加（`tool_indep_s7_live_cli_replayfail`） | d29a0d5 |

前の `Defect_*` 10 本は全部通過（直っている）。

### 再確認の結果（`IndepS7LiveRecheck.*`、通過）

| # | テスト | 内容 |
| --- | --- | --- |
| R1 | `ReconnectMakesTheGameTheDocument` | revertOnDisconnect あり・なし、切断中の編集・Undo・Redo、Close / Open と Drop（自動再接続）の両方でゲームの全 live セル = 文書のビルド |
| R3 | `RandomEditsWithDisconnects` | 固定シード 200 手（値 4 種、Undo、Redo、Close / Open、Drop）の後、全 live セル = 文書、問題なし |
| R4 | `AnswerIsRememberedForTheSameGameOnly` | 同じバンクの一覧なら聞かない、1 つの contentHash が違えば聞き直す |
| R4 | `NoQuestionBeforeTheBankListAndOneZeroByHash` | 覚えた答えがあるとき、一覧が来るまで Checking（質問も Live もなし）、来たら同じゲームとして Live。1.0 のゲームは同じハッシュなら聞かない、違うハッシュなら聞き直す |
| R5 | `BusyResendsGiveUpByTime` | 常に COMMAND_BUFFER_FULL: 送り直しのあと giveUpMs（400 ms）を過ぎて問題に（0.35 s より後）、その後は送らない。新しい値は届き最後の SetValue が新しい値 |
| R6 | `ThrottleWithoutFlush` | throttleMs 200、Flush なしで最後の値が届く |
| R7 | `ReserveFollowsUndoRedoAndReload` | エフェクトの追加 1 → 文書の Undo 0 → Redo 1 → リロードの成功で 0 → その後の Undo でも負にならず、全セル = 文書 |
| R8 | `MixerReleaseSendsTheDocumentValue` | ミキサーが離すと Resync なしで文書の値が届き、項目は Live |
| R11 | `PhaseFailedUntilAReloadSucceeds` | ゲームで失敗 → Failed と failedBanks（理由つき）、ほかのバンクの値の編集は届くが Failed のまま、成功で Live・failedBanks 空、書き出しの失敗 → Failed、reloads() の phase Failed と「no space left」 |
| R12 | `TransitionAuditionFollowsTheRuntimeRule` | 3/4・90 BPM、要求 0 / 2.0 / 1.999 / 2.001 / 0.96（ブロックの境目ちょうど）/ 1.9973 / 3.995 / 4.0 s: 自分で計算した規則（a = max(1, ⌈t / ブロック⌉)、a の頭以降で最初の小節線）= `MusicEditor::Preview` の applySeconds と fireSeconds = エンジンの切り替えのサンプル、切り替えの前は Calm、1 ブロック後は 3 倍（時刻 0 でも Calm から鳴る） |
| R14 | `ReplayLimitsAtTheirEdges` | maxBlocks 40: 40 ブロックは再生、41 は失敗（理由つき）。maxAlignBlocks 100: 最初のブロック 100 は番号を保つ、101 は保たずに再生。1e15 の番号は既定の上限で有限時間（失敗、または番号を保たずに再生） |
| R15 | `BugReportEveryFieldRoundTrips` | 題・notes・summary・比較の文（改行・CRLF・`=`・`#`・非 ASCII・絵文字・空・5000 文字）、Config、版、記録、バンク（id 0xFFFFFFFF、内容）、結果の 4 つ、期待が往復で同じ |
| R16 | `ConfigLineRangesForEveryMember` | config.h の X マクロで全メンバ（60 余り）: 整数は最大値・0 を受け、-1・最大 + 1・2^64 以上・小数・前後の空白・`+1`・空・`0x` を断り、断っても Config は変わらない。浮動小数は 1.5 を受け、`x`・空・±1e40・`1.5x` を断る |
| R18 | `DocumentChangesDuringTheHandshake` | HelloReply を遅らせ、その間に文書を編集（1.1 と 1.0 のゲーム、Hello のハッシュに Accepted）: 質問なし、編集は SetValue で届き、ReplaceChunk / ReloadBank は送らない |
| R17 | `tool_indep_s7_live_cli_*`（7 本） | 0 一致、1 期待との不一致、2 壊れた報告・無い報告・再生の失敗・引数不足・無い記録 |

依頼の 3（実装担当の未検証）: ハンドシェイクの最中に文書が変わっても照合は正しく、1.0 / 1.1 とも Accepted の後に送るのはその後の編集（SetValue）だけで、
質問の要る差分を黙って送ることは無かった（R18、欠陥なし）。

### 新しい欠陥（`tool_indep_s7_live_<名前>`、失敗のまま）

| テスト | 再現・期待と実際 | 根拠 | 重さ |
| --- | --- | --- | --- |
| `Defect_ValueEditBuiltWithAStructuralEditIsLost` | 編集器あり。Amb にエフェクトを足し、次の Tick の前に Sfx の音量を -6 dB に（1 回のビルドに両方）。StructEdit は送られるが音量は送られず、ゲームの Sfx は -1 のまま、phase は Live。音量のセルが構造の計画に回った BUSS の表の中にあり、値として送られない（Settle を挟めば届く） | D-183（経路の順の後もゲーム = 文書のビルド）、live_sync.hpp の Live の定義 | 高（ドラッグ中などに黙って値が落ちる） |
| `Defect_ReconnectSendsALiveStructuralEditAgain` | 編集器あり、エフェクトをライブで足す → 切断 → 再接続。再接続で AddEffect をもう一度送る: revertOnDisconnect なしでは編集器が Undo なしに 2 回目の Add を受け（ゲームに 2 個）、あり・なしとも reserve() の effects が 2 | D-182（ゲームの状態を前提にしない）・D-183（再接続のあと数え直す） | 中（C-01 の後は実際に構造が二重になる） |
| `Defect_PreviewLoadBuildAfterAHotReloadFails` | プレビューエンジンにつないでイベントを足す（ホットリロード成功）→ `LoadBuild(別のビルド)` が「the bank build/2/Main.sndb did not load」で失敗。リロードで入れ替わったバンクはエンジンが持つハンドルに無く、外されずに残る。差分を送る（不一致・知らないビルド → リロード）の後も同じ | preview.hpp `LoadBuild`「in place of the banks loaded before」 | 中（ライブの編集の後にプレビューを作り直せない） |
| `Defect_TransitionPreviewUsesTheTargetsLeadInsteadOfTheBridges` | 規則にメモリ上のブリッジ、行き先の素材はプリロードなしのストリーム。要求 1.99 s: エンジン（ランタイム）は 2.0 s で切り替える（`FirstMaterialPrime` はブリッジのプライム = 0）が、`MusicEditor::Preview` は行き先のプライム 30 ms で遅れと判断し 4.0 s | D-184（モデルはランタイムと同じ先読み）、music `transition.cpp` | 中（遷移の予告が 1 小節ずれる。組 M のモデル） |

| `Defect_RememberedAnswerGoesToAnotherGameWhenTheHelloArrivesAfterPoll` | 1.0 のゲーム（不一致、ハッシュ 0x2222）に「差分をすべて送る」と答えた後、別のビルドのゲーム（0x3333）が代わりに立つ。UI スレッドが `Connection::Poll` した後、`LiveSync::Tick` の前に HelloReply が届くと、`awaitingSync()`（クライアントの今の状態）は真なのに `hello()` は Poll が取った前のセッションの返事のままで、LiveSync は前のゲームのハッシュで「同じゲーム」と判断し、質問せずに SyncDecision（差分をすべて送る）を送る。テストは HelloReply を Poll の後まで遅らせて順序を固定（毎回再現）。`IndepS7LiveRecheck.NoQuestionBeforeTheBankListAndOneZeroByHash` が TSan の 20 回で 1 回落ちた原因 | D-182（答えを覚えるのは同じゲームのときだけ）、connection.hpp（Poll の後に読む） | 中（別のゲームに黙って全部送る。通信のスレッドとの競合で稀） |

`IndepS7LiveRecheck.NoQuestionBeforeTheBankListAndOneZeroByHash` の 1.0 の後半はこの欠陥のため稀に落ちる（期待は緩めていない）。

### 残る仕様の穴

- G10（新）: 覚えた答えが無い初めての不一致では、バンクの一覧が来る前に質問が出る（check() の判定は後から埋まる）。header どおりだが、
  判定の無い質問になる。推奨: 1.1 のゲームでは初めての質問も一覧が来るまで Checking（一覧が来ない 1.0 はすぐ聞く）。
- G11（新）: プレビューエンジンのストリームの実際のプライムは `MusicSettings::streamPrimeMs` と別（プリロードなしのストリームで、
  要求 1.96 s をモデルは 2.0 s、エンジンは 4.0 s）。D-179f のとおり設定値で受けるが、プレビューでの試聴とモデルの予告を合わせる口
  （エンジンが使うプライムを読む、または設定する）が無い。
- 再接続でゲームに構造の変更が残っているかは分からない（欠陥 `ReconnectSendsALiveStructuralEditAgain` の直し方: 再接続ではライブの
  構造の変更を送り直さず、そのバンクをリロードする、が保守的）。
- G9 は変更なし（決定どおり）。G1〜G8 は決定どおりに直っている（上の通過したテスト）。

### 実装担当のテストの弱い箇所（再確認）

- `tool_profiler_streams_test` の `DetailViewsFillFromTheGame` が全体実行（-j2）で 60 秒待って落ちた（今回の構成 8 と前回の TSan で 1 回ずつ、
  単独では 5 回通過）。マスクを購読する前に `Play(Blip)` しており、負荷が高いと Blip の終わり（イベントログ）が購読の前に過ぎて
  二度と届かない（テストの順序の不備、実装の欠陥ではない見込み）。購読（subscribedMask が付く）を待ってから Play するのがよい。
- 構造の変更と値の編集を 1 回のビルドに入れるテストが無い（欠陥 `ValueEditBuiltWithAStructuralEditIsLost`）。再接続のテストに
  ライブの構造の変更が無い。プレビューエンジンの LoadBuild をリロードの後に呼ぶテストが無い。ブリッジのある規則の先読みのテストが無い。

### 再確認の構成と結果

-j2、1 構成ずつ、ビルドは `$S/live-indep/b-*`（全部消した）。「新しい欠陥」= 上の `Defect_*`（5 本目の競合の欠陥は fe907ec で足したので、それより前の構成では未登録）。

| 構成 | コミット | テスト | 結果 |
| --- | --- | --- | --- |
| 8: TOOL_CORE、gcc Debug | c2dd454 | 全部（295 本、無効 4・スキップ 1） | 失敗は新しい欠陥 4 本と `tool_profiler_streams_test`（実装担当のテストの順序の不備、上。単独で 5 回通過）。食い違っていた 2 本（F16、music_editor_test）と前の欠陥 10 本は通過 |
| 8 + ASan/UBSan | c2dd454 | `tool\|livelink`（110 本） | 失敗は新しい欠陥 4 本だけ。サニタイザの報告 0 件 |
| 8 + TSan | c2dd454 | `tool`（77 本）、並行性 5 本（`tool_indep_s7_live_{threads,replay,sync,recheck}_test`、`tool_live_concurrency_test`）を -j1 で 20 回 | `tool` は新しい欠陥 4 本だけ。20 回: recheck が 3 回目に 1 回失敗（上の競合の欠陥）、ほかは通過。ThreadSanitizer の報告 0 件 |
| 8 + Release | c2dd454（テストの .cpp は fe907ec） | `tool`（77 本）、同じ 5 本を 20 回 | `tool` は新しい欠陥 4 本だけ。20 回: 全通過 |
| 8 + clang | fe907ec | `tool`（78 本） | 失敗は新しい欠陥 5 本だけ（競合の欠陥は毎回再現） |

MinGW（`-std=c++20 -fsyntax-only -Werror -Wall -Wextra -fno-exceptions -fno-rtti`）: `indep_s7_live_{recheck,defects,sync,replay}_test.cpp`、
`indep_s7_compute_test.cpp`、`music_editor_test.cpp` 通過。

回していない: 構成 1・6・7・9（変えたのはテストだけ。組 U の画面は実装担当が 9 で確認済み）。fe907ec での構成 8 の全体実行・ASan・TSan
（その後の変更は欠陥のテスト 1 本の追加だけ、clang と構文検査で確認）。

### 再確認で未検証の箇所

- ReadCell で読んで違うものだけ送る経路（D-182 の「または」の側）は、送り直しの側と区別して確かめていない（結果のゲームの状態だけを見た）。
- 1.0 のゲームでの再接続の値の送り直し（MiniGame は値を覚えない）。言語バンクを含むバンクの一覧での「同じゲーム」の判定。
- giveUpMs の既定（5000 ms）そのもの（400 ms に設定して確かめた）。
- 再生の上限の既定（1 時間）のちょうどの境目（40 / 100 ブロックに設定して確かめた）。先行ブロックのある（Begin のある）記録での maxBlocks の数え方（Blocks() は先行ブロックを含む）。
- 実デバイス、TCP の本物のゲーム、Windows / macOS（前と同じ）。
