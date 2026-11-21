# セッション C 組 P（ツール側のライブ機能・プレビューエンジンと試聴・記録と再生）メモ

実装担当のメモ。受け入れ側への引き継ぎ。ブランチ `c/live`（ローカルコミットのみ）。

## 進み具合

- [x] 0. 既存の `tool_live_mixer_test` の不安定の調査と修正（2670d8f、下の節）
- [x] 1. 接続と照合（11.2）
- [x] 2. 文書の編集のライブ反映
- [x] 3. ホットリロードと予備容量
- [x] 4. プレビューエンジンと試聴
- [x] 5. 項目単位の Revert と読み返し
- [x] 6. プロファイラの残りのモデル
- [x] 7. 記録と再生
- [x] 8. まだつなげないものの扱い（`LiveSupportOf` の 1 か所）
- [x] 9. テスト（`tool/test` の 7 本 + CLI 2 本）
- [x] 10. 文書（`tool/README.md` の「段階5 のライブ機能・プレビュー・記録と再生（組 P）」、このメモ）
- 構成ごとの結果は「回した構成と結果」。

## 作ったもの

核（`Sonus::tool_core`、Tela なし、UI なし）:

| ファイル | 役割 |
| --- | --- |
| `tool/include/snd/tool/live_sync.hpp`, `src/live_sync.cpp`（新） | `LiveSync`: 文書（`ProjectDocument`）のライブ反映。接続時の照合（`check()`: 版、両方のビルドハッシュ、バンクごとの判定 Same / Differs / ToolOnly / GameOnly、知っているビルドか）、決定（`LiveDecision` Ask / SendDiffs / Refuse、`ChooseSendDiffs` / `ChooseRefuse`）、編集・Undo・Redo の反映（SetValue / ReplaceChunk / StructEdit / ホットリロード）、間引き（`throttleMs`）、背圧（`maxInFlight`）、送り直し、項目ごとの状態（`items()`）、問題（`problems()`）、未対応（`unsupported()`）、ホットリロードの経過（`reloads()`）、予備容量（`reserve()`、`structEdits()`、`UndoStructEdit`）、1.0 の相手の機能（`features()`）。`LiveSupportOf`（item 8 の 1 か所）、`DirectoryReloadWriter` |
| `tool/include/snd/tool/preview.hpp`, `src/preview.cpp`（新） | `IPreviewEngine`（プレビューエンジンの抽象）と `AuditionRouter`（試聴の受け口: Event、3D と移動、音楽の遷移、パラメータ・ステート、全停止。宛先はゲームかプレビュー。できないことは理由つき） |
| `tool/include/snd/tool/profiler_streams.hpp`, `src/profiler_streams.cpp`（新） | `ProfilerStreams`: CPU（BlockTiming と budget との比）、BlockDetail、メモリの区分、StreamList、ストール、LOD の段と変化、イベントログ（`LogFilter`）。`SetViews` で画面に出すものだけ購読（`ProfilerMaskFor`、1.0 の相手には 1.1 のビットを出さない）。`Copy*Since` で取りこぼしを数える（`StreamCount`） |
| `tool/include/snd/tool/recording_report.hpp`, `src/recording_report.cpp`（新） | `DescribeRecording`（1.1 のレコードを含む要約とテキスト）、`ReplayConfigFor`、`ConfigText` / `ParseConfigLine`（snd_config の全数値メンバ）、`ReplayRun` / `CompareRuns` / `ConditionReasons`、`CompareWithRecordedRun`（記録中のイベントログ・LOD・ストールとの比較）、期待（`ReplayExpectation` とファイル、`CheckExpectation`）、不具合報告（`WriteBugReport` / `ReadBugReport`） |
| `connection.hpp/.cpp`（追記） | `ConnectionDesc::deferSyncDecision`、`awaitingSync()`、`DecideSync()`、`peerMinor()`。既定の振る舞いは変えていない |
| `cell_sync.hpp/.cpp`（追記） | `RequestRevert`（1.1 の相手で値がバンクの値なら RevertValue、ほかは従来の SetValue）、`RequestReadBack`（ReadCell、64 件ずつ）、`CellStatus` の末尾に read* と revert の欄 |
| `mixer.hpp/.cpp`（追記） | `RevertOnly`（この項目だけ戻す、1 手）、`CellsOf` |
| `session.hpp/.cpp`（追記） | `RevertItem` / `RevertOnly` / `ReadBack`、`AttachDocument`（LiveSync を持つ。Connect で Prepare、Tick で Tick、問題をメッセージに）、`EnableProfilerStreams`。session.hpp は live_sync.hpp を含めない（前方宣言。`Sonus::tool_core` だけのホストのため） |
| `tool/CMakeLists.txt` | 新しいソース、`add_subdirectory(preview)`、`sonus_tool` の INTERFACE に livelink の include（recording_report.hpp が livelink の型を使う） |

