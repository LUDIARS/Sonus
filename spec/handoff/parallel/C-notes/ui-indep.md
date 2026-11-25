# セッション C 組 U（Tela の画面）独立テストのメモ

独立テスト担当のメモ。受け入れ側への引き継ぎ。ブランチ `c/ui`（実装担当のコミットと組 L の最新の取り込み 9425a07 の上、ローカルコミットのみ）。

手順: 実装を読む前に、仕様（設計書 11 章、`tool/README.md`、CONTRACTS.md の D-67・D-67a・D-100・D-101）、公開ヘッダ
（`tool/ui/include`、`tool/include`、`tool/preview/include`、Tela）、依頼書 `U-impl.md`、実装担当のメモの「作ったもの」と
「決定の案」から性質を書き出してコミットした（d64204d、`tool/test/indep_s7_ui_properties.md`）。要素の ID は実装を読まずに、
実行時の要素の木（ID・ラベル・位置）から探した。そのあとテストを書き、最後に実装（`tool/ui/src`）を読んで欠陥を狙うテストを足した。

## 作ったもの

書いた場所は `tool/test/` の新しいファイル（`indep_s7_ui_*`）と `tool/test/CMakeLists.txt` の末尾への登録、性質の一覧、このメモだけ。
実装・既存のテストは変えていない。

| ファイル | 内容 |
| --- | --- |
| `indep_s7_ui_properties.md` | 性質の一覧（A〜K） |
| `indep_s7_ui_fixture.hpp` | 文書の上の App（`Rig`）、要素の ID・接頭辞・ラベルでの検索、押す・打つ・ドラッグ、大きなプロジェクトの生成（`LargeFiles`: Event 1000、40 トラック × 30 クリップ、開始 1e9 ms、長い名前 200 文字、バス 100、ルール 150 行、ノード上限 32 のパッチ、材質 64、範囲 ±1e30 のパラメータ）、要素の重複・Tela の上限の検査、`SND_INDEP_DUMP=1` で要素の木を出す |
| `indep_s7_ui_common_test.cpp` | A（画面の名前・タブの順・F キーと Ctrl+PageUp/Down/Tab・画面の切り替えで入力が終わる）、B（画面ごとの Ctrl+Z / Y / Shift+Z、ミキサーはライブの値だけ、Ctrl+S と未保存、文書なし）、C（UTF-8、ParseNumber、FormatNumberText、TextEntry のキー・上限・数値・置き換え、Esc） |
| `indep_s7_ui_screens_test.cpp` | D・E（Event の一覧と全ページ、クリップの箱が `ComputeTimeline` と軸のとおり、横のドラッグの絶対量と 1 手、縦のドラッグ、トラックのボタン、Delete、Undo 後の選択、名前の欄の拒否、数値の欄の規則、Buses の一覧・拒否される編集（マスターの削除、子のあるバス、循環する親、自分へのセンド）・+ Child とミキサーの追従・エフェクトの値の正規化、Params の項目と `ComposeEvent`、カーブの点のドラッグ・隣を越えない・2 点で Delete、3D の値と `Compute`・エミッタのドラッグ、Music のルール表の行数・自動の並べ替えの拒否・試聴の計算と最小先読み、Material の行・最後の材質・対称な組、Patch の上限・循環する接続・ノードのドラッグ、Problems の行と Go、文書なしのライブの画面） |
| `indep_s7_ui_gesture_test.cpp` | F（多くの宣言をまたぐドラッグ、ドラッグ中の画面の切り替え・Tela の取り消し・要素が消える・Ctrl+Z、動かさない押下、数値の欄のドラッグ・Shift・ホイール・整数の範囲、数値のドラッグ中の画面の切り替え、ノードのドラッグ中の取り消し） |
| `indep_s7_ui_limits_test.cpp` | G・E（全 12 画面 × {文書なし、空、基本、大きい（7 つの選択）} × DPI 0.25〜4 × 320×200〜3840×2160 を子プロセスで: 落ちない、要素 ≤ 4096、ID の重複なし、図形 ≤ 256・点 ≤ 4096、位置が有限、compose。範囲外の DPI と大きさ。フレームと画面の往復で ID が同じ）。ヘッドレスの CTest 用に大きなプロジェクトと空のプロジェクトを書く |
| `indep_s7_ui_raster_test.cpp` | H（クリップの箱の画素、maxDistance の円の半径、カーブの点。DPI 1 と 2） |
| `indep_s7_ui_threads_test.cpp` | J（実時間のプレビューエンジン（Silent）とライブリンクの中で、UI スレッドが全画面をポインタとキーで操作） |
| `indep_s7_ui_defects_test.cpp` | 欠陥 5 件（`Defect_*`、失敗のまま）、書き戻しの確認 1 件、仕様の穴の記録 3 件（`SpecGap_*`、通る。今の振る舞いを出力する） |
| `CMakeLists.txt`（末尾に追記） | 上のテストの登録、`tool_indep_s7_ui_screen_{large,empty}_<画面>`（24 本、`sonus-tool --headless --screen` が画像を書くこと）、知らない画面名 5 本（WILL_FAIL）、欠陥のテストを 1 件ずつ |

## 性質の一覧と結果

番号は `indep_s7_ui_properties.md`。結果は構成 9（gcc Debug、Tela 入り）。

