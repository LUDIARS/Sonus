# セッション C 組 M（編集できるプロジェクトと全エディタのモデル、buildtool）メモ

実装担当のメモ。受け入れ側への引き継ぎ。ブランチ `c/model`（ローカルコミットのみ）。

## 進み具合

- [x] 0. 編集できるプロジェクト（`ProjectDocument`、`doc_build`）と buildtool の拡張（メモリ上の供給元・メモリへの出力）
- [x] 1. Event エディタ
- [x] 2. ミキサーの残り（と上書きの書き戻し）
- [x] 3. パラメータとカーブ
- [x] 4. 3D 減衰のプレビュー
- [x] 5. 音楽
- [x] 6. マテリアル表
- [x] 7. パッチ（プロジェクト形式、buildtool、仮の `XPCH`）
- [x] 9. 文書（tool/README.md、project-format.md、bank-format.md §9、依頼 C-10〜C-12）
- 構成ごとの結果は末尾。

## 作ったもの

すべて `Sonus::tool_core`（Tela なし）に置いた。`tool/ui`・`tool/app`・`livelink/`・`core/` などは変えていない。

| ファイル | 役割 |
| --- | --- |
| `buildtool/include/snd/buildtool/source.hpp` | `ProjectSource`（ファイルの供給元の差し替え）、`OutputFile`（メモリへの出力） |
| `buildtool/include/snd/buildtool/build.hpp` | `BuildOptions::source` / `memoryOutput` を末尾に追加（CLI と既存の使い方は不変） |
| `buildtool/src/project.cpp/.hpp` | `Load(ProjectSource&)`、`ReadProjectFile`（WAV・blob も供給元から）、種類 `patch` とスキーマ（`synth.patch`、`nodes`、`connections`、`output`） |
| `buildtool/src/build.cpp` | 供給元からの読み込み、メモリへの出力、patch の検査（`CheckPatches`）と `XPCH` への出力、ID 表の種類 `patch` |
| `buildtool/include/snd/buildtool/patch.hpp`, `src/patch.cpp` | パッチのノードの表（ツールと共有）、検査、依存順の配列への変換、仮の `XPCH` の書き出しと読み込み（変換を 1 か所に） |
| `tool/include/snd/tool/document.hpp`, `src/document.cpp` | 文書モデル: 全種類、保存の規則、GUID、ファイル名、参照、リネームと別名、削除、1 つの Undo / Redo |
| `tool/include/snd/tool/doc_build.hpp`, `src/doc_build.cpp` | 文書の検査とメモリ上のビルド（診断をオブジェクトとメンバに対応付け）、`ParsedBank`（契約の bank_check で読む）、`DiffBuilds` / `ClassifyEdit`（値の編集 / 構造の変更）、`LocateField` と `FieldColumns`（メンバ → バンク・チャンク・行・列・ライブの宛先） |
| `tool/include/snd/tool/project.hpp`, `src/project.cpp` | `Project::Load(const ProjectDocument&)` を追加（既存の `Load(dir)` はそのまま） |
| `tool/include/snd/tool/event_editor.hpp` | Event エディタのモデル |
| `tool/include/snd/tool/mixer_editor.hpp` | ミキサーの構造と `WriteBackMixer` |
| `tool/include/snd/tool/curve_editor.hpp` | カーブ・パラメータ・合成・ランプ |
| `tool/include/snd/tool/spatial_preview.hpp` | 3D 減衰のプレビュー |
| `tool/include/snd/tool/music_editor.hpp` | テンポマップ、タップテンポ、Segment / Playlist / 音楽 Event、遷移ルール表、最小先読み、遷移の試聴 |
| `tool/include/snd/tool/material_editor.hpp` | マテリアル表 |
| `tool/include/snd/tool/patch_editor.hpp` | パッチエディタ |
| `tool/test/doc_fixture.hpp` ほか `*_editor_test.cpp`、`document_test.cpp`、`runtime_compare_test.cpp` | 組 M のテスト（tool/README.md の表） |
| `buildtool/test/source_patch_test.cpp` | 供給元・メモリへの出力・パッチのテスト |
| `spec/interface/project-format.md` | `patch` の種類、`synth.patch`、ツールが書くときの決まり |
| `spec/interface/bank-format.md` | §9（追記）: 仮の任意チャンク `XPCH` |

