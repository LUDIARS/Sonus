# 組 L（livelink 1.1）独立テストのメモ

ブランチ `c/livelink`（ローカルコミットだけ）。性質の一覧は `livelink/test/indep_s7_properties.md`
（実装を読む前に仕様とヘッダだけから書き、最初にコミットした: eb8ec4b）。

## 作ったもの

| ファイル | 内容 |
| --- | --- |
| `livelink/test/indep_s7_properties.md` | 性質の一覧（A〜L、根拠と確かめ方） |
| `livelink/test/indep_s7_common.hpp` | 補助: 偽の編集器 `FakeEditor`（RT 安全）、手で動かすゲーム側 `RawGame`（1.0 のゲームの模擬）、BUSS チャンク |
| `livelink/test/indep_s7_unit_test.cpp` | 契約の定数、wire の検査（StructArgs・費用・ReloadBank・セルの宛先・マスク）、手で組んだ記録の再生条件、壊れた 1.1 のレコード（15 件） |
| `livelink/test/indep_s7_live_test.cpp` | 本物の core: minor の取り決め（両方向、手で組んだ 1.0 の相手）、照合の端の規則、保留の決定とタイムアウト、Revert / ReadCell、ReplaceChunk の受け付け表、試聴の全停止と上限、構造の変更と予備容量、ホットリロード、更新番号、budget、RT、背圧、古いセッション（32 件） |
| `livelink/test/indep_s7_record_test.cpp` | 記録の並び、開始時の状態、FromCreation、2 回の再生の一致、**記録した実行と再生の出力のハッシュの一致**、差し替え（2 回・同じ鍵を含む）、構造の変更、ホットリロード（8 件） |
| `livelink/test/indep_s7_fuzz_test.cpp` | 順序の違反、短い／長いペイロード、未知の種別、長さの嘘（ゲーム側・ツール側）、乱数の変異（7 件） |
| `livelink/test/indep_s7_impl_test.cpp` | 実装を読んでの追加（構造の変更の記録が一杯、枠の共有、引数の上限、2 つのバンクのセル、上書き層の容量、始まらない試聴、別のバンクのファイルでのリロード）（8 件、うち欠陥 1） |
| `livelink/test/indep_s7_stress_test.cpp` | 並行性: ツール・UI・描画の 3 スレッド、要求の途中の再接続の嵐（2 件） |
| `livelink/test/CMakeLists.txt` | 上の 6 本の登録の追加だけ（`livelink_indep_s7_*`） |

## D-170: 既存のテスト 3 か所の更新（719de09）

```
tests/integration/livelink_override_test.cpp:249
-  SND_CHECK_EQ(s.Ack(s.client->Audition(0, nullptr, true, true)), SND_ERR_UNSUPPORTED);  // stop all: no core command
+  SND_CHECK_EQ(s.Ack(s.client->Audition(0, nullptr, true, true)), SND_OK);  // D-170（プロトコル 1.1）: 試聴の全停止は動く（§8.6）

tests/integration/indep_livelink_protocol_test.cpp:246
-    SND_CHECK_EQ(hr.protocolMinor, 0u);
+    SND_CHECK_EQ(hr.protocolMinor, ll::kProtocolMinor);  // D-170（プロトコル 1.1）: ゲームは自分の minor（1）を名乗る

tests/integration/indep_livelink_profile_record_test.cpp:349-（SkippedCommandsAreCountedExactly）
+  std::vector<std::uint64_t> liveDataKeys;  // D-170: 差し替えのデータ（RecordLiveData）の鍵
+    if (ll::IsKind(it, ll::kRecordLiveData)) liveDataKeys.push_back(it.liveData.key);
-    auto count = [&](const snd::cmd::CommandHeader& h, const std::uint8_t*) {
+    auto count = [&](const snd::cmd::CommandHeader& h, const std::uint8_t* payload) {
-        case CT::LiveReplaceChunk: ++replace; ++skip; break;
+        case CT::LiveReplaceChunk: {  // D-170: データが記録にあれば再生する（skipped に数えない）
+          ++replace; （payload の prepared を鍵にして、前に LiveData があれば skip に数えない）
```