| 番号 | 要約 | テスト | 結果 |
| --- | --- | --- | --- |
| A1〜A3 | 画面の名前の往復、タブの順、F2〜F10・Ctrl+PageUp/Down/Tab | `IndepS7Ui.ScreenNames…`、`TabOrder…`、`FunctionKeys…` | 通過 |
| A4 | 画面の切り替えで入力が終わる（commit しない） | `ChangingTheScreenEndsTheTextEntry` | 通過 |
| B1・B2 | 画面ごとの取り消しの文脈 | `CtrlZUndoesTheDocumentOnEditorScreens`、`CtrlZOnTheMixerUndoesLiveValuesOnly` | 通過 |
| B3・B4 | Ctrl+S と未保存、文書なし | `CtrlSSaves…`、`NoDocument…` | 通過 |
| C1〜C4 | UTF-8、数値の読み書き、TextEntry | `Utf8…`、`ParseNumber…`、`FormatNumberText…`、`TextEntryKeys`、`AppEscape…` | 通過 |
| C5 | 名前の欄（重複・空の拒否、1 手、ID が変わらない） | `EventNameFieldRefusesTakenNames` | 通過 |
| C6 | 数値の欄がモデルの規則を通る | `EventNumberFieldsGoThroughTheModelsRules`（通過）、`Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused` | **欠陥**（入力した範囲外の値が切り詰めて書かれる） |
| D1 | Event | `EventList…`、`EventClipBoxes…`、`EventClipDrag…`、`EventClipVerticalDrag…`、`EventTrackButtons…`、`EventDeleteAfterUndo…`、`EventSelectedTrackRemovedFromOutside` | 通過（縦のドラッグは欠陥 3） |
| D2 | Buses | `BusesList…`、`BusesScreenDoesNotPassRefusedEdits`、`BusesAddChild…`、`BusesEffectParam…` | 通過 |
| D3 | Params | `ParamsFieldsAndComposition…`、`ParamsCurvePoints…` | 通過（狭い欄は欠陥 4） |
| D4 | 3D | `SpatialValuesAndEmitterDrag` | 通過 |
| D5 | Music | `MusicRuleTableRowsAndAutoSort`、`MusicPreview…` | 通過 |
| D6 | Material | `MaterialRows…` | 通過 |
| D7 | Patch | `PatchLimitAndCycleAreRefused`、`PatchNodeDrag…` | 通過 |
| D8 | Profiler / Replay / Link（接続・サービスなし） | `LiveScreensWithoutAGameSayWhy` | 通過 |
| D9 | Problems（行の数、エラーが先、Go） | `ProblemsRowsAreTheBuildsIssues` | 通過 |
| D10 | 画面だけの選択と文書のずれ | `EventDeleteAfterUndo…`、`EventSelectedTrack…`、`SpecGap_ClipSelectionIsAnIndexAcrossUndo` | 通過。ただし仕様の穴 1 |
| E1〜E5 | ID の安定と一意 | `IdsAreStableOverFramesAndRoundTrips`、`EventNameField…`（名前の変更）、`EveryScreen…`（重複なし） | 通過 |
| F1・F2・F9 | ドラッグの量と値、範囲、動かさない押下 | `EventClipDrag…`、`EventClipVerticalDrag…`、`PressWithoutMotionIsNoStep` | 通過（欠陥 3 を除く） |
| F3〜F5 | 点・エミッタ・ノード | `ParamsCurvePoints…`、`SpatialValues…`、`PatchNodeDrag…` | 通過 |
| F6 | 数値の欄のドラッグとホイール | `NumberFieldDragAndWheel`（通過）、`Defect_NumberFieldWheelNotchesBetweenFramesAreLost` | **欠陥**（フレームの間の複数のノッチが 1 つになる） |
| F7・F8 | 宣言の差し替えをまたぐ、中断（画面の切り替え、取り消し、要素が消える、Ctrl+Z） | `IndepS7UiGesture.*` | 通過（グループは閉じ、手は高々 1 つ、次の編集は別の手） |
| G1〜G3 | 上限・DPI・大きさ | `EveryScreenEveryProjectEveryDpiStaysInTelasLimits`、`OutOfRangeViewportsAreClamped` | 通過（ASan / UBSan は下の構成の表） |
| H1〜H3 | 画素（DPI 1・2） | `IndepS7UiRaster.*` | 通過 |
| I1・I2 | ヘッドレスの全画面（大きい・空）、知らない画面名 | `tool_indep_s7_ui_screen_*`、`tool_indep_s7_ui_bad_screen_*`、`tool_indep_s7_ui_screen_missing_name` | 下の構成の表 |
| J1 | UI スレッドと接続・エンジンのスレッド | `IndepS7UiThreads.ScreensDrivenWhileEngineAndLinkRun` | 下の構成の表 |
| K | 実装を読んだ後の追加 | `indep_s7_ui_defects_test.cpp` | 欠陥 5（うち 1 件は ASan の構成で見つけた Pick）、記録 3、確認 1（通過） |

## 欠陥の一覧（`tool_indep_s7_ui_defects_test`、CTest では `tool_indep_s7_ui_<名前>`）

1. **`Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused`**（中）
   - 再現: Event 画面で Shot を選び、Max dist. の値（`ev.f.n4`）を押して入力を始め、`-5` を打って Enter。クリップの Start でも `-20`。
   - 期待: モデルが拒否する（maxDistance ≥ 0、startMs ≥ 0）ので文書は変わらず、入力は開いたまま（kit.hpp「Enter commits through the
     model's setter (refused: the entry stays open)」、text_entry.hpp、README「確定はモデルの関数（拒否なら開いたまま）」）。
   - 実際: 0 が書かれ（1 手）、入力が閉じる。`kit.cpp` の `commitText` が `ClampNumber(v, sp.minValue, sp.maxValue, …)` で欄の範囲に
     切り詰めてから setter を呼ぶので、モデルは拒否の機会がない（画面の側の規則。打った値と違う値が黙って入る）。欄の範囲が
     モデルの規則より狭い欄（例: Priority 0..255 なら 300 → 255）でも同じ。
   - 実装担当のテストの「拒否」は文字の無い入力（読めない文字列）だけで、読める範囲外の値を試していない。
2. **`Defect_NumberFieldWheelNotchesBetweenFramesAreLost`**（中）
   - 再現: Buses 画面で Sfx を選び、Volume の値の上でホイールを 5 ノッチ、間に宣言を挟まずに送る（速いホイール、ホストがまとめて
     渡すとき）。
   - 期待: 5 ステップ、1 手（kit.hpp「the wheel steps (one undo step per series)」、README「ホイール（続けて回した分が取り消し 1 回）」）。
   - 実際: 1 ステップ。`kit.cpp` のホイールは宣言の時の `sp.value` に足すので、次の宣言までの 2 つ目以降のノッチが同じ値を書く。
     ミキサーのフェーダーとノブ（`pointer_router.cpp`）は `ControlValue` で今の値を読むので起きない。実装担当のテストは
     ノッチごとに Frame を呼ぶので見つからない。
3. **`Defect_ClipVerticalDragTrackComesFromThePressPointNotTheMotion`**（中）
   - 再現: 3 トラックの Event で中央のトラックのクリップを、上端から 2 px のところで押して 5 px 上へ動かして離す（下端近くで押して
     10 px 下も同じ）。
   - 期待: 縦の移動が半レーン（24 px）未満ならトラックは変わらない（決定の案 e「縦の移動量をレーンの間隔（48 px）で丸めたトラックへ」）。
   - 実際: 上のトラックへ移る。`event_view.cpp` の離した時の処理が `floor(離した点の当たり判定の中の y / 48)` で、移動量ではなく
     押した点の位置で決まる（上端の近くで押せば 1 px 上でも移り、下端の近くでは 13 px 下で移る）。実装担当のテストは中央を押して
     ちょうど 48 px 動かすだけ。
4. **`Defect_NumberFieldValueHasNoRoom`**（低）
   - 再現: 1280×800、Params 画面（パラメータ Speed）と Material 画面（Phys）。
   - 期待: 数値の欄の値の要素に、値を出して押せる幅がある（kit.hpp「a vertical drag on the value」「a click without motion types」）。
   - 実際: `pc.par.def.value` 2 px、`pc.par.min/max/ramp.value` 16 px、`ma.t.impulse.value` 10 px。`kit.cpp` は欄の幅を 40 px 以上に
     するが値には欄 − 44 px しか残さない（負にもなりうる。Tela は負の幅で例外を投げなかった）。値の文字が出ず、2 px の当たり判定は
     実質押せない。