プレビュー（新しいディレクトリ `tool/preview/`、`Sonus::tool_preview`、`SND_BUILD_TOOL_CORE` / `SND_BUILD_TOOL` で作る。Tela なし）:

| ファイル | 役割 |
| --- | --- |
| `preview/CMakeLists.txt` | `sonus_tool_preview`（依存表のモジュールではない。構成ルート `sonus` と同じ扱いで各モジュールをリンク）と `sonus-replay` |
| `preview/src/memory_fs.*` | メモリ上の `IFileSystem`（ビルドしたバンク、ホットリロードの書き出し先） |
| `preview/src/runtime_host.*` | ツールの中のランタイムの組み立て（D-93 の順: FS → ワーカー → コーデック → ストリームの枠と IO → `compose::SourceKinds` → [LiveRuntime か Replayer] → core → [デバイスと AudioDriver] → [Server を MemoryLink で]）。出力は手動・デバイス・無音。直近の出力はロックなし |
| `preview/include/snd/tool/preview/preview_engine.hpp`, `src/preview_engine.cpp` | `PreviewEngine`（`IPreviewEngine` の実体）: `LoadBuild`（依存順、言語バンクは読まない）、`PutRebuiltBank`、試聴（Play、MoveEmitter、SetListener、SetParam、ブロック指定の SetMusicState、Stop、StopAll）、`Render` / `Pump` / `Status` / `RecentOutput` / `Transitions` |
| `preview/include/snd/tool/preview/replay_host.hpp`, `src/replay_host.cpp` | `Replay` / `ReplayTwice` / `CheckRecording` / `CheckRecordingFiles` / `MakeBugReport` / `ReplayBugReport` / `WriteWav` / `OutputPlayer`（デバイスで聞く）、`BanksOf`、`ReadBankDirectory` |
| `preview/cli/sonus_replay.cpp` | `sonus-replay`（UI なしの入口。`tool/app` ではない） |

テスト（`tool/test`、Tela 不要。登録は `tool/test/CMakeLists.txt`）:

| ファイル | 内容 |
| --- | --- |
| `live_fixture.hpp` | 解析的な DC の素材のプロジェクト（音楽の遷移用も）、プレビューエンジン＋Connection＋LiveSync の `Rig`、構造の編集器の代役 `YesEditor` |
| `live_preview_test.cpp` | 文書から鳴らしたレベル、3D（SpatialPreview の値と 2e-5 で一致、移動）、ライブの編集が音に効く、全停止、遷移の試聴が `MusicEditor::Preview` の計算どおりのブロックとサンプルで切り替わる（レベルが 2 倍に）、ゲームへの試聴（全停止はゲームの再生を止めない） |
| `live_sync_test.cpp` | 一致、値の編集・Undo・Redo・間引き（40 手で送った数 < 40、最後の値が届く）、ReplaceChunk（列が増える）と断られたらホットリロード（EVNT.priority）、新しいオブジェクトのホットリロードと最中の編集、StructEdit（編集器あり: 予備容量、Undo。なし: ホットリロード）、不一致の質問と SendDiffs、Refuse、ホットリロードの失敗（問題・外れたバンク）、再接続で送り直し、ミキサーの監視の値の優先、未対応の一覧、Session との組み合わせ |
| `live_revert_test.cpp` | RevertValue（読み返しで上書き中でない）、Undo で SetValue、`RevertOnly` |
| `profiler_streams_test.cpp` | マスク、CPU・BlockDetail・メモリ・ストリーム・LOD・イベントログ（Play と再生の終わり） |
| `replay_test.cpp` | プレビューエンジンで記録 → 保存 → 読み込み → 2 回再生（決定とビットが一致）、記録したイベントログとの比較、期待のファイル（一致・不一致）、WAV、不具合報告の往復と再生、壊れた入力（記録、報告、期待、Config） |
| `live_v10_test.cpp` | 1.0 のゲーム（livelink のテストの `RawGame`）: 1.1 の要求を送らない、機能は理由つきで無効、SetValue は届く、ホットリロードは未対応として一覧、全停止・読み返し・Revert は 1.0 の扱い |
| `live_concurrency_test.cpp` | 実時間（無音の出力）のプレビューエンジンとライブリンクの中で UI スレッドが編集・Tick・読み出し・試聴・切断、2 スレッドで同時に再生 |
| `live_mixer_test.cpp`（既存、待ち方だけ） | 下の節 |
| CTest `tool_replay_cli` / `tool_replay_cli_bad_input` | `replay_test` が書いた不具合報告を `sonus-replay` で再生（FIXTURES）、無いフォルダは失敗 |