ほかの行、ほかの既存のテストは変えていない。3 本とも gcc Debug で通過。MinGW の構文検査（依頼書の指定のフラグ）も通過
（`-Wall` を足すと `indep_livelink_protocol_test.cpp:311`・`indep_livelink_profile_record_test.cpp:339` の `%zu` で落ちるが、
どちらも D-170 と関係のない既存の行）。

## 性質の一覧と結果

番号は `indep_s7_properties.md`。結果は gcc Debug（構成ごとの結果は下）。

| # | 要約 | テスト | 結果 |
| --- | --- | --- | --- |
| A1〜A3, A9 | 版・MinorOf・ProfilerMaskOf・大きさ・IsKnown | `IndepS7Contract.*` | 通過 |
| A4 | マスクの規則（1.0 / 1.1） | `IndepS7Wire.ProfilerMaskRulesPerMinor` | 通過 |
| A5 | StructArgs の表 | `IndepS7Wire.StructArgsAddBus` / `StructArgsOtherOps` | 通過 |
| A6 | 費用 | `IndepS7Wire.StructCostOfEveryOp` | 通過 |
| A7 | ReloadBank のパス | `IndepS7Wire.ReloadBankPathRules` | 通過 |
| A8 | セルの宛先は値を見ない | `IndepS7Wire.CellAddressIgnoresValue` | 通過 |
| B1〜B3 | 1.0 のツールと 1.1 のゲーム | `IndepS7Handshake.OldToolOnNewGame` | 通過 |
| B4 | 1.1 の Client と 1.0 のゲーム（手で組んだゲーム側） | `IndepS7Handshake.NewClientOnOldGame` | 通過 |
| B5 | major の不一致 | `IndepS7Handshake.MajorMismatchIsRejected` | 通過 |
| B6, B7 | 端の規則（バンク無し、ビルド違いの 2 バンク） | `NoBankEdgeRule`, `BanksOfDifferentBuildsAlwaysMismatch` | 通過 |
| B8, B9 | 保留の決定、QueryBanks、両方の選択、タイムアウト | `DeferredSyncDecisionWithBankList`, `UndecidedMismatchTimesOut` | 通過 |
| B10 | 順序の違反 | `IndepS7Fuzz.OrderViolations` | 通過 |
| C1〜C8 | Revert / ReadCell / 端 / 1 回だけの答え / 読みは書かない | `IndepS7Cell.*` | 通過 |
| D1〜D6 | ReplaceChunk の受け付け表 | `IndepS7Replace.*` | 通過 |
| D7, F10, G4, G5 | リロード中の NOT_READY、2 つ目は LIMIT | `IndepS7Reload.EditsDuringReloadAreNotReady` | 通過 |
| E1〜E5 | 試聴の全停止（ゲームの再生は止めない、後の試聴は止めない、上限、切断） | `IndepS7Audition.*` | 通過 |
| F1〜F8 | 予備容量（ちょうど一杯、1 つ超え、断り、外す、切断の戻しの順、Server を越えて続く） | `IndepS7Struct.*` | 通過 |
| F9 | どの経路でも二重に返さない・返し忘れない | `IndepS7Struct.*`, `IndepS7Impl.*`, `IndepS7Stress.*` | **欠陥 1**（下） |
| G1〜G3, G6〜G8 | リロードの成功の順、無い／読み込むだけ、読めない、覚えを捨てる、切断中も完了、Ack 1 回 | `IndepS7Reload.*` | 通過 |
| H1〜H3 | 更新番号の単調、Since、取りこぼし（`first + n - 1 == 受け取った数`） | `IndepS7Sequences.MonotonicSinceAndMissed` | 通過 |
| H4 | UI スレッドと通信スレッド | `IndepS7Stress.ToolAndUiThreadsAgainstARenderingGame` | 通過 |
| I1, I2 | 記録の並び、開始時の状態、origin | `IndepS7Record.MidRunOrderAndStartingState`, `FromCreationOrigin` | 通過 |
| I3, I4 | 2 回の再生の一致、記録した実行との出力の一致 | `TwoReplaysAgreeAndEqualTheRecordedRun`, `MidRunWithStateEqualsTheRecordedRun` | 通過 |
| I5, I6 | 差し替え・構造の変更の再生 | `ReplaceChunkIsReplayed`, `TwoReplacementsWithTheSameKeyReplayInOrder`, `StructEditIsReportedOrReplayed` | 通過 |
| I7〜I10 | LOD / ストール / 記録中の BankMap / 1.0 の記録 / Blocks() | `IndepS7Replay.*`, `HotReloadInsideRecordingIsABankChange` | 通過 |
| J1〜J6 | 短い・長い・未知・長さの嘘・壊れたレコード・乱数 | `IndepS7Fuzz.*`, `IndepS7Replay.MalformedNewRecordsAreRejected` | 通過 |
| K1 | RT（確保・ロック・IO なし） | `IndepS7Rt.NewAudioPathsAreRtSafe` | 通過（new と libc の両方のフックが有効な gcc Debug で違反 0） |
| K2 | budget の定義（binary32 で一致） | `IndepS7Profiler.BudgetDefinitionAndBlockDetail` | 通過 |
| L1, L2 | 各 seq に 1 回、2500 件の Ack 付き要求で切断なし | `IndepS7Session.ManyAckRequestsAnsweredOnceNoDisconnect` | 通過 |
| L3 | 古いセッションの RevertValue / ReplaceChunk を適用しない | `IndepS7Session.OldSessionRevertAndReplaceAreNotApplied` | 通過（staleEdits 2） |

