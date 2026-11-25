# セッション C 組 U（Tela の画面）メモ

実装担当のメモ。受け入れ側への引き継ぎ。ブランチ `c/ui`（ローカルコミットのみ）。出発点は ce1aa3e（組 M・P・L の取り込み後）。

## 進み具合

- [x] 0. 共通の土台（画面の切り替え、文書の Undo / Redo、保存、文字の入力、候補の一覧、ドラッグの受け口）
- [x] 1. Event エディタ
- [x] 2. ミキサーの残り（バス・センド・エフェクト・スナップショット・ダッキング・カテゴリ、書き戻し）
- [x] 3. パラメータとカーブ
- [x] 4. 3D 減衰のプレビュー
- [x] 5. 音楽エディタ
- [x] 6. マテリアル表
- [x] 7. 波形生成パッチ
- [x] 8. プロファイラの残り
- [x] 9. 記録と再生
- [x] 10. 接続
- [x] 11. 共通（切り替え、Undo / Redo、保存と未保存、問題の一覧、文字の入力）
- [x] テスト、ヘッドレスの全画面、README
- 構成ごとの結果は「回した構成と結果」。

## 作ったもの

書いた場所は `tool/ui/`、`tool/app/`、`tool/test/`、`tool/CMakeLists.txt`、`tool/test/CMakeLists.txt`、`tool/README.md` とこのメモだけ。
モデル（`tool/include`・`tool/src`・`tool/preview`）、`livelink/`、`buildtool/` は変えていない。

UI（`sonus_tool_ui`、Tela に依存。OS 非依存）:

| ファイル | 役割 |
| --- | --- |
| `ui/include/snd/tool/ui/views.hpp`（変更） | 画面 12 面（`Screen`、Mixer = 0 と Profiler = 1 は段階2 の番号のまま）、タブの順、`ScreenName` / `ParseScreen`、画面ごとの状態（選択・ページ・表示）、`ViewContext::app`、`ObjectKey`（GUID の 32 桁） |
| `ui/include/snd/tool/ui/app.hpp`, `src/app.cpp`（変更） | `Workspace`（文書、プレビューエンジン、試聴の振り分け、再生のサービス）、`Key`、文字の入力と候補の一覧、文書の Undo / Redo / 保存と未保存、問題の一覧（`CheckDocument` を編集の後に 250 ms 以内で）、`Navigate`（GUID からその編集画面へ）、3D プレビューの持ち主、ミキサーの結び直し、`WriteBackMixer`、Delete の受け口、プレビューエンジンの `Pump` |
| `ui/include/snd/tool/ui/text_entry.hpp`, `src/text_entry.cpp`（新） | キーの表現（`KeyEvent`）とツールの中の文字の入力（UTF-8、数値、確定・拒否・取り消し） |
| `ui/include/snd/tool/ui/kit.hpp`, `src/kit.cpp`（新） | 画面の部品: 名前の欄、数値の欄（± / ドラッグ / ホイール / 入力）、候補の欄、トグル、ページ送り、ドラッグの受け口（`Hit`）、タブ、`DocumentDrag`（ドラッグ 1 回を文書の 1 手に） |
| `ui/include/snd/tool/ui/pointer_router.hpp`, `src/pointer_router.cpp`（変更） | `ControlKind::Custom` と `DragHandler`（押下・移動・離す・ダブルクリック・ホイール、押した時のハンドラをジェスチャーの終わりまで保持、要素が消えたら離したことに）。フェーダーとノブの振る舞いは変えていない |
| `ui/include/snd/tool/ui/control_math.hpp`, `src/control_math.cpp`（追記） | `TimeAxis`（タイムライン）、`RulerStepMs`、`PlotAxes`（値の図）、`NumberDrag` / `ClampNumber` |
| `ui/include/snd/tool/ui/editor_draw.hpp`, `src/editor_draw.cpp`（新） | トラックのレーン、ルーラー、カーブ、上から見た図、印のタイムライン（音楽）、パッチ、系列（CPU・LOD）の図形（`SafeDrawing` で上限を守る） |
| `ui/include/snd/tool/ui/services.hpp`（新）, `src/replay_service.cpp`（新、別ライブラリ `sonus_tool_ui_replay`） | 再生のサービス `ReplayService`（2 回の再生と比較、不具合報告の書き出し・読み込みと再生、WAV、試聴）。実装は `Sonus::tool_preview` を使う |
| `src/status_view.cpp`（変更） | ヘッダ（12 のタブ、接続、不一致の質問）、フッタ（メッセージ、プロジェクトと未保存、文書の Undo / Redo / Save、問題の数、同期の数） |
| `src/common_view.cpp`（新）, `src/editor_util.hpp`（新） | 候補の一覧の重ね表示、Problems 画面、オブジェクトの一覧（選択・New・Remove・Copy・名前・ページ）、候補の作り方などの共通 |
| `src/buses_view.cpp` | ミキサーの構造（Buses / Snapshots / Ducking / Categories） |
| `src/event_view.cpp` | Event エディタ（試聴のボタンつき） |
| `src/params_view.cpp` | パラメータとカーブ |
| `src/spatial_view.cpp` | 3D 減衰のプレビュー |
| `src/music_view.cpp` | 音楽（Segments / Playlists / States & rules / Stingers） |
| `src/material_view.cpp` | マテリアル表 |
| `src/patch_view.cpp` | 波形生成パッチ |
| `src/profiler_view.cpp`（追記） | プロファイラのタブ（Overview = 段階2、CPU、Memory、Streams、LOD、Event log） |
| `src/replay_view.cpp` | 記録と再生（ファイル名の入力と、同じフォルダの `*.snrec` の一覧から選んで読み込む） |
| `src/link_view.cpp` | 接続（Matching / Items、Problems は Problems 画面へ） |
| `src/mixer_view.cpp`（追記） | 上書きの一覧の上に Read back と Write back to project |