## 決定の案（受け入れ側が D-番号を振る）

1. **文書の保存**: 変えていない（読み込んだときと構造が同じ）ファイルは書かない。変えたファイルは `json::Write` の形（2 スペース、
   メンバは保存順、末尾改行）。ツールが足すメンバは project-format.md の表の順（`notes` は順位を持たない）で、前に来るべきメンバの
   直後に入れる。数値は往復で同じ double になる最短の `%.{1..17}g`（整数は小数点なし、非有限は 0）。読めなかった `*.json` は
   オブジェクトにせず、書きも消しもしない。
2. **ファイル名**: `<種類のフォルダ>/<名前>.json`（名前の `/` はサブフォルダ、Windows で使えない文字と空白は `_`、`.` で始まる部分は
   `_` を前置、末尾の `.` は `_`）。衝突は大文字小文字を無視して `_2`, `_3` …。保存前に消えたファイルの名前は、元のオブジェクト以外に
   渡さない。リネームは、ファイルが旧名の規則どおりの場所にあるときだけ新しい名前の場所へ移す（`RenameOptions::moveFile`）。
3. **リネームと参照**: 旧名を `aliases` に残すのが既定（`keepAlias`）。旧名で書かれた参照は GUID に書き換える（ビルドは別名を
   参照の解決に使わないため）。他のオブジェクトの旧名（alias）を新しい名前にするのは拒否（ビルドの規則 10.1）。名前を戻すと
   その名前は alias から外れる。
4. **削除**: 参照されているオブジェクトの削除は拒否（`force` で強制。壊れた参照は `BrokenReferences` と buildtool のエラーで出る）。
5. **Undo / Redo**: 文書の編集はすべて 1 つの履歴（エディタをまたぐ）。1 手 = 触ったオブジェクトの前後の写し。
   `BeginGroup`/`EndGroup` で 1 手、グループ外は併合キーが同じ連続を 1 手、元に戻った手は残さない（既存の `EditHistory` と同じ考え方）。
   **ミキサーの履歴との関係**: `MixerModel`（ライブの上書き層の値）の履歴は別のまま。ライブの値を文書に入れるのは
   `WriteBackMixer`（文書の 1 手）。書き戻しはバスのフェーダー・エフェクトのパラメータ・カテゴリのフェーダーだけで、ミュート・ソロ・
   パラメータの上書きは監視の状態として書かない。書き戻し後はホストが `Project::Load(doc)` → `MixerModel::Bind` し直す。
6. **検査**: エディタの問題は buildtool をメモリ上の文書に走らせた診断そのもの（同じ規則・同じ文言・同じ順）。変えていない
   オブジェクトはファイルのテキストで渡すので行・列はファイルのもの、変えたオブジェクトは保存されるテキストのもの。
   行・列から JSON のメンバのパスを求めて `DocIssue::path` に入れる。
7. **値の編集と構造の変更**: 編集の前後をビルドし、チャンクごとに比べる。表の形（行数・列の有無）が同じで、変わったセルが
   すべてライブで書ける（core の `CheckEdit` の規則: F32 列すべてと決まった U32 列、`EVNT.priority` は不可）かつ宛先が引ける
   （core の `ResolveRow` の逆: ID 表、1 段・2 段の所有表、DUCK は対象バスと順位）なら値の編集（`SetValue` 群）。
   それ以外（行数の変化、ブロブ、名前・GUID・別名、ライブで書けない列、バンクの増減・依存の変化）は構造の変更。
   距離カーブ（`EVNT.curveVolume` など）の点は ID の経路で引けないので構造の変更になる。
8. **対応表**: `FieldColumns` に、メンバのパターン → (チャンク, 列) を持ち、`LocateField` がバンクの行を引いて宛先（runtimeId、
   index = 1 段なら番号、2 段なら上位 16 ビット | 下位 16 ビット）と書けるかを返す。全項目をビルドで確かめるテストあり。