## 欠陥（失敗のまま残した）

### `IndepS7Impl.Defect_RemoveOfLiveBusAddedWithFullStructLogKeepsReserve`（軽〜中）

- 再現: `RuntimeDesc::maxStructLog = 2`、`dev_reserve.buses = 3`、偽の編集器。AddBus B1、B2、B3（3 つ目は構造の変更の記録が
  一杯で `RuntimeDrops::structLog = 1`）→ RemoveBus B3（SND_OK）。
- 期待: B3 はライブで足したバスなので外せば予備容量が返る（buses 2）。続く AddBus B4 は入る。
- 実際: buses は 3 のまま、AddBus B4 は SND_ERR_OUT_OF_BUDGET。
- 根拠: livelink-protocol.md §8.4「RemoveBus … ライブで足したバスを外すと、その分を返す」、struct_edit.hpp
  「removing an object a live edit added gives its units back once the remove is applied」。
- 原因（runtime.cpp `ApplyStruct`）: 記録が一杯のとき「applied, but a revert cannot undo it; its reserve stays taken」として何も
  覚えず、Remove は記録の中だけを探すので、その費用は二度と返らない（この runtime が生きている間ずっと予備容量が減る）。
- 案: 記録に入らない Add は断る（SND_ERR_LIMIT_REACHED、予備容量を返す）か、記録とは別に「ライブで足した ID と費用」を持つ。
  前者が単純で「戻しが必ず完全」という上書き層の方針（VA: 覚えきれない編集は断る）とも揃う。

ほかに欠陥は見つからなかった。

## 仕様の穴（期待値を決め打ちせず今の振る舞いを記録した。選択肢と推奨）

1. **`Replayer::Finished()` の時点**: `Blocks()` ブロックを描いた直後は false、もう 1 ブロック描くと true（`Next` が範囲の後の
   ブロックで立てる）。ヘッダは「render at least this many」。→ 推奨: 最後の記録ブロックを描き終えた時点で true にする
   （ツールが `Blocks()` だけ描いて `Finished()` を見ると待ち続ける）。少なくとも「Blocks()+1」と書く。
2. **Client の答えの保持数が書かれていない**: CellValue は最後の 256、Ack は最後の 1024 を持ち、超えると古いものが黙って消える
   （600 件の ReadCell で取れたのは 256。`Sequences().cells` は 600）。→ 推奨: client.hpp に上限を書き、押し出された答えの数を
   `ClientStats` に出す（D-86 のゲーム側は守っている。ツール側の取りこぼし）。
3. **リリースビルドのバンクも同じオブジェクトを持つときの SetValue**: dev バンクと上書き層の無いバンクの両方が "Sfx" を持つと、
   SetValue は全体を SND_ERR_UNSUPPORTED で断る。§8.3 の ReplaceChunk（上書き層の無いバンクは UNSUPPORTED）と揃っており
   全部か無しかなのは妥当。→ 推奨: §5（SetValue）にもこの規則を書く。