アプリ（`tool/app/`）: `main.cpp`（文書を読み、`AttachDocument`、`--preview` でプレビューエンジンを作って接続、試聴の振り分け、再生の
サービス、`--screen` の全画面）、`options.*`（`--send-diffs`、`--preview silent|device`、`--reload-dir` / `--reload-prefix`、
`--report-dir`、`--screen <名前>`）、`host_windows.cpp`（WM_KEYDOWN / WM_CHAR を `App::Key` へ。入力中・一覧が開いている間の Esc は
Tela に渡さない）、`host_macos.mm`（キーコードと文字を `App::Key` へ）。

テスト（`tool/test`、Tela 入りの構成だけ）: `ui_editor_helpers.hpp`（文書の上の App、id で押す・候補を選ぶ・打つ・ドラッグ）、
`ui_music_fixture.hpp`、`ui_event_screen_test`、`ui_buses_screen_test`、`ui_params_screen_test`、`ui_spatial_screen_test`、
`ui_music_screen_test`、`ui_material_screen_test`、`ui_patch_screen_test`、`ui_live_screens_test`（プレビューエンジンをゲームに）、
`ui_editor_limits_test`、`ui_editor_input_test`、`ui_editor_concurrency_test`、`ui_editor_raster_test`（コーン・レーン・印・
組の表のセルの画素、DPI 1・2）、`tool_app_screen_<名前>`（12 面のヘッドレス）と
`tool_app_bad_screen`。内容は `tool/README.md` の「段階5 の画面（組 U）」の表。

## 差し戻し（独立テストの後の決定 D-186〜D-189）で変えた点

決定の案の行き先: a・b・f・i・j・l・m → D-186、c・d・e → D-187、g・h → D-188、k → D-189（名前で一意な物は名前から）。

| 決定 | 欠陥・仕様の穴 | 変えたこと | コミット |
| --- | --- | --- | --- |
| D-187 | 欠陥 5 | `App::Pick` は値を写してから一覧を閉じる（同じ形の箇所はほかに無かった: `TextEntry::CommitNow` は写してから呼ぶ、ほかの関数は値渡し） | 353bee2 |
| D-187 | 欠陥 1 | 打った数値は切り詰めずに setter へ（拒否なら開いたまま）。± / ドラッグ / ホイールは欄の範囲で止まる | 00986ce |
| D-187 | 欠陥 2 | ステップ（ホイール、±）は `App::FieldBase` / `FieldWrote`（宣言の後に書いた最新の値）に足す | 00986ce |
| D-187 | 欠陥 3、穴 4 | 縦のドラッグは押した点からの移動量 `round(dy / 48)`、範囲の外は端のトラック | 00986ce |
| D-187 | 欠陥 4 | 数値の欄の値は 48 px 以上（`kMinNumberValue`）。ラベルが縮み、24 px 未満なら値の文字へ。ボタンの入らない欄はボタン無し。Params のパラメータの欄は 1 行に 1 つ | 00986ce |
| D-186 | 穴 2 | 入力中は全キーが入力欄へ（Ctrl+Z / Y、Delete、F キー、Ctrl+PageUp/Down は何もしない） | 00986ce |
| D-186 | 穴 9 | Esc は常にツールが受ける（`App::Key` は true）。ドラッグ中なら `PointerRouter::Cancel`（`DragHandler::cancel`: 文書のドラッグは 1 手を戻す、フェーダー・ノブは押した時の値へ）、パッチの接続待ちを消す。Windows のホストは Esc を Tela に渡さない | 00986ce |
| D-186 | 穴 1 | 位置で持つ選択（クリップ、アクション、点、テンポ点、Stem、Playlist の行、ルール）を Undo / Redo と画面の切り替えで外す。マーカーの選択は名前で持つ | 00986ce |
| D-188 | 穴 3 | 構造の変更での結び直しは `MixerModel::Rebind` でライブの値を対象で引き継ぐ。引き継げない値があれば保留して質問（`rebind.writeback` / `rebind.discard`、本体の上の帯）。構造が元に戻れば（Undo）質問も消える。上書きを戻して引き継げない値が無くなっても結び直す | e4fa328, 00986ce |
| D-189 | 穴 6 | 材質（`ma.mat.<NameKey>`、`ma.pair.<a>.<b>`）、State、マーカー（タイムラインと表）、Stinger の ID は名前から（`ui::NameKey` = HashId の 16 進 8 桁、重なれば `_<位置>`） | 00986ce |
| D-189 | 穴 5 | `EventEditor::Cone()`（実効のコーン、無いときは 0 / 0 / 0 dB）。Event 画面の欄と 3D 画面の行が同じ値（無ければ「none」）。図はコーンが有効（0 < outer < 360）なときだけ | e4fa328, 00986ce |
| D-189 | 穴 7 | 3D の既定のエミッタは maxDistance の半分（無ければ 5 m）だけ右前（(0.6d, 0.8d)）。取っ手を先、本体を後に宣言（本体が上） | 00986ce |
| D-189 | 穴 8 | カーブの図は x にも 4 % の余白（`PlotAxes::Fit` の xMargin、`kCurveXMargin`）。点のドラッグで動かない軸は元の値のまま | 00986ce |
| D-189 | 穴 10 | Link は未接続なら「not connected」（版・機能の理由を出さない） | 00986ce |
| D-189（訂正） | 穴 11 | 同じファイル・行・列・種類・文言の問題を 1 回にする `DistinctIssues` を `doc_build` に足し、表示の側（`App::problems()` = Problems 画面とフッタの件数）がそれを通す。`CheckDocument` は buildtool の診断と一語一語・件数まで同じまま（ecd6d46 で一度 `CheckDocument` で除いたが、受け入れ側の訂正で 2fbb846 で戻した） | 146f728, 2fbb846 |
| テスト | 弱い箇所 | `ui_editor_fix_test`（上の全部）、`model_fix_test`（核）、上限を画面ごとの別プロセスで 320×240 まで（`tool_ui_editor_limits_<画面>`） | fbc68cb ほか |

