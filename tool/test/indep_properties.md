# 独立テスト: 検証すべき性質（実装を読む前に作成）

出典: 設計書 4.6 / 11 章 / 14 章、`spec/interface/livelink-protocol.md`、`CONTRACTS.md` D-56、`tool/README.md`（実装担当の決定。
設計書と矛盾しない限り仕様として扱う）。各行の「出典」は R = README、P = プロトコル、D = 設計書、C = CONTRACTS。

## A. 値の送信（cell_sync / connection）

| # | 性質 | 出典 |
| --- | --- | --- |
| A1 | 値が変わったときだけ送る。同じ値を設定し直しても送らない。同じフレーム内の連続ドラッグは 1 通（最後の値） | R |
| A2 | Ack(SND_OK) で確定し、Ack 待ちの数が 0 に戻る | R |
| A3 | Ack(SND_ERR_COMMAND_BUFFER_FULL) は同じ値を再送する（拒否扱いにならない） | R |
| A4 | それ以外の Ack 失敗は「拒否」として残り、値がまた変わるまで再送しない。値が変われば送る | R |
| A5 | 接続（再接続）のたびに「バンクと違う値」と「一度でも送った値」を送り直す（バンク値に戻した値も、一度送っていれば送る） | R |
| A6 | 切断中の編集は送信待ちに残り、接続したら送る | R |
| A7 | ビルドハッシュ不一致: 既定は SyncDecision(mode 0) を送り、ミキサーが管理する値を全部送る。`--refuse-mismatch` なら mode 1 / 切断し、再試行しない | R, P§4 |
| A8 | 本物の core + livelink で、送った値が出力の DC レベルに解析どおりに効く（dB → 振幅） | D 11.2 |
| A9 | 切断しても上書きした値はゲーム側に残る（revertOnDisconnect でない限り） | D 11.2 |
| A10 | 古い Ack（前セッションの seq）が新しいセッションの値を確定させない | P§2（seq は送信側の通し番号） |

## B. ミキサーの規則

| # | 性質 | 出典 |
| --- | --- | --- |
| B1 | フェーダー範囲 −∞〜+12 dB。−∞ は −144 dB として送る | R |
| B2 | ミュートは −144 dB を送り、フェーダー値は残る。ミュート解除でフェーダー値を送る | R |
| B3 | ソロ: ソロのバス・その祖先・子孫以外を −∞。複数ソロは和集合。センドは追わない | R |
| B4 | ソロ + ミュート: ミュートしたバスはソロでも −∞。ソロ解除で全バスが元の値（フェーダー値／ミュート状態）に戻る | R |
| B5 | カテゴリのミュートも −∞ | R |
| B6 | エフェクトのパラメータは dsp の範囲に切り詰め、整数パラメータは丸める | R |
| B7 | kInit パラメータは操作不可 | R |
| B8 | 上書き一覧 = バンクと違う値すべて + 有効なパラメータ上書き。Revert / Revert all 後は一覧が空（または該当行が消える） | R |
| B9 | バス音量の Revert はミュートとソロも外す。他のソロでまだ消えていれば全ソロを外す | R |
| B10 | パラメータのノブを回すと上書きが有効になり、取り消し 1 回で両方戻る | R |

## C. 取り消し・やり直し

| # | 性質 | 出典 |
| --- | --- | --- |
| C1 | ドラッグ 1 回 = 取り消し 1 回（途中の値は残らない） | R |
| C2 | 同じコントロールへのホイール連続 = 1 回。別のコントロールを挟むと分かれる | R |
| C3 | Undo → Redo で状態が往復し、Undo 後に送る値は Undo 後の状態の値 | R |
| C4 | 新しい編集で Redo の列は消える | 一般 |
| C5 | Revert / Revert all も取り消せる（一覧と状態が一致して戻る） | R（未明記 → 穴の可能性） |

## D. コントロールの写像