5. **`Defect_PickWithAnItemOfTheOpenListReadsFreedMemory`**（中、ASan で発見）
   - 再現: 候補の一覧を開き（`OpenPicker`、または画面の `PickField` を押す）、`app.Pick(app.picker()->items[i].value)` を呼ぶ。
   - 期待: pick の関数がその項目の値を受け取る（app.hpp の `Pick(const std::string&)` と `picker()` の組み合わせで素直な呼び方）。
   - 実際: `App::Pick` が `picker_.reset()`（項目を解放）してから `fn(value)` を呼ぶので、`value` は解放した領域を指す。ASan では
     heap-use-after-free（パッチ画面の「+ Node」の pick から `PatchEditor::AddNode` → `FindNodeKind` で読んだ）、ASan なしでは
     受け取った値が壊れる。画面の一覧のボタンは値を写して捕まえるのでクリックでは起きないが、ホスト（Ergo のプラグイン）やテストが
     呼ぶと起きる。直し方の例: `Pick` の中で `value` を先に写す。

（確認して欠陥でなかったもの: `WriteBackAfterStaleMixerKeepsNewerDocumentValues` — ライブの上書きがある間に文書で別のバスの値を
変えてから書き戻しても、上書きの無いバスの新しい値は消えない。通過。）

## 仕様の穴の一覧（選択肢と推奨）

1. **選択が番号（クリップ、トラック、カーブの点、アクション、ルール）で、Undo / Redo の後に別の物を指す**
   （`SpecGap_ClipSelectionIsAnIndexAcrossUndo`）: クリップ A・B から A を消して B を選び、Ctrl+Z で A が戻ると選択は番号 0 = A になり、
   Delete で A（選んでいない方）が消える。選択肢: (a) 今のまま（強調は A に移るので見た目とは合う）、(b) 文書の版が画面の外
   （Undo / Redo・他の画面・ライブ）で変わったら番号の選択を外す、(c) オブジェクトの中の要素に安定したキーを持たせる（モデル）。
   **推奨 (b)**（データを消す操作の対象が黙って入れ替わらない。安い）。
2. **入力中の Ctrl+Z / Ctrl+Y が文書に効く**（`SpecGap_UndoKeyDuringATextEntryUndoesTheDocument`、text_entry.cpp「shortcuts pass」）:
   入力は開いたまま、下の文書が 1 手戻る。選択肢: (a) 今のまま、(b) 入力中は取り消し・やり直しを入力が食う（入力を取り消す）、
   (c) 入力を確定してから戻す。**推奨 (b)**（入力欄で Ctrl+Z を押す人は打った文字を消したい。開いた入力の commit は番号で物を
   指すことが多く、下で文書が変わると別の物に書く）。
3. **文書の構造の変更でライブの上書きが黙って消える**（決定の案 g、`SpecGap_StructuralEditDropsLiveOverridesSilently`: 上書き 1 → 0、
   メッセージ 0）: 選択肢: (a) 今のまま、(b) メッセージを出す、(c) モデルの要望 4（キーで引き継ぐ `Rebind`）が入るまで、上書きが
   あるときは値だけの変更と同じく待って質問する。**推奨 (c)、少なくとも (b)**。ゲームには上書きの値が残ったまま、ミキサーは
   バンクの値を出すので、ツールとゲームがずれる（11.2「正しいデータは常にツール側」）。ライブでのずれは確かめていない。
4. **クリップの縦のドラッグがトラックの範囲の外**: 今は何もしない（元のトラックのまま、横の移動は残る）。選択肢: 端のトラックへ
   切り詰める／何もしない。**推奨: 端へ切り詰める**（決定の案 e の「丸めた」に素直）。テストはどちらも許している。
5. **コーンの「無い」の表し方が画面で違う**: Event 画面は `cone` の無い Event を 360 / 360 / 0 dB と出し（画面の側の既定）、3D 画面の
   情報行は `SpatialPreview::Cone` の 0 / 0 / 0 dB を出す。ランタイムではどちらも「コーンなし」（`sys_dsp.cpp`: outer > 0 のときだけ）。
   **推奨**: モデルに `EventEditor::Cone()` を足し（実装担当の要望 3 と同じ）、両画面が同じ値を出す。
6. **ID が位置の番号になる名前つきの要素**（決定の案 k）: 材質（`ma.mat.<番号>`、`ma.pair.<a>.<b>`）、State（`mu.state.<番号>`）、
   マーカー。材質を 1 つ消すと残りの材質の ID が全部ずれる（E3 に反するが、決定の案 k の範囲）。**推奨**: 名前で一意なもの（材質、
   State、マーカー、Stinger）は名前から ID を作る。名前の無い配列の要素（クリップ、点、ルール）は番号のまま。
7. **3D の既定の配置で、リスナーの向きの取っ手がエミッタに重なる**（エミッタ 5 m 前、`sp.listener.yaw` 568..582 × 383..397 と
   `sp.emitter` 563..587 × 386.9..410.9）: エミッタの上の部分を押すとリスナーが回る。**推奨**: 取っ手を本体より先に宣言する
   （本体が上）か、既定のエミッタを横にずらす。
8. **端の点が半分切れる**: カーブの x の範囲に余白が無く、最初と最後の点（と当たり判定）が図の端に半分かかる（画素で確認）。
   **推奨**: x にも余白（`PlotAxes::Fit` の y と同じ割合）。
9. **入力も一覧も無いときの Esc**: `App::Key` は Esc で接続待ち（パッチの pendingFrom）を消して false を返すので、Windows のホストは
   Esc を Tela に渡し、Tela がウィンドウを閉じる（README の「Tela が Esc でウィンドウを閉じる」）。接続の待ちを Esc で取り消そうと
   するとツールが終わる。コードを読んだだけで、Windows のホストでは確かめていない。**推奨**: 画面が Esc を使ったとき（pendingFrom を
   消したとき）は true を返す。
10. **Link 画面の未接続の文言**: 接続していないのに「the game speaks protocol 1.0」と出る（`peerMinor()` の 0 を「1.0 のゲーム」と
    読んでいる）。**推奨**: 未接続なら「not connected」。
11. **Problems に同じエラーが 2 行**: 知らない asset を指すクリップで、`CheckDocument` が同じ文言・位置のエラーを 2 件返す（画面は
    そのまま出す）。モデル（buildtool の診断）の側。画面で重ねるかはモデルの担当と決める。

## 実装担当の「決定の案」への賛否

- **a（12 面、Buses をミキサーと分ける、F2〜F10、Ctrl+PageUp/Down）**: 賛成。キーはテスト（A3）のとおり。
- **b（取り消しの文脈: ミキサー画面ではライブの値、ほかでは文書）**: 賛成。2 つの履歴を 1 つのキーで扱うには画面で分けるのが
  いちばん分かりやすく、フッタに文書の Undo / Redo の件数があるので見分けられる。ただし入力中の Ctrl+Z は文書に通さない
  （仕様の穴 2）。