組 P の差し戻しの取り込み（`git merge --no-ff c/live`、衝突なし）: Link 画面に `LiveSync::failedBanks()`（バンクと理由、`ln.failed.<行>`）、
ヘッダの接続のランプは `LivePhase::Failed` で赤。テスト `ToolUiLive.FailedReloadIsShownOnTheLinkScreen`（書き出せないホットリロード）。
`ChooseSendDiffs` / `ChooseRefuse` と `TransitionPreview` の変更は、画面がモデルの値を出すだけなので直しは要らなかった（音楽の画面のテストは
`MusicEditor::Preview` の値と比べている）。

モデルに足した・直したもの（U-fix.md で許された `mixer`・`event_editor`・`doc_build` だけ）:
- `mixer.hpp/.cpp`: `struct LiveValue`、`MixerModel::LiveValues()`、`static Unmatched(project, values)`、`Rebind(project, values, dropped)`。
- `event_editor.hpp/.cpp`: `EventEditor::ConeDesc`、`EventEditor::Cone()`。
- `doc_build.hpp/.cpp`: `DistinctIssues(issues)`（新しい関数。`CheckDocument` は変えていない）。

決定と食い違って落ちる独立テスト（`indep_s7_*` は変えていない）:
- `IndepS7Ui.ChangingTheScreenEndsTheTextEntry`（`tool_indep_s7_ui_common_test`）: 入力中の F4 で画面が変わり入力が終わることを期待。D-186 で入力中の画面の切り替えのキーは入力欄が受ける（何もしない）。`OnScreen` での切り替え（ボタン）では今も入力が終わる。
- `IndepS7UiScreens.MaterialRowsLastMaterialAndSymmetricPairs`（`tool_indep_s7_ui_screens_test`）: `ma.mat.<番号>` の ID を期待。D-189 で材質の ID は名前から。
- `IndepS7UiScreens.ProblemsRowsAreTheBuildsIssues`（`tool_indep_s7_ui_screens_test`）: Problems の行の数 = `CheckDocument` の件数を期待。
  訂正後の D-189 で画面は重複を 1 行にする（`CheckDocument` は重複を含むまま）ので、重複のある文書で行が 1 つ少ない。
- `IndepS7UiDefects.SpecGap_ClipSelectionIsAnIndexAcrossUndo`（`tool_indep_s7_ui_SpecGap_ClipSelectionIsAnIndexAcrossUndo`）: 今の振る舞い（Undo の後の Delete で 1 つ消える）を記録する。D-186 で選択は外れ、Delete は何も消さない（クリップは 2 つのまま）。

## 差し戻し 2 回目（ui-indep.md「再確認」の N1〜N6 と決定 1〜7）で変えた点

先に `git merge --no-ff c/live`（8ed549d、衝突なし。`IPreviewEngine::StreamPrimeMs` が増えたが UI とテストに実装は無く直しは不要）。

| 決定 | 欠陥 | 変えたこと | コミット |
| --- | --- | --- | --- |
| 1 | N1 / N2 | Esc の取り消しはグループを捨てる。`ProjectDocument::AbortGroup`（触った物を BeginGroup の時の状態に戻し、段を残さず Undo / Redo の一覧は変えない。中身が変わったときだけ revision を進める）。数値の欄のドラッグと文書のドラッグ（`DocumentDrag`）の cancel がこれを呼ぶ。フェーダー・ノブは `ControlSink::ControlCancel`（既定は値を戻して ControlEnd）で押した時の値と（パラメータの上書きの）on/off に戻してからグループを閉じる。`EditHistory` は行って戻った変更を段にしないので、ライブの履歴（Undo / Redo の数）も変わらない | 0629b4a, 2d8b692 |
| 2 | N3 | 整数の欄は変換の前に、有限でない・整数でない・負・型の範囲の外を拒否（`WholeU16/U32/U64`、`commitText` も整数の欄は小数を拒否）。入力は開いたまま、文書は変わらない。Link の Port は 1〜65535。ファイルから読んだ値・ドラッグの計算は飽和（`SatU32/SatU64/SatInt`）、float へは `ToF32`（範囲で止める）。UI の浮動小数→整数の直の変換（buses・event・link・music・profiler・patch・material の 18 か所と同じ形の箇所）を全部置き換えた | 2d8b692 |
| 3 | N4 | 窓を詰めて並べ直す方ではなく、**App が受け付ける最小の論理サイズを数値の欄のある画面で 800 × 500 に引き上げた**（`kMinEditorWidth` / `kMinEditorHeight`）。それより小さい窓では本体に「window too small: this screen needs 800 x 500」だけを出す（`body.small`、Profiler の Event log は `prof.log.small`）。対象は Buses、Event、Params、3D、Music、Material、Patch、Link と Profiler の Event log。Mixer（数値の欄なし。段階2 の `NarrowWindowPagesTheStrips` の 560 × 700 を保つ）、Profiler のほかのタブ、Replay、Problems は 320 × 240 まで描く。値は 640 × 400 で 9 個、800 × 500 で 0 個が窓の外（試した大きさ 640×400、800×500、960×600、1024×640、1280×720）から決めた。README に書いた | 2d8b692 |
| 4 | N5 | 「書き戻す」は、行き場の無い上書きの名前を控えてから `RebindProject(true)`（GUID・エフェクトの種類・パラメータで引き継ぎ、引き継げない物は捨てる）、その後で `WriteBackMixer`。消えた・種類の変わったエフェクトの上書きは新しい物に書かれず、「mixer: discarded N live overrides without a place: 名前…」をメッセージに出す。質問が出ている間に Write back を押しても同じ道を通る | 2d8b692 |
| 5 | N6 | `SpatialPreview::PlaceListener` / `PlaceEmitter`（±1e6 m）、`TurnListener` / `TurnEmitter`（±1e6 rad）で範囲の外と非有限を拒否。3D の欄はこれを通す。`NumberText` は非有限で「-」、`FiniteText` が printf の inf / nan を「-」に替え、`Kit::Label` の文字は全部これを通す | 0629b4a, 2d8b692 |
| 6 | 選択 | （3 回目で revision だけの判断に変えた）位置で持つ選択は、画面自身の編集（ポインタ・キー・Pick の後の revision を `knownRevision_` に控える）以外で revision が変わったとき外す。ただしホストの値だけの変更で外すと `Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused`（選んだクリップをホストが `MoveClip` で動かしてから開始位置を打つ。組 U の Defect なので通す必要がある）が落ちるので、**選択の指す配列の形（トラックごとのクリップ数・アクション数、カーブの点の数、テンポ点・Stem・Playlist の行・ルールの数）が変わったときだけ**外す（`App::PositionShape`）。Undo / Redo、画面の切り替え、書き戻し、結び直しの答えでは今まで通り常に外す | 2d8b692 |
| 7 | テスト | `ui_editor_fix2_test`（7 本: Esc と履歴、フェーダーの Esc、整数の欄、窓の最小、書き戻しの対応、3D の値、ホストの変更と選択） | 1e7771d |