9. **カーブの合成**（4.3 と実装の差）: ランタイム（`sys_dsp.cpp`）は `compose` 列を読まず、対象ごとに固定の規則（音量・ピッチは加算、
   ローパス最小、ハイパス最大、**パンは加算**）。設計 4.3 の「他は乗算」とパンの扱いが食い違う。ツールはランタイムと同じに計算し、
   ランタイムが未対応の対象（send / effectParam / spread / reverbSend / blend3D）は `compose`（既定は乗算）で計算する。
   スコープの宣言外の値もランタイムは使う（`scopeMask` を確かめていない）ので、計算では使い、`outsideScope` で報告する。
10. **スナップショット**: `busVolume` はランタイムと同じく「バスの音量への dB の加算（重み × 値）」。`effectParam` / `sendLevel`
    （ランタイム未対応）は絶対値として扱い、重み w ごとにプロジェクトの値から目標へ w だけ寄せる。重みは 0..1 に丸める（D-85）。
    取り込みは、プロジェクトの値と違うバスのフェーダー（オフセットとして）と違うエフェクトのパラメータ。
11. **クリップの長さ**: WAV のフレーム数 / レート。ループ区間（`loop` か smpl）があれば「頭からループ終端まで＋区間 ×（loopCount − 1）
    ＋残り」、無ければ全体 × loopCount。ピッチ 2^(cents/1200) で縮める。loopCount 0 と synth は無限、入れ子はその Event の長さ。
12. **シーケンス種別で使わないメンバ**: 消さずに残し、`UnusedMembers` で表示用に返す（ビルドも無視する）。
13. **音楽の既定ルール**: 行が無いときはランタイムの組み込み（次の小節、フェード 1 拍 / 1 拍）。行のフェードの既定は 0（ビルドと同じ）。
14. **最小先読み**: 1 ブロック＋プライム時間。プライム時間は、メモリ上の素材 0、prepare 付きの遷移 0、頭から始めるストリームで
    preloadMs > 0 なら 0、頭以外から始めるストリームは entryPreload があれば 0 で無ければ設定値、preloadMs 0 のストリームは設定値。
    設定値（ストリームのオープン・シーク・最初のデコード）は機種の値なので `MusicSettings::streamPrimeMs` で受ける（既定 30 ms、仮）。
    State は、そこへ至るルール（と既定）のうち最悪の値。
15. **遷移の試聴**: 遷移元の State のプレイリストの最初の Segment の上で計算する。`lastPosition` は履歴が無いので頭。
    間に合わない時（発火 − 要求 < 最小先読み）は late 規則（nextGrid: 届く次のタイミング、startLate: 要求＋先読み、cancel）。
16. **マテリアル**: 組の上書きは接触の種類によらない（PHYS の pair 表に種類が無い）。単体音が無い側は全体の既定、同じ Event は 1 回。
    材質の番号は配列の位置。末尾への追加は番号を変えない。挿入・移動・削除は番号の付け替え（`Renumbering`）を返す。強さのカーブの
    評価は仮（C-11）。
17. **パッチ**: ノードの表・規則・バイト配置は仮（C-10）。画面上の位置は `position: [x, y]`（`notes` ではない）。出力に届かない
    ノードは警告して除く。既存の「patch は段階 6 まで不可」は `synth.patch` の無いときのエラーとして残した（既存テストは不変で通る）。
18. **ProjectDocument のファイル名の衝突判定**は、ディスクにあって読めなかったファイルも含める（上書きしない）。

### 差し戻しで決まったこと（D-176〜D-180、受け入れ側の決定）と変えた点

