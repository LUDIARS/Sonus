# 組 U（Tela の画面）の独立テスト: 満たすべき性質

実装（`tool/ui/src`、`tool/app`）と実装担当のテスト（`ui_*`、`ui_editor_helpers.hpp`）を読む前に、次だけから書き出した:
設計書 11.1〜11.3、`tool/README.md`（「画面と操作の仕様」「Tela の使い方」「段階5 の画面（組 U）」）、CONTRACTS.md の D-67・
D-67a・D-100・D-101、`tool/ui/include/snd/tool/ui/*.hpp`、モデルの公開ヘッダ（`tool/include/snd/tool/*.hpp`）、
依頼書 `U-impl.md`、実装担当のメモ `C-notes/ui.md` の「作ったもの」と「決定の案」、Tela の公開ヘッダ。

確かめ方は解析的（要素の木の数・ID・ラベル、モデルの値、画素の位置）。保存した画像との比較は使わない。
要素の ID は実装を読まずに、実行時に要素の木から（ラベルと位置で）探す。

テストファイル（`tool/test/`）: `indep_s7_ui_common_test.cpp`（共通・文字の入力・キー）、`indep_s7_ui_screens_test.cpp`
（画面ごとのモデルとの対応）、`indep_s7_ui_gesture_test.cpp`（ドラッグ）、`indep_s7_ui_limits_test.cpp`（上限・DPI・画素）、
`indep_s7_ui_threads_test.cpp`（並行性）、`indep_s7_ui_defects_test.cpp`（欠陥。1 件ずつ CTest に登録）、ヘッドレスの CTest。

## A. 画面の一覧と切り替え（views.hpp、README のショートカット、決定の案 a）

- A1 `ScreenName` / `ParseScreen` は 12 面すべてで往復し、名前は README の 12 個（mixer buses event params spatial music
  material patch profiler replay link problems）。知らない名前・空・大文字違いは拒否し、出力を変えない。
- A2 `kTabOrder` は 12 面の置換（重複なし、全部ある）。
- A3 キー: F2 ミキサー、F3 プロファイラ、F4 Buses、F5 Event、F6 Params、F7 3D、F8 Music、F9 Material、F10 Patch。
  Ctrl+PageDown（Ctrl+Tab）は `kTabOrder` の次、Ctrl+PageUp（Ctrl+Shift+Tab）は前、両端で回り込む。12 回で元に戻る。
- A4 画面を切り替えると文字の入力（`TextEntry`）は終わる（text_entry.hpp「changing the screen ... ends it」）。

## B. 文書の履歴・保存・未保存（app.hpp、README、決定の案 b）

- B1 ミキサー以外の画面で Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z は文書の Undo / Redo（`UndoCount` / `RedoCount` が 1 動く）。
- B2 ミキサー画面の Ctrl+Z はライブの値の履歴（`MixerModel`）だけを戻し、文書は変えない。
- B3 Ctrl+S は文書を保存する（保存後 `IsDirty()` が偽、`App::dirty()` もフレームの後で偽）。編集の後のフレームで `dirty()` が真。
- B4 文書が無いとき、Ctrl+Z 等・Ctrl+S・エディタの画面は落ちない（「--project で起動する」の表示）。

## C. 文字の入力の回避（text_entry.hpp、kit.hpp、決定の案 c）

- C1 `AppendUtf8` は 1〜4 バイトの UTF-8 を正しく書く。サロゲート（U+D800〜DFFF）と U+10FFFF 超は無視する。
  `PopUtf8` は最後の 1 文字（多バイトでも）を消す。空で呼んでも落ちない。
- C2 `ParseNumber`: "12"、"-3.5"、"1e3"、"+2" を読む。""、"-"、"."、"1.2.3"、"abc"、"inf"、"nan"、"0x10"、"1e999"（有限でない）、
  "1 2" を拒否する。
- C3 `FormatNumberText(v, d)` を `ParseNumber` で読み戻すと、`d` 桁に丸めた値（小数 `d` 桁以下の値なら同じ値）。
- C4 `TextEntry`: 文字は末尾に足され、Backspace で 1 文字消え、Enter で commit が呼ばれ成功なら終わる、拒否なら開いたまま
  （`refused` が真）、Esc は commit を呼ばずに終わる。`kMaxBytes` を超えて伸びない。数値の入力は数字・符号・'.'・'e' 以外を
  受け付けない。新しい `Begin` は前の入力を commit せずに置き換える。