モデルに足した・直したもの（許された `document`・`spatial_preview` だけ。`event_editor`・`doc_build`・`mixer` は今回変えていない）:
- `document.hpp/.cpp`: `ProjectDocument::AbortGroup()`。
- `spatial_preview.hpp/.cpp`: `kMaxCoordinate`、`kMaxAngle`、`PlaceListener`、`PlaceEmitter`、`TurnListener`、`TurnEmitter`。
- UI の公開の型: `ControlSink::ControlCancel`（`pointer_router.hpp`）、`App::WindowTooSmall` / `DeclareTooSmall`、`kit.hpp` の
  `WholeU16/U32/U64`・`ToF32`・`SatU32/U64/Int`・`ShownNumber`・`FiniteText`、`layout_metrics.hpp` の `kMinEditorWidth/Height`。

## 差し戻し 3 回目（ui-indep.md「再確認 2 回目」の P1〜P5 と決定 1〜6）で変えた点

| 決定 | 欠陥 | 変えたこと | コミット |
| --- | --- | --- | --- |
| 1 | P1・P2 | 最小の窓を全画面で 1 つ、**論理 1024 × 640** にした（`kMinEditorWidth` / `kMinEditorHeight`、DPI によらない）。未満では `App::Declare` が「window too small: the tool needs 1024 x 640」（`body.small`）だけを宣言し、タブ・フッタ・本体は宣言しない（キーは効く）。Profiler の Event log だけの `prof.log.small` はやめた。以上では全画面のボタンと欄が窓の中: パッチのノードの当たり（本体・出力・入力の端子）は図の矩形に切り、全部外なら宣言しない（出力の端子のドラッグの座標は切った後の原点から計算）。値は `ui_editor_window_test` の `SND_UI_WINDOW` で全画面（Mixer、Profiler 6 タブ、Replay、Problems、Link、エディタ 15 の選び方）× DPI 0.25 / 1 / 2 / 4 を確かめて決めた: 800×500 は Material の組の表と既定・解決、Segment のグリッド、Event の Add synth、記録の行のボタンが外、900×560・960×600 も Material と記録の行、1000×620 は記録の行のボタン 1 個、1024×640 で 0 個（1280×720 でも 0）。独立テストは `ui::kMinEditorWidth` / `kMinEditorHeight` を読むので食い違いは無い | 7600de5 |
| 3 | P4 | 位置で持つ選択は、画面自身の編集の後の revision（`knownRevision_`）と文書の revision が違えば外す（値だけの変更でも）。`App::PositionShape` はやめた | fb79bdb |
| 2 | P3・P5 | `ProjectDocument` の `Create` / `Set` / `SetDefaulted` / `Insert` は、どの列（U32 / F32 / U64 / I64）にも入らない数値（非有限、最大の有限 float 超）を拒否。`Modify` はその値を新しく持ち込むときだけ拒否（ファイルに元からある値で編集全部が止まらないように）。整数の列は前から各エディタが型の範囲で拒否している（`EventEditor::SetNumber` の priority / maxInstances、loopCount など）。モデルの浮動小数→整数は `ClampToU32` / `ClampToU64` / `ClampToI32`（NaN は 0、範囲で飽和）と `U32At` / `U64At` に: `TempoMap::Musical`、`project.cpp` の channels、`SegmentEditor::Tempo` の tick・beatsPerBar・beatUnit、マーカーの frame、スナップショットの effect / param / send、ルールの timingCount。遷移の試聴は `MusicEditor::kMaxRequestSeconds`（1e6 s）の外・負・非有限で無効、画面の `mu.pv.req` は拒否（最小先読み `mu.pv.prime` も 0〜1e5 ms）。ミキサーのパラメータの名前の値（`FormatValue`）は非有限で「-」 | b9498d2 |
| 4 | 穴（同じ種類の別のエフェクト） | `LiveValue::effectChain`（値を取った時のバスのエフェクトの種類の列）。結び直しで列（数・並び・種類）が変わったバスのエフェクトの値は引き継がず、質問に出す（名前に「バス 種類 パラメータ」） | a90c020 |
| 5 | 穴（入れ子の Abort） | `AbortGroup` のヘッダに「どの段から呼んでも一番外のグループごと捨て、全段を閉じる。その後の EndGroup は何もしない」と書いた（振る舞いは変えていない） | b9498d2 |
| 6 | テスト | `model_fix3_test`（核）、`ui_editor_fix3_test`、`ui_editor_window_test` | cf9a167, 7600de5 |

自分のテストで決定に合わせて変えたもの:
- `ToolUiDeclaration.NarrowWindowPagesTheStrips`（段階2）: 560 × 700 の窓は最小未満になり、ストリップのページ送りを見られない。
  560 × 700 では「too small」だけでストリップもページ送りも無いこと、最小の窓ではこのプロジェクトのストリップが 1 ページに入ること
  を確かめる形にし、ページ送りは `ToolUiWindow.MixerPagesAtTheSmallestWindow`（ストリップの多いプロジェクト、最小の窓）に移した。
- `ToolUiFix2.EditorsAskForTheirSmallestWindow`: 640 × 400 の Mixer も「too small」（タブ・フッタなし）を期待するように。
- `ToolUiFix2.HostChangesDropSelectionsWhenTheArraysChange` → `HostChangesDropSelections`: ホストの値だけの変更でも外れることを期待。