| 決定 | 元の案 | 差し戻しで変えたこと |
| --- | --- | --- |
| D-176 文書の保存 | 1、2、18 | 負のゼロは `-0`（`FormatNumber`）。`Number()` は非有限を JSON にならないリテラルにし、`Set` / `Insert` / `Modify` / `Create` が拒否（`FormatNumber` の 0 は最後の守り）。`Save` は一時ファイル＋rename で書き、全部書けてから削除。失敗は `false` と `lastSave()`（`SaveResult`）、残りは保留（dirty）で次の `Save` がやり直す。`SaveTo` は名前を付けて保存（素材・blob・読めなかった *.json を写す、外は `outside`、読めないものは `missing`）。Windows の予約名に `_`、大文字小文字の比較は ASCII だけ |
| D-177 編集の規則と履歴 | 3、4、5 | 値が変わらない設定は true（手を足さない）。全エディタの「すでにその状態」の設定も true。`NewGuid` は 16 回で諦めて ""（`Create` は失敗）。最後の材質の削除は拒否。stem のチャンネルは自分の素材（別ファイルならそのファイル）で確かめ、読めなければ受け入れて `SegmentEditor::Problems()` に出す |
| D-178 検査・分類・対応表 | 6、7、8 | `MemberTable()`（全メンバ: Column / NotInBank / Structure）と `ClassifyMember`、`FieldColumns()` はその Column の分。MTRN / MSTG / MTMP / MMRK / MSEG / MSTM / 距離カーブ（compose と点）/ Stem のカーブ / SYNP / ASST.flags / SNPV.paramIndex を足した。buildtool に `SchemaKinds` / `SchemaMembers`（schema.hpp）を足し、全メンバに分類が付くことをテスト。バンクに無い列は `liveWritable = false`。`BankImage::view()` はビルド時に作り、コピーは共有、遅延生成は錠の下 |
| D-179 計算はランタイムに | 9〜15 | `ComposeIgnored(target)` と `CurveEditor::ComposeIgnored()`、`ComposeResult::composeIgnored`。拍子の変化は直前の拍子の小節線だけ（`AddTempo` / `SetTempo` が拒否、読み込んだ小節の途中の変化は `Problems()`）。`TempoMap::MusicalIsApproximate`。到達できない行がある表の `AutoSort` は理由付きで拒否 |
| D-180 マテリアルとパッチ | 16、17 | `EvaluateStrength` は非有限の入力を 0 に。`ReadPatchChunk` はプログラムの連続、ノードの種類、入力（同じプログラムの前のノード）、対応表のプログラム番号を確かめる。buildtool の PHYS の `put` は大きさ 0 で写さない（同じ形はほかに無い） |

### 差し戻し 2 回目（D-176・D-177 への追記）

- `SaveTo` の「同じディレクトリか」は実体で比べる（`fs::equivalent`、無ければ正規化したパス）。同じなら `Save` と同じ経路
  （`Defect_SaveToSameDirectoryOtherSpellingLeavesStaleFiles`）。
- `SaveTo` の宛先の規則: プロジェクトの中・プロジェクトを含む親は拒否。宛先にこの保存が書かない *.json が前からあれば拒否
  （宛先の下を再帰で見る。`.` で始まるディレクトリは見ない）。拒否は false、理由は `lastSave()`、何も書かず文書と `directory()` は不変。
  外を指す参照は今のまま（写さず `outside`）。
- 値が変わらない設定は JSON を一切書き換えない（`Defect_SameValueSettersRewriteExplicitDefaults`）。
  - 文書: `JsonSameMeaning`（数値は値で比べる）。`Modify` は意味が変わらなければ元のテキストに戻す（手も dirty も増えない）。
    `SetDefaulted(path, value, default)`（省略を既定値とみなして比べる）。`MoveElement` の同じ位置は true で何もしない。
  - 全エディタの設定関数を見直した: 既定値のある数値・列挙・真偽は `SetDefaulted` か「今の意味と同じなら true」。参照は名前で
    書かれていても同じ対象なら書き換えない。オブジェクトを組み直す設定（synth、action、snapshot の値、遷移の行、Stinger、
    tempo、marker、stem、playlist の entry、カーブの点、パッチの parameterCurve、強さのカーブ）は今の値と比べてから書く。
    cone と parameterCurve の params は、ほかのメンバ（notes など）を残して書く。Rename 系の同じ名前は true。

## 契約の変更点

- 契約（`contract/`）は変えていない。livelink のプロトコルも変えていない。
- buildtool の公開ヘッダ: `build.hpp` の `BuildOptions` の末尾に 2 メンバ、新しいヘッダ `source.hpp`、`patch.hpp`。
- プロジェクト形式（凍結対象外）: 種類 `patch`、`synth.patch`。既存の項目の意味の変更は 1 つ: 「`patch` は段階 6 まで不可」を
  「`synth.patch` の参照が要る」にした（参照の無い `{"kind": "patch"}` は従来どおりエラー）。
- バンク形式: 文書に §9（仮の任意チャンク `XPCH`）を追記。形式の版・既存の節は変えていない。

## 他の持ち場への依頼