4. **ReloadBank のファイルが別のバンク**: `ReloadBank(Live, "two.sndb")` は Live を外し Two を読み込んで Done / SND_OK。→ 推奨:
   読み込んだバンクの bankId・言語が要求と違えば Failed（SND_ERR_INVALID_ARGUMENT）にする（外したバンクは外れたまま、
   読み込みに失敗したときと同じ扱い）。少なくとも §8.5 に書く。
5. **列が 0 本の ReplaceChunk**: 行数が同じなら SND_OK で何も変えない（§8.3「置き換えに無い列は今の値のまま」から導ける）。
   → 現状で良い。表に一行足すと明確。
6. **存在しない Event の試聴の Ack**: SND_OK（記録した結果。鳴ったかどうかではない）。枠は漏れない（確認済み）。→ §8.6 に
   「Audition の Ack は記録の結果」と書く。
7. **ライブで足したエフェクトが付いたまま、ライブで足したバスを外す**: バスの分は返り、エフェクトの分（effects 1、bytes 80）は
   残る。切断の戻しで編集器は「外したバスの上のエフェクト」の Undo を受ける。予備容量の最終的な合計は 0 で正しい。→ 推奨:
   バスを外したとき、そのバスを対象にしたライブの編集（エフェクト、そのバスから出るセンド）も一緒に返して記録から消す。
   少なくとも IStructEditor に「対象が無い Undo が来る」ことを書く。
8. **RemoveBus で返すもの**: 単位（buses）だけでなく bytes も返している（80）。§8.4 の「その分」は両方と読める。→ 明記する。
9. **Server を同時に 2 つ**: server.hpp は「One Server per System」だが、同じ LiveRuntime に 2 つ目の Server を Start できてしまい、
   SPSC のリング（records、results など）の消費者が 2 つになる（記録を流す Server の記録から Begin などが欠けた: 試した
   段階で `kReplayNoBegin` になった）。→ 推奨: `Server::Start` が、同じ runtime で動いている Server があれば断る
   （SND_ERR_LIMIT_REACHED）。この検査が無いので、D-87 の「古いセッションの編集が記録に入る」場合の再生の一致は、契約の
   範囲内（Server を 1 つずつ）では起きないことを確かめた上で、テストから外した。
10. **Begin の origin が 1 / 2 以外**: Replayer::Create が SND_ERR_CORRUPT（保守的で良い）。

### 実装担当が判断を求めた点への意見

- **途中から始めた記録（MidRun）を警告だけにすること / `BitExactExpected()` の意味**: 2 回の再生どうしは MidRun でも一致する
  （確認）。記録した実行との一致は「再生するシステムが同じ core の状態から始める」ときだけ。テストでは、開始時に何も鳴って
  いない MidRun（セル 1 つとオーバーライド 1 つ）で記録した実行とビットまで一致した。→ 推奨: 述語を 2 つに分ける
  （`BitExactExpected()` = 再生どうしのビット一致、今の定義のまま。`MatchesRecordedRun()` = それに加えて origin が
  FromCreation、または C-04 まで MidRun なら false）。分けないなら、今の定義（再生どうし）をヘッダに明記する。
- **ホットリロードでそのバンクの音を止めること**: 賛成（設計書 10.3 の STOP_AND_UNLOAD と同じ。鳴ったままは C-03）。
- **記録の新しいレコード種別を定数にしたこと**: 賛成（`-Wswitch` を壊さない。`RecordItem::kind` に列挙外の値が入ることは
  `IsKind` で扱える）。
- **予備容量の返し方**: 受け付けでの断り（規則・予備・枠・NOT_READY）、編集器の断り、Server の破棄での取り消し、古いセッション、
  Remove、切断の戻し、要求の途中の再接続の嵐、のどれでも二重に返さず返し忘れないことを確かめた。例外が上の欠陥 1 件
  （記録が一杯）と穴 7（バスと一緒にエフェクトを返さない）。
- **`api_indep_runtime_test`**: gcc Debug の全件（9d6333a + 8915416 の後、222 件）で通過。落ちなかったので出力は無い。

### 「決定の案」1〜14 への賛否