モデルに足した・直したもの（今回）:
- `document.hpp/.cpp`: `ClampToU32` / `ClampToU64` / `ClampToI32`、`U32At` / `U64At`。`Create` / `Set` / `SetDefaulted` / `Insert` /
  `Modify` がバンクに入らない数値を拒否（`OutOfBank`）。`AbortGroup` のヘッダ。
- `music_editor.hpp/.cpp`: `MusicEditor::kMaxRequestSeconds` と `Preview` の範囲、`TempoMap::Musical`・`Tempo`・`Markers`・`Rules` の変換。
- `mixer.hpp/.cpp`: `LiveValue::effectChain` と `Locate`（`Unmatched` / `Rebind`）の比較。
- `mixer_editor.cpp`（スナップショットの値の変換）、`project.cpp`（channels の変換）。

## 決定の案（受け入れ側が D-番号を振る。番号は振っていない）

a. **画面とキー**: 12 面（Mixer、Buses、Event、Params、3D、Music、Material、Patch、Profiler、Replay、Link、Problems）。ミキサーの
   構造の編集はライブのミキサー（段階2）とは別の画面 Buses にした（ライブの値と文書の値の取り消しの履歴が別なので画面で分ける）。
   F2〜F10 に 9 面、Ctrl+PageUp / PageDown（Ctrl+Tab / Ctrl+Shift+Tab）で全部を順に。F11 は Tela の全画面なので使わない。
b. **取り消しの文脈**: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z はミキサー画面ではライブの値の履歴（段階2 のまま）、ほかの画面では文書の履歴。
   文書の Undo / Redo / Save はフッタのボタンにもある（件数つき）。Delete / Backspace は画面が選んでいる物（クリップ、カーブの点、
   パッチのノード）を消す。
c. **文字の入力の回避（1 つに揃えた）**: ホストがキーを観測して `App::Key` に渡し、ツールの `TextEntry` が文字列を組む。名前は
   連番で作って欄で変える、数値は ± / 縦のドラッグ（4 px で 1 ステップ、Shift 1/10）/ ホイール / 動かさずに押して入力、参照と選択肢は
   候補の一覧（重ね表示、外か Esc で閉じる）。確定はモデルの関数（拒否なら開いたまま）。Windows の Esc は入力中・一覧のときだけ
   Tela に渡さない。
d. **ドラッグの意味**: 位置のドラッグ（クリップ、点、ノード、リスナー）は押した点からの絶対量（Shift は効かない）、数値のドラッグは
   移動量の積み上げ（Shift 1/10）。文書の編集は最初の移動でグループを開き、離した時に閉じる（1 手）。離した時の追加の編集
   （クリップを別のトラックへ）も同じ 1 手。カーブの点のドラッグは押した時の目盛りのまま（図が拡大縮小しても値が跳ばない）。
e. **クリップを上下に動かすと別のトラック**: 離した時に縦の移動量をレーンの間隔（48 px）で丸めたトラックへ、そのときの開始位置で。
f. **ヘッダの高さは 40 のまま**: 2 段にするとミキサーの縦の寸法が変わり、既存の `ToolUiSnapshot.LiveMixerAndProfilerImages`
   （メータの棒の上端の画素）が丸めの 1.6 px で外れた（アサーションは変えない）。そこでタブと接続だけをヘッダに置き、プロジェクト名・
   未保存・文書の Undo / Redo / Save・問題の数はフッタに移した。
g. **ミキサーと文書の結び直し**: 文書のバス・カテゴリ・パラメータの構成（GUID、名前、親、バンク、エフェクトの種類、スコープ）が
   変われば、すぐにセッションのプロジェクトを文書から作り直してミキサーを結び直す（`MixerModel::Bind`。ライブの上書きは消える）。
   値（音量、エフェクトの値、範囲）だけの変更は、ミキサーに上書きが無ければ結び直し、あれば一度だけメッセージを出して、書き戻すか
   全部戻すまで待つ。書き戻し（`WriteBackMixer`）の後は結び直す。
h. **文書があるときのビルド不一致の既定は「聞く」**（`LiveDecision::Ask`）: ヘッダと Link 画面に質問（差分をすべて送る／拒否）。
   `--send-diffs` で即答、`--refuse-mismatch` で拒否。文書が無いときは段階2 のまま（ミキサーの値を送る）。
i. **再生のサービス**: UI のライブラリは核だけに依存させ、ランタイムを組み立てる `Sonus::tool_preview` は別のライブラリ
   `sonus_tool_ui_replay` からだけ使う（`sonus-tool` は両方をリンク）。
j. **問題の一覧**: `CheckDocument` を文書の版が変わってから 250 ms 以内に 1 回（ドラッグ中に毎フレームはビルドしない）。
   エラーが先、警告が後。ライブの問題と「ライブに出せないもの」も同じ一覧に。Go でそのオブジェクトの編集画面へ。
k. **要素の ID**: 文書のオブジェクトは `<画面>.<…>.<GUID 32 桁>`、オブジェクトの中は配列の番号（`ev.t<トラック>.c<クリップ>`、
   `mu.rule.<行>`、`pa.node.<ノード id>`）。番号は「そのオブジェクトの中の位置」で、一覧の行（Problems、ログ、候補）だけが表示の行番号。
l. **大きいものの収め方**: 一覧はページ送り、タイムラインは見えているクリップだけに当たり判定（レーンごとに 1 枚の図）、組の表は
   8×8 の窓、ルール表はページ。図はレーン・図ごとに `SafeDrawing` の上限の中（超えた分は描かない）。
m. **3D の試聴の追従**: 試聴中にリスナー・エミッタ・向きを動かすと、そのたびに `AuditionRouter::Update3D`（ゲームが宛先なら
   「動かせない」を理由として出す）。

## 仕様の穴の仮決定