- `requests/C-10.md`（D・統合担当）: `PTCH` の形式とランタイムの Patch ソース。それまでツールは `XPCH` に出す。
- `requests/C-11.md`（A）: 強さのカーブの評価の式、組の上書きと接触の種類、足音の代替。材質表のライブ反映の口。
- `requests/C-12.md`（B・統合担当、急ぎでない）: カーブ・3D・テンポマップ・ルールの照合の計算を公開の関数に。今は同じ式をツールに持ち、
  ランタイムと比べるテストで守っている。

## ライブでつないでいない箇所

- 文書の編集のライブ反映（`DiffBuilds` の結果を SetValue / ReplaceChunk / ホットリロードに流すこと）は組 P。
- 試聴（`AuditionRequest`、遷移の試聴）を音にするのは組 P。
- パッチ（ランタイムに Patch ソースが無い、`XPCH` を読まない。C-10）。
- マテリアル表（physics が stub、`PHYS` の差し替えの口が無い。C-11）。
- スナップショットの `effectParam` / `sendLevel`、カーブの send / effectParam / spread / reverbSend / blend3D の一部（ランタイムが
  段階3 で未対応）。ツールは計算を見せるだけ。
- 画面（Tela）は組 U。

## Tela への提案

- 組 M は Tela を使っていない（モデルだけ）。提案なし。

## Ergo Studio の sonus プラグインに足せるもの

UI に依存せず使えるモデル（`Sonus::tool_core` だけで動く）:

- `ProjectDocument`（読み込み・編集・Undo / Redo・保存・参照・リネーム）と `CheckDocument` / `BuildDocument`（検査とメモリ上のビルド）、
  `DiffBuilds` / `LocateField`（ライブの差分）。
- `EventEditor`（タイムラインの計算を含む）、`MixerEditor`（`WriteBackMixer` を含む）、`CurveEditor` / `ParameterEditor` /
  `ComposeEvent` / `RampTrajectory`、`SpatialPreview`、`TempoMap` / `TapTempo` / `BeatGrid` / `SegmentEditor` / `PlaylistEditor` /
  `MusicEditor`（最小先読み、遷移の試聴）、`MaterialEditor`（代替の一覧、`EvaluateStrength`）、`PatchEditor`。

## 回した構成と結果

すべて -j2、ビルドディレクトリはスクラッチパッドの `model/` の下（終わったら消した）。警告ゼロ（-Werror）。

| 構成 | コミット | 実行したテスト | 結果 |
| --- | --- | --- | --- |
| 8: `-DSND_BUILD_TOOL_CORE=ON`（gcc Debug） | 9efbb46 | 全部（ctest） | 226 件中 226 件通過（`platform_device_play_test` は既存の Skipped） |
| 2: clang Debug ＋ `SND_BUILD_TOOL_CORE=ON` | af8f4e7 | `-R 'tool\|buildtool'` | 20 件全部通過 |
| 3: `SND_SANITIZE=address,undefined` ＋ `SND_BUILD_TOOL_CORE=ON` | af8f4e7 | `-R 'tool\|buildtool'` | 20 件全部通過 |
| 5: Release ＋ `SND_BUILD_TOOL_CORE=ON` | af8f4e7 | `-R 'tool\|buildtool'` | 20 件全部通過 |
| 1: gcc Debug（ツールなしの通常構成） | af8f4e7 | `-R 'buildtool\|build_pipeline\|integration'` | 40 件全部通過 |
| 9: `SND_BUILD_TOOL=ON`（Tela、`$S/tela-prefix`） | af8f4e7 | `-R 'tool'` | 34 件全部通過（既存の UI テストを含む） |
| 6: `SND_BUILD_TESTS=OFF` Release | af8f4e7 | ビルドのみ | 通過 |
| 7: `SND_CODEC_OPUS=OFF` | af8f4e7 | ビルドのみ | 通過 |
| MinGW `-fsyntax-only -Werror` | af8f4e7 | 新しい・変えた .cpp（tool/src、buildtool/src/patch.cpp、組 M のテスト全部） | 通過。例外: `buildtool/src/build.cpp`・`project.cpp` は、変えていない `detail/file_io.hpp`（`fopen(path.c_str())`）の既存のエラーで止まる |