- **c（文字の入力の回避: ホストがキーを観測して `TextEntry`）**: 方式には賛成（1 つに揃っていて、Tela に足すべきもの（提案 10〜12）
  とも合う）。ただし入力した数値を欄の範囲で切り詰めてから commit するのは反対（欠陥 1）: 打った値をそのままモデルに渡し、拒否なら
  開いたままにする。
- **d（ドラッグの意味: 位置は押した点からの絶対量、数値は積み上げ、ドラッグ 1 回 = 1 手）**: 賛成（F1・F6・F7・F8 で確認。中断でも
  グループが閉じる）。ただし数値の欄のホイールは今の値から足す（欠陥 2）。
- **e（上下でトラック）**: 方針に賛成、実装に反対（欠陥 3）: 移動量 dy を `round(dy / 48)` にし、範囲の外は端へ切り詰める（仕様の穴 4）。
- **f（ヘッダの高さ 40、文書の操作はフッタ）**: 賛成（既存のアサーションを変えずに済む）。
- **g（上書きがある間は値だけの変更で結び直さない）**: 値だけの変更で待つのは賛成（書き戻しても上書きの無いバスの新しい文書の値は
  残ることを確認した）。構造の変更で上書きを黙って消すのは反対（仕様の穴 3）。
- **h（文書があるときの不一致の既定は「聞く」）**: 賛成。文書があればツールの側が正しいデータを持っているが、ゲームのビルドが
  新しい場合もあり、黙って差分を送るのも黙って断るのも危ない。ヘッドレスや CI は `--send-diffs` / `--refuse-mismatch` で答えを
  決められる。
- **i（再生のサービスを別ライブラリ）**: 賛成。
- **j（問題の一覧は 250 ms に 1 回）**: 賛成。
- **k（ID: GUID と、オブジェクトの中は番号）**: 名前の無い要素は賛成、名前で一意な要素は名前から（仕様の穴 6）。
- **l（ページ送り・窓・見えている物だけ）**: 賛成（G1 で 320×200〜3840×2160、DPI 0.25〜4、大きな文書で上限の中）。
- **m（3D の試聴の追従）**: 賛成（並行性のテストで試聴しながら動かした。音の中身は確かめていない）。

## 実装担当のテストの弱い箇所

- `ToolUiInput.NumberFieldDragShiftAndWheel`: ホイールのノッチごとに Frame を呼ぶので、フレームの間の複数のノッチ（欠陥 2）が出ない。
- `ToolUiEvent.ClipDragDownMovesItToTheNextTrack`: 中央を押してちょうど 48 px なので、押した位置で決まる誤り（欠陥 3）が出ない。
  範囲の外、半レーン未満の移動も無い。
- 入力の拒否（`ui_event_screen_test` の 107 行目付近、`ui_editor_input_test`）は読めない文字列（空）だけで、読める範囲外の値（欠陥 1）
  が無い。
- `ToolUiLimits.EveryScreenOfALargeProjectAtEveryDpi`: 同じプロセスで回すので、Tela の例外が出ればテストの実行ファイルごと落ちる
  （どの画面かが分からない）。640×400 より小さい窓は無い（本テストは 320×200 も回した）。
- 選択が文書とずれる場面（Undo の後の Delete、入力中の Ctrl+Z）のテストが無い。

## 机上で検証した並行部分と結論

- UI スレッド: `App`（宣言、`PointerRouter`、`TextEntry`、`Picker`）、`Session`（`Tick` の中で `Connection::Poll`、`CellSync`、
  `Profiler`、`LiveSync`、`ProfilerStreams`）、`ProjectDocument`、`SpatialPreview`、`AuditionRouter`、プレビューエンジンの持ち主の関数
  （`App::Frame` の `Pump`）。画面のコードはどれもこの中からだけ呼ばれる（宣言の中の関数オブジェクトは Tela の `pointer` の中、つまり
  UI スレッドで発火する。`App::Pointer` → `PointerRouter::Pointer` → `Runtime::pointer`）。
- ほかのスレッド: ライブリンクのクライアントの受信スレッド（`Copy*` / `Take*` だけで UI と会う、段階2 から）、プレビューエンジンの
  オーディオスレッド（Silent）とサーバのスレッド。組 U の画面のコードは新しいスレッドも共有の状態も足していない（`static` の
  可変な状態は `App::Key` の `constexpr` の表だけ）。
- 結論: 組 U の差分に新しいデータ競合の経路は無い。`IndepS7UiThreads.ScreensDrivenWhileEngineAndLinkRun` を TSan と Release で 20 回
  （下の表）。

## 回した構成と結果

ビルドは `$S/ui-indep/b-*`、-j2、構成は 1 つずつ（終わったら消した）。「tool」は `ctest -R tool`。組 U の独立テストは CTest で 46 件
（実行ファイル 7 本（欠陥の実行ファイルは DISABLED で、欠陥 5・確認 1・記録 3 を 1 件ずつ）、ヘッドレスの全画面 24 本、プロジェクトを
書く 1 本、知らない画面名 5 本）。

| 構成 | コミット | 実行したテスト | 結果 |
| --- | --- | --- | --- |
| 9 ＋ ASan / UBSan | 2e0a455（Pick の欠陥のテストと画面のテストの直しは 05a7e87 で、その 2 本を再実行） | tool のテストの実行ファイルと sonus-tool / sonus-replay だけをビルドし `ctest -R tool`（135 件。`-R tool` に当たる buildtool のテスト 9 件はビルドしていないので Not Run） | 組 U の独立テストで ASan が heap-use-after-free を報告（`App::Pick`、欠陥 5）。テストの側は値を写して渡すようにし、欠陥は `Defect_Pick…` に分けた。それ以外の tool のテストは全部通過（実装担当のテストも）、UBSan の報告なし。失敗は `Defect_*` だけ |
| 9: Tela 入り、gcc Debug | 3c601eb（テストの中身は 2e0a455 と同じ。その後は書式の変更だけ） | 全部（350、Skipped 1・Disabled 3 を除く） | 346 通過、失敗 4 = 組 U の `Defect_*` 4 件だけ（意図どおり。Pick の欠陥 05a7e87 を足す前の回）。警告 0。以前の実装担当の回で失敗していた livelink の独立テスト 2 本は通過 |
| 9 ＋ TSan | 05a7e87 | `tool_indep_s7_ui_threads_test` を -j1 で `--repeat until-fail:20`、実装担当と前の独立テストの並行性のテスト（`tool_ui_editor_concurrency`・`tool_ui_concurrency`・`tool_indep_concurrency`）を 1 回ずつ（必要なターゲットだけビルド） | 全部通過、ThreadSanitizer の報告なし。警告 0 |
| 9 ＋ Release | 50bd934 | tool のテストのターゲットだけビルドし `ctest -R ^tool`（124 件、Disabled 2）、`tool_indep_s7_ui_threads_test` を -j1 で 20 回 | 119 通過、失敗 5 = `Defect_*` 5 件だけ（Pick の欠陥も ASan なしで失敗する）。繰り返し 20 回は通過。警告 0 |
| 9 ＋ clang | 50bd934 | tool のテストのターゲットだけビルドし `ctest -R ^tool`（124 件） | 119 通過、失敗 5 = `Defect_*` 5 件だけ。警告 0 |
| 8: 核だけ（Tela なし） | 50bd934 | tool のテストのターゲットだけビルドし `ctest -R ^tool`（40 件、Disabled 1） | 全部通過。組 U の独立テストは Tela 入りの構成だけに登録されるので 0 件（意図どおり） |
| MinGW の構文検査 | 2e0a455 以降 | `x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror -Wall -Wextra -fno-exceptions -fno-rtti`、Tela のインクルードパス、新しいテスト 7 本（fixture を含む） | 全部通過（最初は `%zu` で `-Wformat` の誤り、2e0a455 で直した。05a7e87 で足した Pick のテストも同じ書き方で検査し直した） |