- 文字の入力の上限は 256 バイト（`TextEntry::kMaxBytes`）。数値の入力は数字・符号・小数点・e だけ受け付ける（inf / nan / 16 進は拒否）。
- 新しいオブジェクトは最初のバンクに入れる（バンクが要る種類: event、parameter、segment、playlist、materialTable、patch、snapshot、
  ducking）。新しいパッチには出力のノードを 1 つ置く。新しい Event にはトラックを 1 本。新しい音楽の Event は `"music": {}`。
  新しい材質表は材質 `default` を 1 つ。
- プロファイラのタブは開いた時にその View を購読に足す（`EnableProfilerStreams`）。閉じても外さない（段階2 の View は常に購読）。
- 記録した実行との比較のストールのブロック番号は `clockSample / blockSize` で近似（`StreamStall` にブロック番号が無い）。
- Event の一覧は全 Event（音楽の Event は「(music)」）、音楽の画面の一覧は `music` を持つ Event だけ、3D の画面は全 Event（3D に印）。

## 契約の変更点

なし（`contract/` に触れていない。`api/compose/c_api.cpp` にも触れていない）。

## 他の持ち場への依頼

なし（`requests/C-30.md`〜`C-39.md` は使っていない）。画面に要るもので足りないものはモデル（同じ `tool/` の組 M・P）の範囲だったので、
下の「モデルへの要望」に書いた。

## モデルへの要望（組 M・P へ。モデルは変えずに、今ある公開の関数で作った）

1. `EventEditor::SetClipSource(t, c, asset | event | synth)`: クリップの素材を替える関数が無い（今は削除して足し直すしかないので、画面には
   付けていない）。
2. 作成の関数（今は画面が初期のメンバを組んで `ProjectDocument::Create` を呼ぶ）: `CreateEvent(name, bank)`（トラック 1 本）、
   `CreateParameter(name, bank, min, max, default)`、`CreateSegment / CreatePlaylist / CreateMusicEvent / CreateMaterialTable /
   CreatePatch(name, bank)`（パッチは出力のノードつき）。規則（バンクが要る、材質が 1 つ要る）をモデルに置くため。
3. 読み出し（今は画面が JSON のパスを読んでいる）: トラックのバス・layerParameter・flags、クリップの switchValue・layer・synth
   （`SynthDesc`）・素材、バスの項目とセンドの一覧、ダッキングの項目、カテゴリの項目、グループの項目、`MusicEditor::Stingers()` と
   State の Playlist、`PlaylistEditor::Entries()`、`SegmentEditor::Stems()`、パッチの選択肢のパラメータの名前（`Param` は番号を返す）。
4. `MixerModel::Rebind(project)`: オブジェクトのキーで上書きを引き継いで結び直す関数（今は上書きがある間は値の変更で結び直さない。
   決定の案 g）。
5. 記録した実行のログを作る関数 `RecordedRunLogOf(profiler, streams, blockSize)`（今は画面が作る。ストールのブロック番号は近似）。
6. 問題の一覧の速さ: `CheckDocument` を文書全体で走らせている。大きな文書では編集ごとに重い（LiveSync も編集ごとにビルドする）。
   変わったオブジェクトだけの検査か、LiveSync の `current().issues` を使い回す口があるとよい。
7. `SpatialPreview` の速度（`SetEmitterVelocity`）と外挿は画面に出していない（ドップラーがランタイムに無い段階なので）。要れば足す。
8. `TextEntry`（`text_entry.hpp`）と `control_math.hpp` の `TimeAxis` / `PlotAxes` / `NumberDrag` は Tela に依存しない。Ergo Studio の
   プラグインでも使えるように核（`Sonus::tool_core`）へ移すとよい（今は `sonus_tool_ui` の中）。

## ライブでつないでいない箇所

- 画面から試聴できるのは、Event（2D）、3D（動かすと追従）、音楽の遷移。ゲームが宛先のときは、エミッタの移動・リスナー・ブロックに
  合わせた遷移ができない（`AuditionRouter` が理由を返し、フッタに出す）。
- マテリアル表（PHYS、C-11）とパッチ（XPCH / PTCH、C-10）の編集は文書には入るが、ライブには `LiveSupportOf` の「未対応」として
  出る（Problems に一覧）。ランタイムが使わないカーブの対象・compose も同じ。
- 「Listen」（再生の出力をデバイスで聞く）は `OutputPlayer` に渡すだけで、コンテナにデバイスが無いので鳴ることは確かめていない。
- Streams タブは 1.1 のゲームだけ（1.0 なら理由を出す）。ストールのブロック番号は近似（上の仮決定）。
- 構造の変更（StructEdit）は組 P のとおり、今のランタイムでは UNSUPPORTED でホットリロードに回る。Link 画面の予備容量の行と
  構造の変更の「Undo」は、ゲームが受け付けた構造の変更があるときだけ意味を持つ（テストでは 0 件のまま）。

## Tela への提案（`hq-handover.md` 3 章の 9 件に足すもの）

10. **キーイベント**（文字でないキー: Enter、Esc、矢印、Delete、F キー、修飾キー）を Runtime 経由で、フォーカスのある要素へ。
    回避: ホストのサブクラス / NSEvent モニタで観測して `App::Key`（9 件の 4 text_input と組で）。
11. **ウィンドウのショートカットを止める口**: WindowsView は Esc でウィンドウを閉じる、F11 で全画面。文字の入力中の Esc と衝突する。
    回避: 入力中だけ Esc を Tela に渡さない（サブクラスで先に取る）。
12. **重ね表示（ポップアップ・モーダル）の層**: z 順、外を押したら閉じる、フォーカスの閉じ込め。回避: 画面全体の排他入力のボタンを
    下敷きにして、その上に一覧のパネルを宣言の最後に置く。
13. **無効の状態**（押せないボタン）: 回避: 「(ラベル)」の文字に置き換える（9 件の 8 要素の見た目と組で）。
14. **スクロールする入れ物**（中身の切り抜きとホイール）: 回避: ページ送りと、図の上のホイールを自分で受ける。
15. **要素の数（4096）と図形の数（256 / 4096 点）の上限を上げるか、仮想化**: 長いタイムラインと大きな表で上限に当たる。回避:
    レーンごとに図を分ける、見えている物だけに当たり判定、ページ送り、間引き。