依頼: `spec/handoff/parallel/requests/C-20.md`、`C-21.md`。`api/compose/c_api.cpp` には追記していない。`livelink/`、`buildtool/`、
`tool/ui/`、`tool/app/`、`core/` などは変えていない。

## 既存の `tool_live_mixer_test` の不安定（原因と直した内容）

- 再現: `taskset -c 0 ctest -R tool_live_mixer_test --repeat until-fail:30` で 6 回目に失敗（`live_mixer_test.cpp:189`
  `StateSurvivesDisconnectAndReconnect`、Rel = 0.501 = -6 dB。組 M・組 L の報告と同じ箇所と値）。
- 原因: `Disconnect()` の後に 16 ブロック描いてから「ゲームが戻した」ことを確かめていた。戻し（revertOnDisconnect の
  `LiveClearOverrides` と戻しの印）はゲームのライブリンクのスレッドが切断に気づいてから記録する。テストは 16 ブロックを
  待たずに描く（実時間の待ちなし）ので、1 コアに縛る・負荷が高いと、ライブリンクのスレッドが動く前に 16 ブロックが終わる。
  実装の欠陥ではなく、テストの待ち方（時間ではなく固定のブロック数）の問題。
- 修正（2670d8f）: 「サーバが切断を見て（`!Connected()`）、レベルが戻るまで」を上限つき（60 秒）で待ってから、元の
  16 ブロックとアサーション（期待値は不変）。直後の再接続は既に条件で待っていた。
- 確認: 1 コアに縛って 50 回、同じコアに CPU を食うループを置いて 30 回、全通過。TSan / Release の 20 回は下の表。

## 決定の案（受け入れ側が D-番号を振る）

1. **プレビューエンジンの置き場所**: 抽象（`IPreviewEngine`、`AuditionRouter`）は核、実体は `tool/preview/` の別のライブラリ
   `Sonus::tool_preview`。依存表のモジュールではなく、構成ルート `sonus` と同じ「組み立ての側」として全ランタイムの
   モジュールをリンクする（`snd_add_module` を通らないので表の検査に当たらない）。C API（`Sonus::sonus`）を使わないのは、
   ライブリンクを TCP でしか起こさず（ポートを返す関数が無い）、Replayer を入れる口が無いため。D-93 の順で組み立て、
   `compose::SourceKinds` を使う。二重の組み立てをやめる口を依頼 C-20 に書いた。
2. **プロセス内の接続**: プレビューエンジンは `MemoryLink` で Server を起こし、ツールは普通の `Connection`（`connector()`）で
   つなぐ。ライブ反映・Revert・読み返し・プロファイラ・記録はゲームと同じ経路。ブロック単位の厳密さが要る試聴（音楽の
   ステートを決まったブロックで要求）と、ゲームには無い操作（エミッタの移動、リスナー）はエンジンの関数で。
3. **照合**: Hello のハッシュは文書の今のビルド（`Prepare`）。不一致の既定は Ask（決定の保留）。1.1 のゲームには QueryBanks で
   バンクごとに比べる。答えたあとは同じ `LiveSync` のうちは覚えて、再接続の不一致にも同じ答えを返す（「同じ選択で決める」の
   解釈）。ゲームの handshakeTimeoutMs（5 秒）を過ぎると切られ、再接続でまた聞く。