## 未検証の箇所

- Windows / macOS のホスト（キー、Esc、WM_CHAR）。仕様の穴 9 はコードを読んだだけ。
- ライブで上書きが消えたあとのゲームとツールのずれ（仕様の穴 3）。
- 文字の描画（Pictor）。ヘッドレスは文字の帯だけなので、狭い欄（欠陥 4）で文字が切れる様子は画素では見ていない（幅だけ）。
- 画素のテストは箱・円・点の位置だけ。色の区分（選択、ループの区間、フェード、重なり）は見ていない。

回していない構成: 1〜7（ツールなし。組 U の独立テストは `tool/test` の Tela 入りの部分だけなので入らない）、9 の全体（全モジュール）は gcc Debug だけ
（ほかの 9 の派生は tool のターゲットだけをビルドした）。

## 再確認（決定 D-186〜D-189 の後）

実装担当の差し戻しの修正（353bee2、00986ce、e4fa328、fab42a9 ほか、先頭 a364fdb、組 P の c/live の取り込み済み）の上で行った。
組 U の前の `Defect_*` 5 件はすべて通過（直っている）。

### 決定に合わせて更新したテスト（コメントに D-番号）

| テスト | 決定 | 新しい期待 |
| --- | --- | --- |
| `IndepS7Ui.ChangingTheScreenEndsTheTextEntry` | D-186 | 入力中の F4・F2・Ctrl+PageDown・Ctrl+Tab は入力欄が受ける（画面も入力も変わらない、commit しない）。Esc で閉じてからは切り替わる。`OnScreen` とタブのボタンでは入力が終わる（commit しない） |
| `IndepS7UiScreens.MaterialRowsLastMaterialAndSymmetricPairs` | D-189 | 材質の行は `ma.mat.<NameKey>`、組は `ma.pair.<NameKey a>.<NameKey b>`。並べ替え・前への挿入・前の削除で同じ材質の ID が変わらない、重複なし |
| `IndepS7UiScreens.ProblemsRowsAreTheBuildsIssues` | D-189 訂正 | 行の数 = `DistinctIssues(CheckDocument)` の件数、`CheckDocument` は重複を含むまま（件数が多い）、フッタの「n err」も同じ数 |
| `IndepS7UiDefects.SpecGap_ClipSelectionIsAnIndexAcrossUndo` | D-186 | Undo の後に選択が外れ（`clip == -1`）、Delete は何も消さない（クリップは 2 つのまま） |
| `IndepS7UiDefects.SpecGap_StructuralEditDropsLiveOverridesSilently` | D-188 | バスを足しても上書きは対象で引き継がれる（件数が同じ、質問なし、値 -10 dB が残る） |
| `IndepS7UiDefects.SpecGap_UndoKeyDuringATextEntryUndoesTheDocument` | D-186 | 入力中の Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z で文書の取り消し・やり直しの数も値も変わらない |
| `IndepS7Ui.AppEscapeCancelsTheEntryWithoutCommit`（末尾） | D-186 | 入力中の Ctrl+Z の後、入力は開いたまま、文書の手は 1 つのまま（前は「どちらか」の緩い検査だった） |

名前（`SpecGap_*`）は CTest の登録と合わせるため変えていない。中身は決定を期待値にした。

### 再確認の結果（性質 L、`tool/test/indep_s7_ui_recheck_test.cpp`、1 件ずつ `tool_indep_s7_ui_recheck_<名前>`）

| 性質 | テスト | 結果 |
| --- | --- | --- |
| L1 入力中の全部のキー（4 つの修飾 × 26 キー） | `EveryKeyWhileTypingIsTheEntrys` | 通過（文書・選択・画面が変わらず、入力は開いたまま、`Key` は true） |
| L2 Esc（入力、一覧、何も無い全 12 画面、パッチの接続待ち） | `EscClosesTheEntryAndTheListAndIsAlwaysTheTools` | 通過（何も無い時も `Key` が true = ウィンドウに渡さない） |
| L2 ドラッグ中の Esc | `EscDuringAClipDragLeavesTheHistoryAsBefore`、`EscDuringNumberAndFaderDrags` | 値は戻り、取り消しの手は増えない（通過）。ただし欠陥 N1・N2 |
| L3 全部の数値の欄（82 個 × 9 種類の読めない入力） | `TypedNumbersOnEveryFieldGoToTheModelUnchanged` | 通過（どれも拒否、文書は変わらず入力は開いたまま） |
| L3 整数の欄・3D の欄 | `Defect_TypedNumberIsCutToAnIntegerBeforeTheModel`、`Defect_HugeTypedPositionsOnThe3DScreenShowInfAndNan` | **欠陥 N3・N6** |
| L4 ホイールと ± の連打 | `WheelAndButtonSeriesCountEveryStepAndStopAtTheEnd` | 通過（1・3・7 ノッチ、4 回の +、0..8 の端で止まり、端のホイールで手が増えない） |
| L5 縦のドラッグ（押す位置 3 × 移動量 8） | `VerticalClipDragFollowsTheMotionOnly` | 通過 |
| L6 欄の幅（窓 4 × DPI 4 × 14 の選択、1048 欄） | `NumberFieldsHaveRoomEverywhere` | 通過（幅 ≥ 48、重なりなし、1280×800 以上は窓の中）。小さい窓は **欠陥 N4** |
| L7 名前から作る ID | `NameMadeIdsAreUniqueWithAwkwardNames` | 通過 |
| L8 結び直しの質問 | `RebindCarriesWhatItCanAndAsksForTheRest`、`RebindQuestionGoesWhenTheUnplacedOverrideIsReverted`、`RebindWriteBackWritesWhatHasAPlace` | 通過（引き継ぎ、保留と質問、質問中の編集、Undo で消える、捨てる、戻す、書き戻す）。エフェクトの種類が変わった場合は **欠陥 N5** |
| L9 コーン | `ConeIsShownAlikeOnBothScreens` | 通過 |
| L10 Link | `LinkNotConnectedShowsNoPeerVersion`、`LinkFailedBanksAreListed` | 通過（行の数 = `failedBanks()`、理由、「failed」、Failed の間も全画面で重複なし） |
| L11 Problems | `ProblemsOnceOnScreenAndFooterCheckDocumentAsTheBuild` | 通過 |
| 前の欠陥 1〜5 | `tool_indep_s7_ui_Defect_*` | 5 件とも通過（直っている） |