16. **文字の幅の計測**: ラベルが切れるかどうか分からない。回避: バイト数で切る（`Clip`）。
17. **ホバーとツールチップ**（切れたラベルの全文、図の上の値）。回避: なし（値は欄や表に出す）。
18. **要素ごとのカーソルの形**（移動・つまみ・リサイズ）。回避: なし。
19. **要素の間のドラッグ＆ドロップ**（端子から端子へ）。回避: 離した点を画面の側で端子の位置と照らし合わせる。
20. **canvas の中の当たり判定**（図形ごとのヒット）: 回避: 図形ごとに文字なしの排他入力のボタンを下に置く（9 件の 2 ElementPointer と組で）。

## Ergo Studio の sonus プラグインに足せる画面

核のモデル（UI に依存しない、`Sonus::tool_core`）をそのまま使える。画面ごとに使うモデル:

| 画面 | モデル |
| --- | --- |
| Event エディタ | `EventEditor`（タイムラインは `ComputeTimeline`）、`ProjectDocument`、`CheckDocument` |
| ミキサーの構造 | `MixerEditor`（`GraphOrder`、`ApplySnapshots`、`CaptureSnapshot`）、`WriteBackMixer`、ライブの値は `MixerModel` |
| パラメータとカーブ | `ParameterEditor`、`CurveEditor`、`ComposeEvent`、`RampTrajectory`、`EvaluateCurve` |
| 3D プレビュー | `SpatialPreview`（表示の変換、`Compute`、`Audition`）、`AuditionRouter` |
| 音楽 | `SegmentEditor`、`PlaylistEditor`、`MusicEditor`（`Rules` / `Unreachable` / `AutoSort` / `Preview` / `MinimumLookahead`）、`TempoMap`、`TapTempo` |
| マテリアル表 | `MaterialEditor`（`Resolve`、`Fallbacks`）、`EvaluateStrength` |
| パッチ | `PatchEditor`（`Check`、`Compiled`） |
| プロファイラ | `Profiler`、`ProfilerStreams` |
| 記録と再生 | `recording_report`（要約、比較、期待、不具合報告）。再生そのものは `Sonus::tool_preview`（`replay_host`） |
| 接続 | `Session`、`LiveSync`（`check` / `items` / `problems` / `reloads` / `reserve`）、`Connection` |
| 共通 | `ProjectDocument`（Undo / Redo / Save / `ReferencesTo`）、`CheckDocument` / `IssuesOf` |

UI 側で Tela に依存しないもの（プラグインでも使える、要望 8）: `TextEntry`、`TimeAxis` / `PlotAxes` / `NumberDrag` / `RulerStepMs`、
`FaderDbToPos` などの `control_math`。

## 回した構成と結果

### 差し戻し 3 回目の後（8be8784。ASan の回は cf9a167 = 文書のコミットの前、コードは同じ）

ビルドは `$S/ui/b-*`、-j2、1 つずつ（ログは `$S/ui/r3/*.log`、終わったら消した）。警告はどの構成も 0。

| 構成 | 実行したテスト | 結果 |
| --- | --- | --- |
| 9: gcc Debug | 全部（434） | 全部通過。組 U の `Defect_*`（1〜5、N1〜N6、P1〜P5）と組 P の `Defect_*` も全部通過 |
| 9 ＋ clang | `ctest -R tool`（216） | 全部通過 |
| 9 ＋ ASan / UBSan ＋ `float-cast-overflow`（`-DSND_SANITIZE=address,undefined,float-cast-overflow`、`-fno-sanitize-recover=all`） | `ctest -R tool`（216） | 全部通過。報告 0（前回の 3 件 `music_editor.cpp:107`・`project.cpp:294`・`music_editor.cpp:254` も出ない。`tool_indep_s7_robust_test`・`tool_indep_model_test` 通過） |
| 9 ＋ TSan | 並行性 5 本、`tool_ui_editor_concurrency`・`tool_indep_s7_ui_threads`・`tool_ui_concurrency`・`tool_indep_concurrency` を -j1 で 20 回 | 全部通過、報告なし |
| 9 ＋ Release | `ctest -R tool`（216）、同じ 4 本を -j1 で 20 回 | 全部通過 |
| 8: 核だけ | `ctest -R tool`（80） | 全部通過 |

MinGW の構文検査: 今回変えたファイル 15 本（`tool/src` の document・music_editor・mixer・mixer_editor・project、`tool/ui/src` の app・
patch_view・profiler_view・mixer_view・music_view、テスト 5 本）は全部通過。


### 差し戻しの後（fab42a9 = 組 P の差し戻し c/live を取り込んだ後）

ビルドは `$S/ui/b-*`、-j2、1 つずつ（終わったら消した）。警告はどの構成も 0。

| 構成 | 実行したテスト | 結果 |
| --- | --- | --- |
| 9: gcc Debug | 全部（393） | 386 通過。失敗 7 = 組 P の新しい欠陥 `tool_indep_s7_live_Defect_*` 4 件（c/live で失敗のまま取り込んだもの）と、決定と食い違う組 U の独立テスト 3 件（`tool_indep_s7_ui_common_test`、`tool_indep_s7_ui_screens_test`、`SpecGap_ClipSelectionIsAnIndexAcrossUndo`、上の一覧）。組 U の `Defect_*` 5 件は全部通過。前の回の livelink の独立テスト 2 本も通過 |
| 9 ＋ clang | `ctest -R tool`（175） | 168 通過、失敗は同じ 7 件 |
| 9 ＋ ASan / UBSan | `ctest -R tool`（175） | 168 通過、失敗は同じ 7 件。ASan / UBSan の報告なし |
| 9 ＋ TSan | 並行性 5 本（`tool_ui_concurrency`・`tool_indep_concurrency`・`tool_ui_editor_concurrency`・`tool_indep_s7_ui_threads`・`tool_ui_live_screens`）、`tool_ui_editor_concurrency` と `tool_indep_s7_ui_threads` を -j1 で 20 回 | 全部通過、報告なし |
| 9 ＋ Release | `ctest -R tool`（175）、同じ 2 本を -j1 で 20 回 | 168 通過・失敗は同じ 7 件。20 回は全部通過 |
| 8: 核だけ | `ctest -R tool`（78） | 74 通過、失敗 4 = 組 P の `Defect_*` 4 件だけ |