4. **差分の基準**: ゲームのバンクの contentHash が、ツールが知っているビルド（`SetBaseline`、送ったホットリロード、今のビルド）
   なら、そこからの `DiffBuilds` で値の単位で送る。知らないビルドなら、ホットリロードできればホットリロード、できなければ
   全部の表を ReplaceChunk し、ブロブは「確かめられない」として一覧に残す。1.0 のゲームはビルドハッシュだけで照合する。
5. **経路の順**: 値（`DiffBuilds` の ValueEdit）→ SetValue（セルごとに新しい値で置き換え）。形が同じ表 → ReplaceChunk（同時に 1 つ、
   chunkSlots の既定 1 に合わせる）。バス・エフェクト・センドの追加と削除だけの変更 → StructEdit（ほかの表に波及する変更、
   エフェクトの途中への挿入、センドの削除は対象外）。それ以外と、ReplaceChunk / StructEdit / SetValue が UNSUPPORTED・
   OUT_OF_BUDGET・（StructEdit の）LIMIT_REACHED → そのバンクのホットリロード。COMMAND_BUFFER_FULL・NOT_READY・
   （ReplaceChunk の）LIMIT_REACHED は黙って送り直す（1000 回で問題に）。ほかの失敗は問題（編集の revision、オブジェクト、
   メンバ、結果の文）。
6. **ホットリロード**: 書き出すのは送る時点の最新のビルド。そのバンクの待っている要求はリロードに含まれるので捨て、
   送った後の答えはエポックで捨てる（`superseded`）。リロード中は何も送らない（NOT_READY を利用者に見せない）。失敗は
   `reloads()` の phase Failed、`detached`（NOT_FOUND / BANK_IN_USE / LIMIT_REACHED 以外）と問題。新しいバンク（ゲームに無い）は
   `kReloadLoadIfAbsent`。バンクの削除はゲームに外す手段が無いので一覧（止めない）。
7. **間引き**: 文書の revision が変わってもビルドは `throttleMs`（既定 33 ms）に 1 回。最後の状態は間隔のあと次の Tick で
   必ずビルドして送る（`FlushNow` で即時）。保留中のビルドがある間は phase を Syncing にする。
8. **ミキサーとの優先**: 同じセルをミキサーの監視の値（`MixerModel` の値がバンクの値と違う間）が持っていれば、文書の
   ライブ反映はそのセルを送らず HeldByMixer にする（監視の値が聞こえ続ける）。ミキサーが戻しても、文書の値は自動では
   送り直さない（`Resync()` か次の編集）。文書を保存してもライブの状態は変わらない。ホストは `Project::Load(doc)` で
   ミキサーのバンクの値を文書に合わせる。
9. **予備容量**: ゲームの `DevReserveUsage` を読む手段がプロトコルに無いので、ツールが OK を受けた StructEdit から数える
   （限度は `LiveSyncOptions::reserveLimits`、既定は snd_config_init の値）。`UndoStructEdit` は AddBus → RemoveBus、
   AddEffect → RemoveEffect（末尾）、AddSend は戻す手段が無い（false、ホットリロードで消える）。
10. **項目単位の Revert**: `MixerModel::Revert` / `RevertOnly`（1 手）の後に `CellSync::RequestRevert`。次の送信で、値が
    バンクの値のままで相手が 1.1 なら RevertValue、ほかは SetValue。Undo は普通の編集（SetValue）。読み返しは ReadCell を
    64 件ずつ（client の保持 256 より少なく）。1.0 の相手には何も聞かない。
11. **プロファイラの購読**: 画面の View の集合からマスクを作る（LOD は BlockTiming の段も使うので Timing を含める）。
    `Session::EnableProfilerStreams` は第 2 段の画面のビット（voices、meters、timing、memory、event log）を必ず残す。
12. **試聴**: Event・3D はゲームでもプレビューでも。ゲームではエミッタを動かせず、リスナーはゲームのもの（理由つき）。
    遷移の試聴はプレビューでは要求の時刻を含むブロックの頭で SetMusicState を記録し（ブロックの粒度）、遷移そのものは
    ルール表どおりにランタイムが決める（テストで `MusicEditor::Preview` の発火サンプルと一致）。ゲームでは From を
    OverrideState し、To はツールが送った時点（ブロックに合わせられない）。全停止は 1.1（1.0 は理由つきで無効）。