### 新しい欠陥（失敗のまま）

- **N1 `Defect_EscOnADragThatChangedNothingUndoesTheEditBefore`（高）**: 何も変えなかったドラッグ（テストは左端で止まったクリップ。範囲の端の数値の欄、
  行って戻ったドラッグも同じコードの経路だがテストしていない）の途中で Esc を押すと、**その前の無関係な手が取り消される**。`kit.cpp` の `DocumentDrag` と数値の欄の
  `cancel` が「`EndGroup` してから `Undo`」で取り消すが、変化の無いグループは手を残さないので、`Undo` が 1 つ前の手を消す。
  再現: Event で Priority を 9 にする（1 手）→ 開始 0 のクリップを押して左へ 60 px（開始は 0 のまま）→ Esc → Priority が 128 に戻る。
  D-186「進行中の操作があればそれを取り消す」に反し、利用者の編集が黙って消える。
- **N2 `Defect_EscOnADragLeavesItAsARedoStep`（中）**: Esc で取り消したドラッグがやり直しの手として残り（Ctrl+Y でドラッグが戻る）、
  ドラッグの前にあったやり直しの手は消える。同じ原因（取り消しを `Undo` で行う）。直し方の例: 文書に「開いたグループを捨てる」
  操作が無いので、ドラッグの前の値を覚えて戻してからグループを閉じる（手は残らない）、またはモデルに `CancelGroup` を足す。
- **N3 `Defect_TypedNumberIsCutToAnIntegerBeforeTheModel`（中）**: 整数の欄（Channels、LOD prio.、Bars、Entry、Loops、ほか
  `static_cast<std::uint32_t>(v)` の 18 か所: buses_view・event_view・music_view・link_view・profiler_view）は、打った値をモデルに渡す前に
  整数へ型変換する。2.5 は 2 として書かれ、-1・1e300・2^32 は整数の範囲の外（未定義動作。ここでは 0 などになってモデルが受け付ける
  か、今の値と同じとして入力が閉じる）。Link の Port に 70000 を打つと 4464 になる。D-187「切り詰めずにそのままモデルに渡す」に反する。
  期待: 整数でない・範囲の外の値は拒否（入力は開いたまま）。
- **N4 `Defect_NumberFieldsOutsideTheWindowAtSmallWindows`（低〜中）**: 窓 320×240（App が受け付ける最小の論理の大きさ）で 49 個、
  640×400 で 9 個の数値の欄が窓の外に宣言される（Event の項目、エフェクト、スナップショット、Params、Segment、Playlist、遷移の試聴、
  Material）。見えず押せない。D-187「入りきらない行は折り返す・段を分ける・ページを送る」。1280×800 以上では窓の中。
- **N5 `Defect_RebindWriteBackPutsAnOverrideIntoAnotherEffect`（中）**: Sfx の lowpass のパラメータにライブの上書き（500 Hz）がある
  ときに、文書でそのエフェクトを消して別の種類（chorus）を同じ位置に足すと、質問が出る（正しい）。そこで「Write back」を押すと、
  lowpass の値が chorus のパラメータ 0 に書かれる（0.5 → 1、chorus の範囲に切り詰められて）。`ResolveRebind(true)` が古い
  プロジェクトに結ばれたままのミキサーで `WriteBackMixer` を呼び、エフェクトを位置で書くため。期待: 引き継げない値（質問に出た値）は
  書き戻さない、または種類が合うときだけ書く。エフェクトを消しただけの場合は、新しいエフェクトは作られない（確認、通過）。
- **N6 `Defect_HugeTypedPositionsOnThe3DScreenShowInfAndNan`（低）**: 3D 画面の位置の欄は有限の数なら何でも受け付け、
  `SpatialPreview` は float で計算するので、1e30・1e300 で「distance inf m」「azimuth -nan deg」「cone nan deg」が出る。Tela の
  上限は超えない（図は落ちない）。期待: float に入らない値は拒否するか、表示が有限。

### 残る仕様の穴

- ホイールの手のまとまり: 同じ欄のホイールは、ほかのコントロールへ移るまで（時間が空いても）1 手にまとまる（ルータのキーが変わらない）。
  段階2 のフェーダーと同じ。「続けて回した分」の「続けて」を時間で切るかは決まっていない。推奨: 今のまま（フェーダーと揃える）。
- 位置で持つ選択を外すのは Undo / Redo と画面の切り替えのとき（`ClearPositionSelections`）。ほかの経路での文書の変更（Write back、
  ライブ、ほかのホスト）では外さない。今の画面の操作では起きないが、決定の「画面の外で文書が変わったら」とは範囲が違う。
- パッチのノードが重なると、下のノードの端子は押せない（生成した配置で osc1 の出力の端子が osc27 の本体の下）。重なりを避ける
  決まりは無い。

### 再確認で回した構成と結果

ビルドは `$S/ui-indep/b-*`、-j2、1 つずつ（終わったら消した）。9 の派生と 8 は tool のターゲットだけをビルドし `ctest -R ^tool`。
組 U の独立テストは CTest で 69 件（前の 46 件 ＋ 再確認の 23 件（実行ファイルは DISABLED で、22 件を 1 件ずつ））。

| 構成 | コミット | 結果 |
| --- | --- | --- |
| 9: Tela 入り、gcc Debug、全部 | 50f6d0f | 415 件のうち失敗 10 = 組 U の新しい `Defect_*` 6 件（N1〜N6）＋ 組 P の `tool_indep_s7_live_Defect_*` 4 件（対象外）。前の組 U の `Defect_*` 5 件と更新したテストは通過。警告 0 |
| 9 ＋ ASan / UBSan | 93902f3 | 188 件のうち失敗 10 = 同じ 10 件だけ。ASan・UBSan の報告なし（一覧・入力欄・質問の帯を閉じるコールバックも含む。N3 の浮動小数から整数への範囲外の変換は gcc の `-fsanitize=undefined` に `float-cast-overflow` が入らないので報告されない） |
| 9 ＋ TSan | 55574da | `tool_indep_s7_ui_threads_test` と `tool_ui_editor_concurrency_test` を -j1 で 20 回、`tool_ui_concurrency`・`tool_indep_concurrency`・`tool_live_concurrency` を 1 回: 全部通過、報告なし |
| 9 ＋ Release | 55574da | 188 件のうち失敗 11 = 同じ 10 件 ＋ `tool_profiler_streams_test`（60 s で失敗。組 P のテストで組 U は触っていない。ほかの担当のビルドと同時に走っていた。再実行はしていない）。並行性の 2 本を 20 回: 通過 |
| 9 ＋ clang | 55574da | 188 件のうち失敗 10 = 同じ 10 件だけ。警告 0 |
| 8: 核だけ | 55574da | 69 件のうち失敗 5 = 組 P の `Defect_*` 4 件 ＋ `tool_profiler_streams_test`（60 s、Release と同じ）。組 U のテストは入らない（意図どおり） |
| MinGW の構文検査 | 50f6d0f | 再確認・common・screens・defects の 4 本: 通過 |