| # | 性質 | 出典 |
| --- | --- | --- |
| D1 | p→dB の折れ線の節点: 0 → −∞、0+ → −90、0.05 → −60、0.25 → −30、0.5 → −12、0.75 → 0、1 → +12 | R |
| D2 | (0,1] で連続・狭義単調増加、dB→p は逆写像（往復誤差が小さい）、範囲外は飽和 | R |
| D3 | 行程 = 高さ − 18 px を動かすと p が 1 変わる。Shift で 1/10。Shift の途中切り替えで値が跳ばない | R |
| D4 | ダブルクリック: 同じコントロールを 400 ms 以内・4 px 以内。境界（400 ms ちょうど・4 px ちょうど／超え）、別コントロールは不成立。成立でバンク値 | R |
| D5 | ホイール: フェーダー ±1 dB（Shift 0.1）、−∞ から上で −90、−90 未満で −∞。ノブは範囲の 1/100 | R |
| D6 | ノブ: 縦 150 px で全範囲、線形。Hz で最小が正なら対数（中点 = 幾何平均） | R |
| D7 | メータ: 高さ ×(dB+60)/60、−60 以下 0、0 以上で満杯。区分 −18 / −6 | R |
| D8 | ピークホールド: 新しいピークで即上がる、1.5 秒保持、その後 20 dB/秒で落ちる（現在のピークより下には落ちない） | R |
| D9 | クリップ: 0 dBFS 到達で点き、Reset clips まで残る | R |

## E. プロファイラ・記録ファイル

| # | 性質 | 出典 |
| --- | --- | --- |
| E1 | ボイス一覧: 実ボイスが先、可聴度の降順 | R |
| E2 | イベントログは新しい順 | R |
| E3 | 処理時間グラフは直近 240 個、縦軸 max(1.25×予算, 1.1×最大) | R |
| E4 | 記録ファイル: 64 バイトヘッダ `SNRC`、版 1、FNV-1a 64 で本体を検査。保存→読み込みで往復 | R |
| E5 | 壊れた magic / 版 / 切り詰め / 長さ不一致 / チェックサム不一致を拒否（落ちない） | R, D 14 |
| E6 | 記録本体はプロトコル §7 のレコード列。要約（アリーナ・LOD・ストール・バンクの数、ブロック範囲）が正しい。不正なレコード長は拒否 | P§7, R |
| E7 | プロジェクト JSON の不正入力（切り詰め・型違い・ランダム変異）で落ちない（エラーを返す） | D 14（ファズ） |

## F. Tela の安全な呼び方（D-56）

| # | 性質 | 出典 |
| --- | --- | --- |
| F1 | SafeDrawing は図形 256・点 4096・作業量の上限を超える前に拒否し、Tela に例外を投げさせない（境界ちょうどは通り、+1 は拒否） | C D-56, R |
| F2 | 非有限座標・負の大きさなどを拒否 | C D-56 |
| F3 | 大きな DPI・極端なビューポートでも描画が例外なく終わる | C D-56 |

## G. 並行性

| # | 性質 | 出典 |
| --- | --- | --- |
| G1 | 接続・切断・再接続・新しいセッション検出を UI スレッドと受信スレッドが並行で行ってもデータ競合が無い（TSan） | R |
| G2 | 新しいセッション（ゲーム再起動）を検出したら値を送り直す | R |

## H. ビルド構成

| # | 性質 | 出典 |
| --- | --- | --- |
| H1 | SND_BUILD_TOOL=OFF ではツールのターゲットが作られず、Tela を要求しない | C D-56 |

---

## 対応するテスト（実装を読んだ後に追記）