| 案 | 賛否 | 理由・補足 |
| --- | --- | --- |
| 1 minor の取り決め | 賛成 | 両方向を手で組んだ相手で確認（1.0 のゲームには 1.1 の種別が 1 つも届かず、マスクは 0x7F に絞られる） |
| 2 照合の端の規則 | 賛成 | バンク無し・ビルド違い・言語・リリースバンクを確認 |
| 3 RevertValue / ReadCell の印 | 賛成 | ただし穴 2（ツール側の保持数） |
| 4 試聴の全停止 | 賛成 | 「全停止より前の試聴だけ」を同じ束の送信で確認。穴 6 |
| 5 構造の変更 | 条件付き賛成 | 欠陥 1 件と穴 7・8 を直す／書く |
| 6 ホットリロード | 条件付き賛成 | 穴 4（別のバンクのファイル）を決める |
| 7 ReplaceChunk の表 | 賛成 | 参照の列の変更・ブロブ・要素の大きさ・境界・重複・リリースバンク・枠を確認。穴 5 |
| 8 budget の定義 | 賛成 | BlockTiming と BlockDetail の両方で binary32 の値が一致 |
| 9 レコード種別を定数に | 賛成 | — |
| 10 開始時の状態の先行ブロック | 賛成 | 記録した実行とビット一致を確認 |
| 11 作成時からの判定 | 賛成 | record_commands + sink で FromCreation を確認 |
| 12 再生での LOD・ストール | 条件付き賛成 | 報告は手で組んだ記録で確認。BitExactExpected の意味は上の意見（分けるか明記） |
| 13 決定のログ | 賛成 | 2 回の再生で全項目が一致。穴 1（Finished の時点で範囲の後のブロックはログに入らない） |
| 14 更新番号 | 賛成 | `first + n - 1 == 受け取った数`、窓、容量より少ない capacity を確認 |

## 実装担当のテストの弱い箇所

- 構造の変更の記録が一杯（`maxStructLog`）、枠（`chunkSlots`）を ReplaceChunk と StructEdit で取り合う場合、
  argCount の上限（65 以上は Client が送らないので SendRaw が要る）を通っていない。欠陥 1 件はここにあった。
- 1.1 の Client の答えの保持数を超える量（穴 2）を試していない。
- `Replayer::Finished()` を `Blocks()` 直後に見ていない（穴 1）。
- 再生と**記録した実行**の出力の一致は確かめているが（`Differences`）、同じ枠（同じ鍵）を 2 回通った差し替えの再生は無い
  （こちらで足して通過）。
- `UpdateNumbersTellNewAndMissedEntries` は `n >= seen` など下限だけの検査がある（取りこぼしの数そのものは固定していない）。
- ファズの再生の検査は `res == OK || CORRUPT || ...`（落ちないことの確認としては十分）。

## 机上で検証した並行部分

- **予備容量**（`ReserveStruct` / `ReleaseStruct`）: 取るのはライブリンクのスレッドだけ（検査してから加算。取る側が 1 つなので
  超過しない）、返すのはオーディオスレッド（適用されなかった、Remove、戻し）とライブリンクのスレッド（受け付けでの断り）。
  どれも atomic の fetch_add / fetch_sub で、同じ PendingChunk の費用は「キューに入れた後はオーディオスレッドだけが返す／
  入れる前はライブリンクのスレッドだけが返す」に分かれていて二重にならない。ただし Server を同時に 2 つ動かすと取る側が 2 つに
  なり検査と加算の間で超過しうる（穴 9 の検査があれば起きない）。
- **試聴の枠**（`auditionsActive_`）: CAS で上限を守る。返すのはオーディオスレッド（OnEnded で追跡中のハンドルを見つけたとき）
  とライブリンクのスレッド（記録できなかったとき）。始まらなかった試聴でも枠が戻ることを確認。
- **Client**: 公開関数はすべて 1 つの mutex の中（client.hpp）。UI スレッドの `Sequences` / `Copy*Since` / `CopyBanks` などと
  通信スレッドの混在を TSan で回した（下）。
- **ReadCell**: オーディオスレッドが `CellReading` を SPSC に入れ、ライブリンクのスレッドが seq とセッションで組み合わせる。
  古いセッションの読みは INVALID_HANDLE の結果だけを返し、CellValue は出さない。