### 再確認で確かめていないこと

- N3 の未定義動作の値（-1、1e300 を整数に変えた結果）は処理系によって違う。テストは「文書が変わらず入力が開いたまま」だけを見る。
- Esc はホストを通さず `App::Key` で確かめた（Windows のホストは動かしていない）。
- `tool_profiler_streams_test` の 60 s の失敗が負荷によるものかは、構成を消した後なので確かめていない。

## 再確認 2 回目（2 回目の差し戻しの後）

実装担当の修正（0629b4a `AbortGroup` と `SpatialPreview` の範囲、2d8b692 N1〜N6 と選択の解除、テスト 1e7771d、先頭 f3c5d6d）の上。
前回の新しい欠陥 N1〜N6 はすべて通過（直っている）。テストは `tool/test/indep_s7_ui_recheck2_test.cpp`（性質 M、1 件ずつ
`tool_indep_s7_ui_recheck2_<名前>`）。

### 決定に合わせて更新したテスト

- `IndepS7UiDefects.Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused`（後半）: 選んだクリップをホストが `MoveClip` で動かしてから
  開始位置を打っていた。決定「位置で持つ選択は、自分の編集以外で文書が変わったら外す（ホストからの変更を含む）」では選択が外れるので、
  先に動かしてから選ぶ順にした（期待は変えていない）。実装担当が「この Defect を通すために値だけの変更では外さない」としていた
  理由はこれで無くなる。

### 結果

| 性質 | テスト | 結果 |
| --- | --- | --- |
| M1 `AbortGroup` | `AbortGroupRestoresEverything`、`RandomSequencesWithAbortsKeepTheHistory`（40 種 × 60 手） | 通過。入れ子（外のグループの中で内のグループを Abort）: 内の Abort の後に外のグループは閉じていて、外のグループの中の手も一緒に消える（記録。決定は入れ子を決めていない。画面は入れ子にしない） |
| M2 画面の Esc（7 種 × 前に手／やり直しの手）、フェーダーとノブ | `EscOnEveryDragLeavesTheHistoryAsBefore`、`EscOnFaderAndKnobDragsLeavesTheLiveHistoryAsBefore` | 通過 |
| M3 整数の欄 25 個 × 8 種類の値、Port | `IntegerFieldsRefuseBeforeConverting` | 通過 |
| M4 小さい窓 | `Defect_BodyButtonsOutsideTheWindowAtSmallSizes`、`Defect_HeaderAndFooterButtonsOutsideSmallWindows` | **欠陥 P1・P2** |
| M5 書き戻し | `WriteBackIsMatchedByObject` | 通過（置き換え: 新しい種類に書かず「discarded 1 live overrides without a place: Sfx snd.fx.lowpass CutoffHz」。削除: 作らない。並べ替え: 質問が出て、lowpass の値は捨てられ highpass に書かれない）。同じ種類の別のエフェクトは仕様の穴 |
| M6 非有限の表示 | `Defect_NonFiniteTextAfterExtremeValues` | **欠陥 P3** |
| M7 選択を外す範囲 | `OwnEditsKeepThePositionSelection`、`HostStructuralEditDropsThePositionSelection`、`WriteBackDropsThePositionSelection`（通過）、`Defect_HostValueEditKeepsThePositionSelection` | **欠陥 P4** |

### 新しい欠陥（失敗のまま）

- **P1 `Defect_BodyButtonsOutsideTheWindowAtSmallSizes`（中）**: 数値の欄のある画面の最小として決めた 800×500（DPI 1）で、本体の
  ボタンが窓の外に出る: Material の組の表 16 セルと `ma.def0`、Segment の `mu.seg.grid.apply` と `mu.seg.grid.offset.inc`、Event の
  `ev.clip.addsynth`。押せない。また 320×240 で Profiler のタブ（Streams、LOD、Event log: `prof.tab.3〜5`）が外に出て、Event log の
  「too small」を見る手段も Ctrl+PageDown などのキーだけ。
- **P2 `Defect_HeaderAndFooterButtonsOutsideSmallWindows`（低〜中）**: 320×240・640×400・799×499 で、ヘッダのタブ（Patch 以降）と
  フッタの Undo / Redo / Save・問題の数が窓の外。Mixer・Profiler・Replay・Problems は 320×240 まで描くとしたが、ほかの画面へも
  文書のボタンへもポインタでは行けない（キーだけ）。
- **P3 `Defect_NonFiniteTextAfterExtremeValues`（低）**: Params でパラメータの max / default に ±1e308（有限の double、モデルが受け付ける）
  を打つと、ミキサーのパラメータのノブの名前が「Speed inf」「Speed -inf」になる（float への変換）。決定「表示する値は必ず有限」。
  検査から外したもの: 「(0=inf)」（凡例）、フェーダーの「-inf dB」（段階2 の約束: -144 dB より下は無音）。
- **P4 `Defect_HostValueEditKeepsThePositionSelection`（中）**: ホストが選んだクリップを `MoveClip` で動かす、選んだカーブの点を
  `MovePoint` で動かすと、選択が残る（`App::PositionShape` が配列の形が変わったときだけ外す）。決定は「ホストからの変更を含む」。

- **P5 `Defect_HugeTransitionRequestTimeMakesNaNMusicalPositions`（低、`-fsanitize=float-cast-overflow` で発見）**: Music の遷移の試聴の
  要求時刻（`mu.pv.req`、画面の値で文書ではない）は有限なら何でも受け付け、1e308 や 1e300 で試聴の位置が非有限・巨大になり、
  `TempoMap::Musical` が NaN / 5e299 を int に変換する（`music_editor.cpp:107`、未定義動作）。決定「表示する値は必ず有限」「範囲を
  決めて拒否」（3D の位置と同じ扱い）。-1e308 は 0.5 s に収まる。

`-fsanitize=float-cast-overflow` を足した構成では、組 U の外（モデル、組 M）でも報告があった（組 U の欠陥にはしていない）:
`tool/src/project.cpp:294`（`channels` に -5 の書かれたファイルで `static_cast<std::uint32_t>`、`IndepToolProject.FuzzedProjectsNeverCrashTheModelsOrTheUi`）、
`tool/src/music_editor.cpp:254`（`tempo[].tick` に -5、`IndepS7Robust.H1_WrongTypesEverywhere`）。ファイルから読んだ値の範囲外の変換。