MinGW の構文検査: 差し戻しで変えたファイル（`tool/ui/src/*.cpp` 全部、`tool/src/mixer.cpp`・`event_editor.cpp`・`doc_build.cpp`、
`host_windows.cpp`、変えた・足したテスト）は全部通過。

### 差し戻しの前

ビルドは `$S/ui/b-*`、-j2、構成は 1 つずつ（終わったら消した。今は残っていない）。「tool の全部」は `ctest -R tool`（90 件のうち
Disabled 2 件を除く 88 件、核だけの構成は 49 件）。

| 構成 | コミット | 実行したテスト | 結果 |
| --- | --- | --- | --- |
| 9: Tela 入り、gcc Debug | e4a924e | 全部（305） | 303 通過。失敗 2: `livelink_indep_s7_impl_test`（`IndepS7Impl.StructEditBeyondTheLogIsRefusedAtAcceptance`）、`livelink_indep_s7_recheck_test`（`IndepS7Recheck.StructLogAndReserveFollowTheModel`）。どちらも livelink だけのテストで、組 U は livelink を変えていない（下の未検証） |
| 9: Tela 入り、gcc Debug | 5f2c311 | tool の全部（88） | 全部通過。警告 0 |
| 9 ＋ clang | 5f2c311 | tool の全部（88） | 全部通過。警告 0 |
| 9 ＋ ASan / UBSan | 5f2c311 | tool の全部（88） | 全部通過。警告 0。（0775893 の時点の回では `tool_ui_music_screen_test` が heap-buffer-overflow: State を足すボタンが `States()` の 2 つの一時オブジェクトの反復子を比べていた。5f2c311 で直した） |
| 9 ＋ TSan | 5f2c311 | `tool_ui_concurrency`・`tool_indep_concurrency`・`tool_ui_editor_concurrency`・`tool_ui_live_screens`、`tool_ui_editor_concurrency` を -j1 で 20 回 | 全部通過（TSan の報告なし）。警告 0 |
| 9 ＋ Release | 5f2c311 | tool の全部（88）、`tool_ui_editor_concurrency` と `tool_ui_live_screens` を -j1 で 20 回 | 全部通過。警告 0 |
| 8: 核だけ（Tela なし） | 5f2c311 | tool の全部（49） | 全部通過。警告 0（UI のテストは入らない） |

MinGW（`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror -Wall -Wextra -fno-exceptions -fno-rtti`、Tela のインクルードパス）:
`tool/ui/src/*.cpp` 全部、`tool/app/main.cpp`・`options.cpp`・`host_headless.cpp`、`host_windows.cpp`（`-fexceptions`、Tela の
`windows_view.hpp`）、新しいテスト 12 本: 全部通過（ccb6aea、その後に変えた `tool/ui/src/*.cpp` 全部と変えたテスト 3 本は 5f2c311 で検査し直して通過）。`host_macos.mm` は検査していない（Objective-C++ のコンパイラが無い）。
構成 1〜7（ツールなし）は組 U の差分が `tool/` だけなので回していない（受け入れ側の全構成の確認に任せる）。

## 未検証の箇所

- Windows / macOS のホストで画面が出て操作できること（キーの観測、WM_CHAR、NSEvent の文字、Esc の扱い）。MinGW の構文検査だけ。
  `host_macos.mm` は構文検査もしていない。
- 文字の描画（Pictor）で文字が欄に収まるか（ヘッドレスは文字の帯だけ）。ラベルはバイト数で切っているので、全角の文字は早めに切れる。
- 実デバイスでの試聴・「Listen」。
- 1.0 のゲームで Streams タブ・Read back が理由つきで無効になること（理由の文は `ProfilerStreams::Available` と `Session::ReadBack`
  の結果を出しているが、1.0 のゲームでの画面のテストはしていない）。
- ゲームが宛先のときの試聴（3D の追従ができない理由の表示）は、宛先の切り替えと「接続していない」の理由だけテストした。
- ライブの構造の変更（予備容量の棒、StructEdit の Undo）とホットリロードの経過の行が出ること（ランタイムが UNSUPPORTED を返し、
  テストの編集はどれも値の編集なので、テストではホットリロードの行も構造の変更の行も 0 件のまま）。
- 大きな文書での速さ（編集ごとの `CheckDocument` と LiveSync のビルド）。計っていない。
- 画素のテストのうち、ヘッドレスの 12 面の画像は書けることだけを見ている（中身の画素はエディタのテストで個別に見た）。
- 差し戻し: Windows のホストで Esc が Tela に渡らずウィンドウが閉じないこと（コードと MinGW の構文検査だけ）。ミキサーの結び直しの
  質問のあとのゲームとツールのずれ（ライブで上書きを捨てた・書き戻した後に、ゲームの値がツールと同じになること）は確かめていない。
- livelink の独立テスト 2 本の失敗が取り込んだ状態からのものであること（出発点 ce1aa3e で回し直してはいない。組 U の差分に livelink は無い）。
- 差し戻し 3 回目: 最小の窓は宣言の位置（ヘッドレス）で確かめた。実際の OS の窓の最小の大きさ（Windows / macOS のホストに最小の
  サイズを伝えること）は入れていない（窓が小さいと「too small」が出るだけ）。文字の幅（Pictor）でラベルが欄に収まるかは見ていない。
  `FiniteText` は名前の語と数字の語を区別しないので、「inf」「nan」だけの語の名前は `Kit::Label` を通る所で「-」になる。
  2 回目の差し戻し（N1〜N6）の時に回しかけた構成はコンテナの再起動で途中で止まり、結果は残っていない（3 回目の上の表で全部を回し直した）。