- **2 つの Server の同時実行**は契約の外（穴 9）。L3 のテストは Server を 1 つずつ作り直す形にした。

## 回した構成と結果

ビルドは `$S/livelink-indep/b-*`、-j2、1 つずつ。終わったら消した。

| 構成 | コミット | 実行 | 結果 |
| --- | --- | --- | --- |
| 1 gcc Debug | f54cf1a 前（ccc5129〜9d6333a のテスト。8915416 の再生テストの 1 件追加は単体で通過） | 全件 ctest（222） | 221 通過。失敗は `livelink_indep_s7_impl_test`（欠陥の 1 件だけ）。`platform_device_play_test` は Skipped。`api_indep_runtime_test` は通過 |
| 3 ASan/UBSan | f54cf1a | `ctest -R livelink`（32）、`SND_FUZZ_ITERATIONS=20000` | 31 通過。失敗は欠陥の 1 件だけ（サニタイザの報告なし。単体で出力を確認） |
| 4 TSan | 34f282e | `ctest -R livelink`（32）と `livelink_indep_s7_(stress\|live)` を -j1 で `--repeat until-fail:20` | 31 通過、失敗は欠陥の 1 件だけ（TSan の警告なし）。20 回の繰り返しは全通過 |
| 5 Release | 34f282e → e5b8c3d | `ctest -R livelink`（32）と並行性 2 本の 20 回 | 31 通過、失敗は欠陥の 1 件だけ。20 回の繰り返しで `IndepS7Stress.ReconnectsDuringRequests` が 1 回、最後の `StopAuditions` が 0（Ready でなく送れない）で落ちた → テストの側で送り直す形に直し（e5b8c3d）、Release で stress を 20 回: 全通過 |
| 2 clang Debug | e5b8c3d | ビルド（警告 0）と `ctest -R livelink`（32） | 失敗は `livelink_indep_s7_impl_test` だけ（-j2 のログでは失敗した SND_TEST の名前を個別に確かめていない。gcc / ASan / TSan では欠陥の 1 件だけ） |
| MinGW 構文検査 | e5b8c3d | 新しい 6 本を `-std=c++20 -fsyntax-only -Werror -Wall -Wextra -Wpedantic -Wshadow -fno-exceptions -fno-rtti` | 全部通過（std::thread を含めて。POSIX 専用のものは無い）。D-170 の 3 本は依頼書のフラグで通過 |

回していない構成: 6（テストなし Release）、7（Opus なし）、8、9（ツール入り。新しいファイルは tool/ に触れない）。
TSan は e5b8c3d の stress の変更（最後の全停止を送り直すだけ）の後には回し直していない。

## 未検証の箇所

- TCP での 1.1（全部メモリの転送路）。
- 本物のストリームを読む再生での StreamList（テストの資産は常駐。slot と state の範囲だけ見た）。
- 言語バンク（languageId ≠ 0）のホットリロード、依存元があって外せないバンク（BANK_IN_USE）。
- 開始時の状態が記録用リングに入りきらない場合（lostState が実際に立つ記録）。手で組んだ記録で報告だけ確かめた。
- 本物の LOD の変化・ストールを含む記録（core で起こせない。手で組んだ記録だけ）。
- Windows / macOS での実行（MinGW は構文検査だけ）。
- K1 の RT 検査は libc のフックが有効な構成（サニタイザなし）だけで意味がある。gcc Debug で有効を確認した。

## 再確認（受け入れ側の決定 D-170〜D-175 と実装担当の修正 6a4377e、d8e5291、10d36af、483e6e4、f61937a の後）

作業ツリーの先頭 6bde346 の上に足した。性質は `indep_s7_properties.md` の M 節（決定と公開ヘッダから、実装を読む前に書いた: 50074a3）。

### 決定に合わせて更新したテスト（56d0cd0、コメントに D-番号）