- C5 名前の欄（画面から）: 既にある名前への変更はモデルが拒否し、文書は変わらず入力が開いたまま。正しい名前は 1 手で、
  Undo 1 回で元の名前に戻る。Esc で取り消すと文書は変わらない。
- C6 数値の欄（画面から）: 範囲の外の値（モデルの規則に反する値、例: 負の startMs、weight < 0）は文書に入らない。
  入力した値は 1 手。`[-]` `[+]` は 1 ステップ。

## D. 画面とモデルの対応（各画面、依頼書「画面はモデルを読むだけ」）

各画面で: モデルの中身（行・項目・値）が要素の木に出ていること（数と値のラベル）、モデルを外から変えると次のフレームの宣言が
変わること、画面の操作はモデルの関数を通る（Undo 1 回で戻る、文書が未保存になる）、モデルが拒否する操作は画面からも通らない。

- D1 Event: 一覧に全 Event（ページ送りで全部に届く）。選んだ Event のトラックの数のレーン、クリップごとに当たり判定の要素。
  クリップの箱の x と幅は `ComputeTimeline` の startMs / endMs を `TimeAxis` で写した値（`LaneClipRect`）。トラックの追加・削除が
  `EventEditor::TrackCount` を 1 変え、Undo 1 回で戻る。Delete キーは選んだクリップを消す（1 手）。
- D2 Buses: 全バスが出る。バスの追加は 1 手（Undo で消える）、マスターの削除・子のあるバスの削除・循環する親の変更・自分への
  センドは画面から通らない（文書が変わらない）。新しいバスはミキサー（`Session::mixer().buses()`）にも出る（決定の案 g）。
- D3 Params: パラメータの項目が `ParameterEditor` の値と一致。カーブの点は `PlotAxes` の位置。点を消して 2 点未満にする操作は
  通らない。合成の確認の値は `ComposeEvent` と一致。
- D4 3D: 表示の値が `SpatialPreview::Compute` と一致、maxDistance の円の半径 = maxDistance / metersPerPixel。
- D5 Music: ルール表の行の数 = `Rules().size()`（ページ送りで全部）、到達できない行に印、到達できない行があるときの
  自動の並べ替えは通らない（ルールの順が変わらない）。
- D6 Material: 材質の数、最後の材質の削除は通らない、組は対称（(a,b) を設定すると (b,a) の表示も同じ）。
- D7 Patch: ノードの数、`kMaxNodes` を超える追加は通らない、循環する接続は通らない。
- D8 Profiler / Replay / Link: 接続なし・文書なし・記録なしで落ちず、理由を表示する。
- D9 Problems: 行の数 = `CheckDocument` の問題の数（＋ライブの問題）、エラーが先。Go でそのオブジェクトの画面へ（`Navigate`）。
- D10 画面の側だけが持つ状態（選択、ページ）が、Undo・オブジェクトの削除・外からの編集の後に文書とずれても落ちず、
  存在しない物を指さない（消えたクリップ・点・ノードを Delete で消そうとして別の物を消さない）。

## E. ID の安定と一意（決定の案 k、README「名前の重複・ID」）

- E1 全画面: 同じ状態で 2 フレーム宣言すると ID の集合が同じ。要素の ID に重複が無い。
- E2 値の変更（音量、startMs、点の y）の前後で、その対象の ID が同じ。
- E3 一覧のオブジェクトを足す・消す・名前を変える（並びが変わる）と、残ったオブジェクトの ID は変わらない。
- E4 画面を往復しても（A → B → A）ID の集合が同じ。ページを送って戻しても同じ。
- E5 違う対象が同じ ID にならない（E1 の重複なし を大きなプロジェクトと全画面で）。

## F. ジェスチャー（pointer_router.hpp、kit.hpp、決定の案 d、e）

- F1 クリップのドラッグ: 横に dx 論理画素 → startMs = 押した時の startMs + dx × msPerPx（押した点からの絶対量）。往復しても
  元に戻る。左端を越えても startMs は負にならない（モデルの規則）。1 回のドラッグが Undo 1 回。
- F2 縦のドラッグは 48 px ごとに 1 つ下のトラック（決定の案 e）、最後のトラックの先・最初のトラックの前へは行かない。
  離した時の移動も同じ 1 手。