| 性質 | テスト（`tool/test/`） |
| --- | --- |
| A1 | `indep_sync_test` OnlyChangedValuesAreSentOncePerFrame |
| A2, A3 | CommandBufferFullResendsTheSameValue |
| A4 | OtherErrorsAreRefusedUntilTheValueChanges |
| A5, A6, G2 | NewSessionResendsDifferingAndEverSentValuesOnly、`indep_concurrency_test` ChaosConvergesToTheToolsState |
| A7 | BuildMismatchSendsEveryManagedValue、RefusedMismatchDisconnectsAndDoesNotRetry |
| A8, B3, B4 | IndepToolLive.SoloAndMuteCombinationsAndRelease（本物の core の DC レベル） |
| A10 | AckOfAnOlderSendDoesNotConfirmTheNewValue |
| B1, B2, B5 | MuteSendsMinus144AndKeepsTheFader、`indep_model_test` FaderRangeEffectParametersAndInitOnly |
| B3（木） | SoloKeepsTheSoloedBusItsAncestorsAndDescendants |
| B6, B7 | FaderRangeEffectParametersAndInitOnly |
| B8, B9, C3, C5 | RandomEditsKeepOverridesUndoAndRevertConsistent、IndepToolLive.RevertRevertAllAndUndoReachTheGame |
| B10 | `indep_ui_test` ParameterKnobDragIsOneStepWithTheOverride、Defect_parameter_knob_wheel_is_two_undo_steps |
| C1, C2, C4 | HistoryMergesAndRedoClearing、WheelOnAnotherControlStartsANewStep |
| D1, D2 | FaderLawKnotsContinuityMonotonicityAndInverse |
| D3 | DragWithoutVerticalMotionKeepsOrdinaryFaderValues、KnobShiftToggleMidDragDoesNotJump、Defect_fader_press_without_motion_at_minus90_silences、Defect_knob_press_without_motion_changes_the_value |
| D4 | DoubleClickLimitsAreInclusiveAndPerControl |
| D5, D6, D7 | WheelKnobAndMeterMappings |
| D8, D9 | PeakHoldHoldsFallsAndLatchesClip、MeterHoldAndClipThroughTheProfiler、Defect_peak_hold_restarted_by_stale_meter |
| E1 | VoicesRealFirstThenLoudestFirst |
| E3 | TimingGraphShowsTheLast240Blocks |
| E4, E5, E6 | HeaderLayoutChecksumAndRoundTrip、TruncatedOrDamagedFilesAreRefused、RecordingStatesFollowAcksAndSettle、Defect_load_accepts_unreadable_records |
| E7 | FuzzedProjectsNeverCrashTheModelsOrTheUi、Defect_duplicate_names_crash_the_ui |
| F1, F2 | SafeDrawingStopsExactlyAtTelasLimits |
| F3 | TimingGraphWorkAtDpi2IsWithinTelasBudget、ScreensComposeAtOrdinaryDpiAndOddViewports、Defect_timing_graph_exceeds_telas_budget_at_dpi4 |
| G1 | ChaosConvergesToTheToolsState（TSan） |
| H1 | ビルド構成の確認（テストではない） |

E2（イベントログの新しい順）は画面の文字列でしか見えないため未テスト。

## Defect_*（失敗するテスト = 実装の誤り）

| テスト | 期待 | 実際 |
| --- | --- | --- |
| Defect_fader_press_without_motion_at_minus90_silences | −90 dB（またはバンク値が −90 未満）のフェーダーを押して横にだけ動かしても値は変わらない | −90 → p = 0 → −∞ になり、バスが無音になる（取り消し 1 手も積まれる） |
| Defect_knob_press_without_motion_changes_the_value | 押して横に動かしただけのノブは値も履歴も変わらない | 値 → 正規化 → 値の往復が厳密でない（対数ノブ等）ため値が 1 ulp ずれ、取り消し 1 手と上書きの差分が生じる |
| Defect_parameter_knob_wheel_is_two_undo_steps | 上書きが無効のパラメータのノブをホイールで続けて回すと取り消し 1 手 | 最初のノッチ（上書きを有効にするグループ）と以降のノッチ（併合）で 2 手 |
| Defect_peak_hold_restarted_by_stale_meter | ピークホールドはピークから 1.5 秒保持して 20 dB/秒で落ちる | 新しい BusMeters が来ない間（ゲームの停止・切断）も最後のメータを毎フレーム与え直し、保持が毎回やり直されて落ちない |
| Defect_load_accepts_unreadable_records | チェックサムが合っていても読めないレコード列（未知の kind）の記録は「壊れている」として Load を拒否 | `Profiler::LoadRecording` は成功し、無効な要約（valid = false）を持つ |
| Defect_duplicate_names_crash_the_ui | 同じ名前のバス（ビルドはエラー、ツールは開く）でもツールは落ちない | ランタイム ID が同じため Tela の要素 ID が重複し、Tela の例外で std::terminate。上書き一覧のキーも重複し、Revert は先頭の行にしか届かない |
| Defect_timing_graph_exceeds_telas_budget_at_dpi4 | ヘッドレスが受け付ける `--dpi` ≤ 4 で描画が例外なく終わる | SafeDrawing の作業量の見積もりが DPI 2 固定のため、DPI 4 で予算超えの処理時間グラフを通し、Tela::Raster が例外 → std::terminate（8192×2048、DPI 4、交互に跳ねる処理時間） |