| テスト | 前 | 後 |
| --- | --- | --- |
| `IndepS7Impl.Defect_RemoveOfLiveBusAddedWithFullStructLogKeepsReserve` | 欠陥（記録が一杯でも Add を適用し予備容量が返らない） | `StructEditBeyondTheLogIsRefusedAtAcceptance`（D-172: 受け付けで LIMIT_REACHED、予備容量も編集器も使わない、ack なしなら Error、`structLog` 0）と `StructLogPlacesComeBackWithTheRevert`（戻しで枠が返り、B3 が入る） |
| `IndepS7Replay.LostStateIsReported` | lostState で BitExactExpected false | D-175: BitExactExpected true、ReproducesRecordedRun false |
| `IndepS7Impl.ReloadWithAFileOfAnotherBankIsRecordedBehaviour` | 振る舞いの記録 | `...Fails`（D-173: Ack INVALID_ARGUMENT、最後の ReloadStatus は Failed、バンクは残らない） |
| `IndepS7Record.*`（Finished） | 1 ブロック多く描いて true | D-175: Blocks()−1 個では false、Blocks() 個で true（leadBlocks 1 と 0）。ReproducesRecordedRun も確認 |
| `IndepS7Session.AnswerStoreOverflowIsRecordedBehaviour` | 振る舞いの記録 | `AnswerStoreLimitsAndLostCounts`（D-170: CellValue 256 ちょうどで全部取れて cellsLost 0、取ったあとは数えない、1 つ超えで cellsLost 1・最も古いものだけ消える、Ack 1025 で acksLost 1） |
| `IndepS7Handshake.DeferredSyncDecisionWithBankList`（穴 3） | コメントだけ | D-171: リリースバンクと共有する SetValue は UNSUPPORTED、セルは書かれない |
| `IndepS7Audition.LimitAndPlacesComeBack`、`IndepS7Impl.AuditionsThatNeverStart...`（穴 6） | 出力だけ／OK 以外も許す | D-171: 存在しない Event の試聴の Ack は SND_OK、枠は戻る |
| `IndepS7Struct.BusReserveExactlyFullOneOverAndGivenBack`（穴 8） | 出力だけ | D-172: Remove は bytes も全部返す |
| `IndepS7Impl.RemoveLiveBusWithLiveEffectThenRevert`（穴 7） | 出力だけ | D-172: エフェクトの分も返り、戻しで Undo が編集器に行かない |
| `IndepS7Replay.BeginWithUnknownOriginIsRecordedBehaviour`（穴 10） | 出力だけ | `...IsCorrupt`（origin 0 / 3 / 7 / 0xFFFF は CORRUPT） |

穴 5（列 0 本）は前から OK を期待値にしていた。2 つ目の Server（穴 9）は新しいテスト（下の M7）。
ほかに、テスト側の誤りを 1 つ直した（2f… 下の構成の節）: `IndepS7Fuzz.OrderViolations` が Hello の前の 4 つの要求を
SND_REQUIRE で送っていたが、ゲームが最初の要求で接続を閉じることがある（ASan で 1 回）。送信を必須にしない形にした。

### 再確認のテスト（`livelink/test/indep_s7_recheck_test.cpp`、10 件）と結果