- 構成 8 の全テストは 9efbb46 で回した。af8f4e7 との差は文書と `document_test.cpp` の書式文字列 1 か所（MinGW 対策）だけで、
  af8f4e7 では構成 2・3・5 で組 M のテストを含む tool / buildtool のテストを回している。af8f4e7 での構成 8 の全テストは回していない。
- TSan（構成 4）は回していない（組 M のモデルはスレッドを使わない）。並行性の繰り返しテストも対象が無いので回していない。

### 差し戻しの後（D-176〜D-180）

| 構成 | コミット | テスト | 結果 |
| --- | --- | --- | --- |
| 8: TOOL_CORE、gcc Debug | 0e2852f | 全部 | 241 件中 238 通過。失敗 3 件は下の「決定と食い違う独立テスト」だけ。`tool_live_mixer_test` は通過 |
| 3 ＋ TOOL_CORE（ASan/UBSan） | 37d9176 | `-R 'tool\|buildtool'` | 35 件中 32 通過（同じ 3 件）。UB の報告なし（`tool_indep_s7_build_test` の E1 も通過） |
| 5 ＋ TOOL_CORE（Release） | 37d9176 | 同上 | 35 件中 32 通過（同じ 3 件） |
| clang ＋ TOOL_CORE | 37d9176 | 同上 | 35 件中 32 通過（同じ 3 件） |
| 9（Tela） | 37d9176 | `-R tool` | 49 件中 46 通過（同じ 3 件） |
| MinGW 構文検査 | 37d9176 | 変えた .cpp | 通過（`buildtool/src/build.cpp`・`project.cpp` は前からの `detail/file_io.hpp` のエラーで止まる） |

0e2852f と 37d9176 の差は文書だけ。`Defect_*` は 8 件中 7 件が通過。通らない 1 件は決定 D-177d と食い違う（下）。

決定と食い違って落ちる独立テスト（変更していない。独立テスト担当が直す）:
- `tool_indep_s7_Defect_EmptyMaterialTableBuildIsUndefinedBehaviour`: 材質を全部消せる前提（`SND_REQUIRE(m.RemoveMaterial(...))`）。
  D-177d で最後の材質の削除は拒否。UB の元（`build.cpp` の PHYS の `put`）は D-180d で直した。
- `tool_indep_s7_document_test` の `IndepS7Doc.C1_NewGuidLayoutAndDeterminism`: 999 回同じ値を返す生成器で `Create` が成功する前提。
  D-177c で 16 回の衝突で諦める。
- `tool_indep_s7_editors_test` の `IndepS7Edit.D14_MusicRules`（813、817、820 行）: 直前の拍子の小節線に当たらない位置での拍子の変化
  （tick 3840×u で beatUnit を変える、tick 7・8 で beatUnit 32 と 4/4 の後）を受け入れる前提。D-179c で拒否。

## 未検証の箇所

- TSan の構成（4）は回していない。
- Windows / macOS での実行（MinGW の構文検査だけ。テストの `doc_fixture.hpp` は `getpid` を使うが MinGW の構文検査は通った）。`buildtool/src/detail/file_io.hpp`（変えていない）が MinGW で通らないため、
  それを含む `build.cpp` / `project.cpp` の MinGW 検査は既存のエラーで止まる（`std::fopen(path.c_str())` が wchar_t）。
- 保存は一時ファイル＋rename（D-176d）。rename が途中で失敗した場合、それより前のファイルは新しく、後は古い（失敗は返す）。
  Windows での rename による上書き（既存ファイルへの `std::filesystem::rename`）は確かめていない。
- `BankImage::view()` を複数スレッドから読むテストは通常の構成で回した。TSan では回していない。
- `SpatialPreview` の複数リスナー（ランタイムも最も近いリスナーだけ）と Doppler（ランタイム未実装）。
- 音楽の遷移の試聴は 1 つの Segment の上の計算。プレイリストを進む・Segment をまたぐテンポ変化・ストール・割り込み中の要求は扱わない。
  ランタイムとの比較はテンポマップ（`Timeline`）と照合（`FindRule`）だけで、遷移の発火の全体（`transition.cpp`）とは比べていない。
- マテリアルの強さのカーブの評価とパッチの形式は仮（C-10、C-11）。
- 大きなプロジェクトでの速さ（`ClassifyEdit` は 2 回ビルドする。WAV の変換はキャッシュ（`DocBuildOptions::cacheDir`）を使える）。