- F3 カーブの点のドラッグは隣の点を越えない（モデルの規則）、1 手。
- F4 3D のエミッタのドラッグ: 離した位置 = `ScreenToWorld(押した位置 + 移動量)`。
- F5 パッチのノードのドラッグ: 位置が移動量だけ変わる（1 手）。
- F6 数値の欄の縦ドラッグ: 4 px で 1 ステップ（`NumberDrag`）、Shift で 1/10、1 回が 1 手、範囲で止まる。
- F7 ドラッグ中に宣言が差し替わっても（毎フレーム）ドラッグは続き、値は連続。
- F8 ドラッグ中に画面を切り替える（F キー）・Tela がジェスチャーを取り消す（cancel）・要素が消える: 文書のグループが閉じる
  （`InGroup()` が偽）、手は高々 1 つ、その後の編集は別の手になる。
- F9 動かさずに押して離す: 文書は変わらない（手が増えない）。

## G. Tela の上限と例外（D-67、D-67a、safe_drawing.hpp、layout_metrics.hpp）

- G1 全画面 × {文書なし、空のプロジェクト（project.json だけ）、基本、大きなプロジェクト（Event 1000、長い名前 200 文字、
  ルール 150 行、ノード上限のパッチ、材質 64、バス 100、極端な値（1e300、-0、巨大な startMs））} × DPI {0.25, 1, 2, 4} ×
  大きさ {小さい、普通、大きい}: 子プロセスで回して落ちない（Tela の例外は -fno-exceptions で終了になる）、要素の数 ≤ 4096、
  ID の重複なし、canvas の図形が Tela の上限（図形 256・点 4096）の中、要素の位置と大きさが有限。
- G2 DPI の範囲外（0、負、NaN、8）を `SetViewport` に渡しても 0.25〜4 に切り詰めて落ちない。
- G3 ASan / UBSan で G1 が通る。

## H. 画像（raster_compositor.hpp、editor_draw.hpp）

- H1 Event のクリップの箱: `Compose` した画像で、クリップの要素の位置の箱が背景と違う色、箱の外（レーンの空いたところ）と
  区別できる。DPI 1 と 2 で同じ論理位置（2 では物理画素が 2 倍）。
- H2 3D の maxDistance の円: エミッタから半径 r = maxDistance / metersPerPixel の位置に線の画素がある。
- H3 カーブの点: `PlotAxes` の位置に点の画素がある。

## I. ヘッドレス（sonus-tool）

- I1 `--headless --screen <名前>` が 12 面すべてで、空のプロジェクトと大きなプロジェクトで終了コード 0、画像を書く。
- I2 知らない画面名は 0 以外で終わる。

## J. 並行性

- J1 UI スレッドが画面の編集・フレームを回す中で、ライブリンクのクライアントのスレッドとゲームのスレッドが動く（TSan で
  報告なし、Release で 20 回）。UI の操作はすべて UI スレッドから。

## K. 実装を読んだ後に足したもの（`indep_s7_ui_defects_test.cpp`）

- K1 入力した数値は欄の範囲で切り詰めずにモデルへ渡し、モデルが拒否すれば入力は開いたまま（kit.cpp の commitText が
  `ClampNumber` してから setter を呼ぶのを狙う）。→ `Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused`
- K2 数値の欄のホイールは、宣言と宣言の間の複数のノッチを全部数える（宣言の時の値に足しているのを狙う）。
  → `Defect_NumberFieldWheelNotchesBetweenFramesAreLost`
- K3 クリップの縦のドラッグのトラックは移動量で決まり、押した点の位置で決まらない（離した点の局所座標の floor を狙う）。
  → `Defect_ClipVerticalDragTrackComesFromThePressPointNotTheMotion`
- K4 数値の欄の値の要素に幅がある（欄 − 44 px の幅が小さく・負になるのを狙う）。→ `Defect_NumberFieldValueHasNoRoom`
- K5 上書きがある間に値だけ変えた文書へ書き戻しても、上書きの無いバスの新しい値は消えない（決定の案 g の stale なミキサー）。
  → `WriteBackAfterStaleMixerKeepsNewerDocumentValues`（通過）
- K6〜K8 仕様の穴の記録（今の振る舞いを出力する）: 構造の変更で上書きが消える、選択が番号で Undo の後に別の物を指す、
  入力中の Ctrl+Z が文書に効く。→ `SpecGap_*`

## L. 再確認（決定 D-186〜D-189 の後、`indep_s7_ui_recheck_test.cpp`）

- L1 (D-186) 入力中は全部のキー（Ctrl / Shift の組み合わせ、Z・Y・S、Delete、Tab、矢印、Home / End、PageUp / Down、F1〜F12）を
  入力欄が受ける: 文書・選択・画面が変わらず、入力は開いたまま。