### 残る仕様の穴

- **同じ種類の別のエフェクト**: lowpass が 2 つあり 1 つ目に上書き、文書で 1 つ目を消すと、残った lowpass（元の 2 つ目）が同じ位置・
  同じ種類なので、質問なしで上書きの値（500）がそれに乗る（「wrote 1 live values」）。エフェクトに GUID が無いので、決定の
  「GUID・種類・パラメータ」の規則では区別できない。選択肢: 今のまま／エフェクトの数か並びが変わったら質問する。推奨: 質問する。
- **入れ子の Abort**: `AbortGroup` は入れ子のどの段から呼んでも外まで閉じて全部を捨てる（M1 の記録）。画面は入れ子にしないので
  今は問題にならない。決めておくなら「呼んだ段だけを捨てる」か「外まで捨てる」か。
- **最小の窓の数**: 800×500 は DPI 1 でボタンがはみ出し（P1）、DPI 2 では出ない（同じ論理サイズで違う）。最小の論理サイズを
  DPI によらず決めるかどうか。

P5 の見え方（Release、ASan なし）: 1e308 で「fires at - s (bar -2147483647 beat -2147483647)」、1e300 で 300 桁の秒数の行。

### 再確認 2 回目で回した構成と結果

ビルドは `$S/ui-indep/b-*`、-j2、1 つずつ（終わったら消した）。tool のターゲットだけをビルドし `ctest -R ^tool`。

| 構成 | コミット | 結果 |
| --- | --- | --- |
| 9: Tela 入り、gcc Debug | 53e94f3 | 203 件のうち失敗は組 U の新しい `Defect_*` 4 件（P1〜P4）だけ（`tool_ui_editor_fix2_test` は最初の回でビルドの一覧に無く Not Run、ビルドして通過）。組 P の `Defect_*` も今回は通過。前の `Defect_*` 11 件（1〜5、N1〜N6）は通過 |
| 9 ＋ ASan / UBSan ＋ `float-cast-overflow` | 53e94f3（P5 は 5f901c9 で足して実行） | ASan の報告なし。`float-cast-overflow` の報告 3 件: 組 U の P5（`music_editor.cpp:107`、画面の値から）と、組 M のモデルの 2 件（`project.cpp:294`、`music_editor.cpp:254`、ファイルの値から。`tool_indep_s7_robust_test`・`tool_indep_model_test` が失敗）。組 U の整数の欄からの報告は無い。ほかの失敗は P1〜P4 |
| 9 ＋ TSan | 5f901c9 | `tool_indep_s7_ui_threads_test`・`tool_ui_editor_concurrency_test`・`tool_ui_concurrency_test`・`tool_indep_concurrency_test` を -j1 で 20 回: 通過、報告なし |
| 9 ＋ Release | 5f901c9 | 204 件のうち失敗 5 = P1〜P5 だけ。並行性の 2 本を 20 回: 通過 |
| MinGW の構文検査 | 5f901c9 | `indep_s7_ui_recheck2_test.cpp`、`indep_s7_ui_defects_test.cpp`: 通過 |

回していない構成: clang、核だけ（今回の依頼の範囲外）、9 の全体（tool 以外）。

### 再確認 2 回目で確かめていないこと

- 入れ子の `AbortGroup` の決まり（記録だけ）。
- Windows のホストの Esc（`App::Key` で確かめた）。
- 小さい窓でのキー操作（Ctrl+PageDown）で全部の画面に届くことは前回確かめたが、ポインタで届かないこと（P2）は宣言の位置で見た。

## 再確認 3 回目（3 回目の差し戻しの後）

実装担当の修正（7600de5 最小の窓 1024×640、fb79bdb 選択はリビジョンで外す、b9498d2 バンクに入らない数値と変換の範囲と遷移の試聴の時刻、
a90c020 エフェクトの列が変わったら質問、先頭 68f3eaa）の上。前回の P1〜P5 はすべて通過。テストは `tool/test/indep_s7_ui_recheck3_test.cpp`
（性質 N、1 件ずつ `tool_indep_s7_ui_recheck3_<名前>`）。

### 決定に合わせて更新したテスト

なし（前回までのテストに決定と食い違うものは無かった。最小の窓は `ui::kMinEditorWidth` / `kMinEditorHeight` を読んでいる）。

### 結果

| 性質 | テスト | 結果 |
| --- | --- | --- |
| N1 最小の窓（ちょうど・1 物理画素小さい・縦長・横長 × DPI 0.25〜4 × 27 の状態: 全画面・全タブ・一覧・結び直しの質問） | `SmallestWindowEverywhere` | 通過（以上で外のボタン 0、未満は `body.small` だけ、重複なし） |
| N1 大きさをまたぐ | `ResizingAcrossTheMinimumKeepsTheState` | 通過（ドラッグ中に縮めて離すとグループが閉じ手は高々 1、小さい間の Delete は何も消さない、入力は Esc で閉じて何も書かない） |
| N2 選択 | `SelectionsAcrossEditsOfEveryKind` | 通過（自分のドラッグ・打った値で残る。ホストの値の変更・Undo・フッタの Redo・書き戻しで外れ、直後の Delete は何も消さない） |
| N3 バンクに入らない数値 | `NumbersThatDoNotFitTheBankAreRefused` | 通過（FLT_MAX ちょうどを受け付け、次の double・±3.5e38・1e300 を拒否、ファイルの 1e300 はほかの編集で残る、`Modify` で持ち込む 1e300 は拒否） |
| N4 壊れたファイル（7 種類の値 × 34 の項目） | `BrokenNumbersInFilesAreShownWithoutUndefinedConversions` | 通過（float-cast-overflow の構成は下の表） |
| N5 エフェクトの列の質問（6 通り）と列が変わらない時 | `EffectChainChangesAsk` | 通過 |
| N6 名前が inf / nan | `Defect_NamesThatReadInfOrNanAreShownAsADash` | **欠陥 Q1** |

### 新しい欠陥（失敗のまま）

- **Q1 `Defect_NamesThatReadInfOrNanAreShownAsADash`（低）**: `Kit::Label` が文字を全部 `FiniteText` に通すので、名前が「inf」「nan」の
  オブジェクトが「-」と出る。例: Buses のグラフの順「graph order: Sfx - Master」（バス「nan」）、Params の合成の行
  「curve 0: - = 1.000 (default) -> pitch …」（パラメータ「inf」）。数値でない名前まで置き換えている（実装担当が未検証と書いた点）。
  直し方の例: 数値を文字にするところ（`ShownNumber` など）でだけ有限にし、ラベル全体は置き換えない。

ほかに新しい欠陥は無い。

### 残る仕様の穴

- 最小の窓は「物理画素」で決まるので、DPI 0.25 では論理の 1 画素ではなく 4 画素単位で大きさが変わる（1023 論理は 1024 に丸まる）。
  問題ではないが、README の「1024 × 640（論理）」はそのように読める。