| # | テスト | 結果 |
| --- | --- | --- |
| M1 | `IndepS7Impl.StructEditBeyondTheLogIsRefusedAtAcceptance`、`StructLogPlacesComeBackWithTheRevert` | 通過 |
| M2 | `IndepS7Recheck.StructLogAndReserveFollowTheModel`（4 つの種 × 120 操作: AddBus / AddEffect / AddSend（ライブのバスと Sfx）、ライブのバスの Remove、バンクのバスの Remove、壊れた編集、編集器の断り 1/8、切断と再接続（戻し）。毎回 `ReserveUsage()`・`StructLogPlaces()`・`structLog` を模型と比べ、結果も模型から予想（OK なら予算と枠があること、OUT_OF_BUDGET なら予算が無いこと、LIMIT_REACHED なら枠が無いこと）） | 通過（食い違い 0） |
| M3 | `RemovedLiveBusTakesItsEffectsAndSends`（B1 の上の 2 つのエフェクトと B1 から出るセンドが一緒に返り、B2 → B1 のセンド・Sfx から出るセンドは残る。戻しは残った 4 つだけを新しい順に Undo） | 通過 |
| M4 | `PredicateTable`（origin なし／FromCreation／MidRun × lostState × LOD × ストールの 20 行）、`PlaybackAliveAtTheStartIsLostState`（開始時に鳴っている音があると lostState ≥ 1、BitExactExpected true、ReproducesRecordedRun false。無ければ両方 true） | 通過 |
| M5 | `FinishedAtTheLastBlockWithoutLead`（leadBlocks 0: Start 前は false、2 個で false、3 個目で true、その後も true）と record の leadBlocks 1 | 通過 |
| M6 | `IndepS7Session.AnswerStoreLimitsAndLostCounts` | 通過 |
| M7 | `OneStartedServerPerRuntime`（2 つ目は LIMIT_REACHED、Start していない方の Stop では空かない、Stop・Destroy で空く、Stop 2 回、再 Start）、`ConcurrentStartsNeverOverlap`（3 スレッドが 25 回ずつ Create / Start / Stop / Destroy: 同時に動いたのは常に 1 つ、LIMIT 以外の失敗 0） | 通過 |
| M8 | `StateAfterReloadOfAnotherBanksFile`（Failed の後: バンク一覧は空、SetValue は NOT_FOUND、通常のリロードは NOT_FOUND、loadIfAbsent で読み込み直せる、そのセルはバンクの値、編集と次のリロードが動く、ack なしの失敗は Error "…another bank"） | 通過 |
| M8 | **`Defect_FailedReloadUnloadsTheGamesOwnOtherBank`** | **失敗（新しい欠陥）** |
| M10 | `MalformedEditsWithAFullLog`（記録が一杯で壊れた StructEdit 400 件: 全部に答え、枠は 2 を超えず、戻しで枠も予備容量も 0） | 通過 |

### 新しい欠陥

**`IndepS7Recheck.Defect_FailedReloadUnloadsTheGamesOwnOtherBank`**（中）
- 再現: ゲームが "Live"（a.sndb）と "Two"（two.sndb）を読み込んでいる。ツールが `ReloadBank(Live, 0, "two.sndb")`。
- 期待: Failed（INVALID_ARGUMENT）、Live は外れたまま（D-173）、**ゲーム自身が読み込んでいた Two は残る**（要求は Two を
  対象にしていない。D-173 の「読んだ別のバンクは登録しない（すでに登録していたら外す）」は、リロードが読み込んだ登録のこと）。
- 実際: Two も外れる。`ReloadBank(Two, 7, "two.sndb", loadIfAbsent)`（言語違い）でも、ゲームの (Two, 0) が外れる。
- 原因（server.cpp `PollReload`）: 同じパスがもう読み込まれていると `LoadBankAsync` はゲームの既存の登録を返し、bankId / 言語の
  不一致のときその登録を `UnloadBank` する。
- 案: `LoadBankAsync` の前にその時点の登録の一覧を覚え、返ってきたハンドルがもとからあった登録なら外さない（Failed だけ）。
  もとからあった登録と同じバンク（bankId・言語）を指す ReloadBank は、外す手順の前に Failed にできる。

ほかに新しい欠陥は見つからなかった。

### 残る仕様の穴

1. **記録が一杯のとき、ライブの Add を打ち消す Remove も断られる**（D-172「どの編集も受け付けで枠を 1 つ取る」の帰結）。
   枠を空けるには切断の戻ししかない（ツールが 1 つ外して入れ直す、ができない）。→ 推奨: 枠が一杯でも Remove は受け付けて
   オーディオスレッドで判定する（打ち消しなら枠は要らない、バンクのものを外すなら適用の時点で枠が無ければ断る）か、
   Remove 用に枠を 1 つ取っておく。少なくとも §8.4 に「一杯なら Remove も LIMIT_REACHED、戻しで空く」と書く。
2. **D-173 の「外す前のヘッダの確認」はしていない**（受け入れ側の補足で可）。そのため Failed の後は要求したバンクが外れたまま。
3. 上の欠陥の扱い（ゲーム自身の登録を外さない）を §8.5 に明記する。

### 実装担当の修正のテスト（f61937a）について

枠の取り合い、引数 65、同じ鍵の差し替え 2 回、取りこぼしの数そのものが足されている。ゲームが同じファイルを既に読み込んでいる
場合のリロードの失敗、Remove も枠を取ることは、実装担当のテストに無い。