13. **再生**: Config は不具合報告にあるもの（プレビューの記録はエンジンの Config を全部書く）、無ければ `ReplayConfigFor`
    （記録のレートとブロック長、フォールバックの容量、LOD 無効、RT ワーカー 0、ライブリンク無効）。バンクを読んでから
    記録の最初のブロック番号まで描いて Start（そろえられなければ `blockNumbersKept = false` を理由に出す、C-21）。
    比較は決定のログ（全項目）とビット（`BitExactExpected` のときだけ）。記録した実行との比較は、記録中にプロファイラが
    受けたイベントログの種類だけを、種類と ID の順で比べる（時計が違うので時刻は比べない）。
14. **期待のファイルと不具合報告の形**（テキスト、版 1）: 期待は `sonus-replay-expectation 1`、`digest`（あれば）、
    `decisions N` と 1 行 1 決定。不具合報告はフォルダ（`report.txt` の key = value、`summary.txt`、`recording.snrec`、
    `banks/*.sndb`、`expected.txt`）。バンクのファイル名に `/`・`\`・`..` は不可。
15. **sonus-replay**: `tool/preview/cli`（`tool/app` は Tela の実行ファイルなので使わない）。終了コード 0 一致、1 不一致、2 入力の誤り。
16. **未対応の判定は 1 か所**（`LiveSupportOf`）: PHYS（C-11）、XPCH・PTCH（C-10）は送らない（blocking）。CURV.compose、
    ランタイムが使わないカーブの対象（send、effectParam、spread、reverbSend、blend3D）、SNPV の effectParam / sendLevel は
    送るが効かない（警告）。統合後はこの関数を直す。

## 契約の変更点

なし。`contract/` は変えていない（新しいコマンド種別・メッセージ・版の数字なし）。`api/compose/c_api.cpp` への追記もなし。
ツールの公開ヘッダの追記だけ（上の表。既存の構造体の途中には何も入れていない。`CellStatus` は末尾に足した）。

## 他の持ち場への依頼

| 番号 | 依頼先 | 要約 |
| --- | --- | --- |
| C-20 | api / 統合担当 | ツールの中でランタイムを組み立てる口（構成ルートの内部の入口に、ライブリンクの転送路・Replayer・手で描く出力を渡せるもの）。今は `tool/preview` が D-93 の順を写している |
| C-21 | core / 統合担当 | 再生のために、描かずにバンクを登録する口か、Replayer がブロック番号の基準を持つ口。今は記録が作成直後から始まるとブロック番号をそろえられない |

既存の依頼で組 P に関わるもの: C-01（構造の変更の core の編集器。今は UNSUPPORTED でホットリロードに回る）、C-02（試聴ごとの停止）、
C-03（鳴ったままのホットリロード、バイト列の転送）、C-04（途中からの記録の core の状態）、C-05（再生での LOD・ストール）、
C-06（ストリームの詳細、ワーカーごとの CPU）、C-07（決定のログの Clip・遷移）、C-10（パッチ）、C-11（マテリアル表）。

## livelink への要望

- ゲームの予備容量の使用量と限度（`DevReserveUsage`）を返すメッセージ。今はツールが数えている（ゲームを再起動すると合わない）。
- RecordControl の開始の Ack を、オーディオスレッドが記録を始めたブロックが決まってから返す（または開始ブロックを返す）。
  今は Ack の直後に鳴らした音が開始前に入り、開始時に生きていた再生（`lostState`）になる（テストは数ブロック待って避けた）。
- バンクを外すメッセージ（文書からバンクを消したとき。今は一覧に残すだけ）。
- ReadCell を複数まとめて読むメッセージ（ミキサーの全セルの読み返しで往復が多い）。
- ReplaceChunk が拒んだ列（どの列が live でないか）を Error の文に入れる（今は UNSUPPORTED だけで、ツールはバンクごとリロードする）。

## モデルへの要望（組 M）

- `ProjectDocument::lastChanged()` は最後の 1 手だけ。LiveSync は「この revision より後に変わったオブジェクト」が欲しい
  （`ChangedSince(revision)`）。今は Tick ごとに集めるので、Tick の間の複数の手の前のものは落ちる（項目の表示だけに影響）。
- `DiffBuilds` の ValueEdit / ChunkEdit から、オブジェクトとメンバのパスを引く逆引き（今は `FieldColumns` を (chunk, column) で
  逆に引いたパターンだけ）。
- 差分ビルド（バンク単位・オブジェクト単位）。ドラッグ中の値の編集でも文書全体をビルドしている（間引きで抑えている）。
- `BankImage` に languageId（今は `view()->languageId()`）。

## 画面への要望（組 U が画面にするときに要る読み出しと操作）

- 接続: `LiveSync::phase()`（色とテキスト `LivePhaseName`）、`check()`（版、両方のハッシュ、バンクごとの判定と「知っている
  ビルドか」）、不一致の質問のダイアログ（`pending()`、`ChooseSendDiffs` / `ChooseRefuse`）、`features()`（1.0 の相手で無効な
  機能と理由）。
- ライブ反映: `items()`（オブジェクトごと・メンバごとの経路と状態。エディタの各行の小さなランプ）、`problems()`（編集の
  revision、オブジェクト、メンバ、文。クリックでそのオブジェクトへ）、`unsupported()`（blocking と警告）、`reloads()`（進み具合
  の帯: Unloading → Loading → Done / Failed、外れたままの警告）、`reserve()`（バス・エフェクト・バイトの棒）と
  `structEdits()`（行ごとに Undo）、`stats()`、`Resync()` のボタン、`FlushNow()`（ドラッグの終わり）。
- ミキサー: 上書きの一覧の行に「この項目だけ戻す」（`Session::RevertOnly(target)`）、Revert は `RevertItem`。読み返しの
  ボタン（`ReadBack`）と、セルごとの `readCurrent` / `readOverridden`（ゲームの値がツールと違えば印）。
- 試聴: 宛先の切り替え（`AuditionRouter::SetTarget`、`Ready(&why)`）、3D の図のドラッグ → `Update3D`、音楽エディタの
  遷移の試聴ボタン → `PlayTransition`（`MusicEditor::Preview` の発火位置を図に）、パラメータのスライダ → `SetParam`、全停止。
  `AuditionResult::unsupported` をトーストに。
- プロファイラ: View のタブ（`SetViews`）、CPU のグラフ（`cpu()` の load、`cpuDetail()`）、メモリの区分の棒（`Memory()`）、
  ストリームの表、LOD の段と履歴、イベントログの絞り込み（`LogFilter`、`LogEntryKindName`）、取りこぼしの数（`*Count().missed`）、
  `Available(view, &why)`。
- 記録と再生: 記録の開始・停止（既存の Profiler）、要約（`DescribeRecording(...).Text()`）、「2 回再生して比べる」ボタン
  （`ReplayTwice`、`ReplayComparison::Text()`）、「聞く」（`OutputPlayer`）と「WAV に書き出す」（`WriteWav`）、不具合報告の
  書き出し（タイトルとメモの入力、`MakeBugReport` → `WriteBugReport`）と読み込み・再生（`ReadBugReport` → `ReplayBugReport`）。
  文字入力は Tela に無い（既存の課題）。
- プレビューエンジン: 状態（`Status()`）、出力の選択（手動 / デバイス / 無音）、メータ（`RecentOutput`）。

## ライブでつないでいない箇所

- 構造の変更が core に反映されること（C-01）。今のランタイムは StructEdit に UNSUPPORTED を返すので、ツールはホットリロードに
  回す（テストは代役の編集器 `YesEditor` で StructEdit の経路を通した）。StructEdit で足したバスに後から SetValue しても、
  バンクの表には無い（上書き層は表のセル）ので届かない。
- 予備容量のゲーム側の値（livelink への要望）。
- バンクの削除（外すメッセージが無い）。言語バンクのプレビューでの読み込み（`DeclareLanguage` をしていない。一覧に出す）。
- マテリアル表（PHYS、C-11）、パッチ（XPCH / PTCH、C-10）、ランタイムが使わないカーブの対象・compose・スナップショットの種類は
  `LiveSupportOf` で未対応として一覧（統合後はこの関数を直す）。
- 試聴ごとの停止（C-02）。ゲームの試聴のエミッタの移動とリスナー（プロトコルに無い）。ゲームでの遷移の要求をブロックに
  合わせること。
- 鳴ったままのホットリロード（C-03）。今はそのバンクの音が止まる。
- 再生での LOD・ストールの再現（C-05）、決定のログの Clip・遷移（C-07）、途中からの記録の core の状態（C-04）。
- プレビューのストリーム素材（IO とストリームは組み立てたが、テストはメモリ上の素材だけ）。

## Tela への提案

なし（組 P は Tela を使っていない。画面への要望は上）。

## Ergo Studio の sonus プラグインに足せるもの

- 核だけで: `LiveSync`（文書のライブ反映。Ergo のゲームにつないだまま編集が届く）、`AuditionRouter`（ゲームへの試聴）、
  `ProfilerStreams`、`recording_report`（記録の要約、期待、不具合報告のフォルダ）、`CellSync` の Revert と読み返し、
  `MixerModel::RevertOnly`、`Session::AttachDocument`。
- `Sonus::tool_preview` を足せば: プレビューエンジン（ゲームなしで試聴、ホットリロードの書き出し先）と再生のホスト
  （2 回の再生と比較、不具合報告の再生、WAV）。Ergo は C++17 なので、C++20 のヘッダを含めるファイルは C++20 にする
  （`sonus_tool_preview` は `cxx_std_20` を伝える）。

## 回した構成と結果

ビルドは `$S/live/b-*`、-j2（終わったら消した）。

| 構成 | コミット | 実行したテスト | 結果 |
| --- | --- | --- | --- |
| 8: TOOL_CORE、gcc Debug | d017510 | 全部（266） | 263 通過。失敗 3: `embed_subproject`（session.hpp が live_sync.hpp を含めて tool_core だけのホストで通らなかった → 0a7da93 で直し、通過を確認）、`livelink_indep_s7_unit_test` / `livelink_indep_s7_impl_test`（組 L の決定と食い違う既知の独立テスト `IndepS7Replay.LostStateIsReported`、`IndepS7Impl.Defect_RemoveOfLiveBusAddedWithFullStructLogKeepsReserve`。組 P は livelink を変えていない） |
| 8: TOOL_CORE、gcc Debug | 0a7da93 | `embed_subproject` と `tool_`（52） | 全部通過 |
| （以下、走らせたら追記） | | | |

`tool_live_mixer_test` の再現と修正の確認: 上の節（1 コア 50 回、CPU を食うループと同じコアで 30 回、全通過）。

MinGW（`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror -Wall -Wextra -fno-exceptions -fno-rtti`）: 新しい・変えた .cpp
（tool/src の 8、tool/preview の 5、tool/test の 8）全部通過（0a7da93）。

## 未検証の箇所

- 実デバイスでのプレビュー（`PreviewOutput::Device`）と `OutputPlayer`（コンテナにデバイスが無い。無音の出力は並行性のテストで回した。
  `OutputPlayer` はテストしていない）。
- TCP の本物のゲーム（テストはプロセス内の転送路と、手で動かす 1.0 のゲーム）。
- 実際に予備容量を超える StructEdit（OUT_OF_BUDGET → ホットリロード）の経路は、代役の編集器が常に OK なので通していない
  （UNSUPPORTED → ホットリロードは通した）。`UndoStructEdit` の RemoveBus は通していない（RemoveEffect は通した）。
- ゲームのバンクが知らないビルドのとき（`knownBase = false`）のホットリロードと、書き出し先が無いときの全表の ReplaceChunk。
- 取りこぼしの数（`missed`）が 0 でない場合（クライアントの履歴が押し出される量を作っていない）。
- 言語バンク、依存のあるバンクのホットリロード（BANK_IN_USE）。
- `items()` の対象は Tick の間に起きた最後の手の GUID だけ（モデルへの要望）。間の手のオブジェクトは「バンク全体」の項目になる。
- 大きなプロジェクトでの速さ（編集ごとに文書全体をビルド）。
- Windows / macOS での実行（MinGW の構文検査だけ）。