- L2 (D-186) Esc は常にツールが受ける（`App::Key` が true）。入力・一覧を閉じる（commit / pick しない）、何も無い時は何もしない、
  パッチの接続待ちを消す、ドラッグ（クリップ、数値の欄、フェーダー）を取り消す。取り消しの後、文書の取り消しの履歴は押す前と
  同じ（手が増えない、やり直しの手も増えない、前の手を消さない）。
- L3 (D-187) 全画面の全部の数値の欄（82 個）で、数として読めない入力（空、"-"、"."、文字、"1e"、1e999）は拒否され文書は変わらず
  入力が開いたまま。整数の欄は打った値を整数に切らずにモデルへ（2.5、-1、1e300、2^32 は拒否）。3D の欄の巨大な値で非有限の表示が
  出ない。
- L4 (D-187) 宣言の間の n ノッチ・n 回の ± は n ステップ、端で止まる（端ではそれ以上の手が増えない）。
- L5 (D-187) 縦のドラッグのトラック = clamp(t + round(dy / 48))、押した位置（上・中・下）によらない。
- L6 (D-187) 数値の欄の値の幅 ≥ `kMinNumberValue`、自分のボタン・ラベルと重ならない（窓 320×240〜1920×1080 × DPI 0.25〜4）。
  窓の中に収まる（1280×800 以上。小さい窓は Defect）。
- L7 (D-189) 名前から作る ID: 記号・非 ASCII・長い名前・大文字小文字違い・別の種類の同じ名前で、全画面に重複が無い。
- L8 (D-188) 結び直し: 引き継げる値は引き継ぐ（質問なし）、引き継げない値で保留して質問を出し上書きを残す、質問中の値・構造の
  編集で上書きが消えない、Undo で質問が消える、捨てる・書き戻す・上書きを戻すで結び直す。書き戻しは別の種類のエフェクトに値を
  書かない、消えたエフェクトを作らない。
- L9 (D-189) コーンの表示が Event 画面・3D 画面・`SpatialPreview::Cone` で同じ（無い時は 0 / 0 / 0、3D は「none」）。
- L10 (D-189、組 P) Link: 未接続なら「not connected」で相手の版を出さない。ホットリロードの失敗は `failedBanks()` の件数の行と理由、
  状態の「failed」。Failed の間も全画面で ID の重複なし。
- L11 (D-189 訂正) Problems とフッタは `DistinctIssues`（同じ問題は 1 回、順序は保つ）、`CheckDocument` は buildtool のまま。

## M. 再確認 2 回目（2 回目の差し戻しの決定の後、`indep_s7_ui_recheck2_test.cpp`）

- M1 `AbortGroup`: 何も変えない・行って戻る・複数の物（作成と削除を含む）のグループを捨てると、状態・Undo・Redo の数と中身が
  BeginGroup の時のまま、revision も変わらない（何も変わらないとき）。入れ子の扱いを記録。乱数の操作列（40 種 × 60 手）に
  Abort を混ぜても、毎回の Abort の不変条件と「全部 Undo で初め、全部 Redo で終わり」が成り立つ。
- M2 画面の Esc: クリップ（変化なし、行って戻る、動かす）、数値の欄（端、動かす、行って戻る）、カーブの点 × {前に手がある、
  やり直しの手がある}: 文書・Undo / Redo の数が押す前のまま、前の手・やり直しの手がそのまま効く。フェーダーとノブ: 値と
  ライブの履歴が押す前のまま。
- M3 整数の欄（25 個）は 2.5、-1、2^32（32 ビットの欄）、2^64、±1e300、nan、0.0001 を拒否（文書が変わらず入力が開いたまま）。
  Port は 0、65536、70000 を拒否、1 と 65535 を受け付ける。
- M4 小さい窓（320×240、640×400、799×499、800×500、1024×640 × DPI 1・2）: 画面の本体は「too small」（数値の欄なし）か、
  ボタンが全部窓の中。ヘッダとフッタのボタンも窓の中。
- M5 書き戻し: 種類の置き換え・削除で新しいエフェクトに値を書かず捨てた物を名前で出す、並べ替えで別の種類に書かない。
  同じ種類の別のエフェクトは記録。
- M6 全画面の文字に inf / nan が出ない（全部の数値の欄に ±1e308・1e-308・±1e30 を打った後）。
- M7 位置の選択: 画面自身の編集では残り、ホストの構造の変更・書き戻しで外れる。ホストの値だけの変更でも外れる（決定）。
