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

## 未検証の箇所

- TSan の構成（4）は回していない。
- Windows / macOS での実行（MinGW の構文検査だけ。テストの `doc_fixture.hpp` は `getpid` を使うが MinGW の構文検査は通った）。`buildtool/src/detail/file_io.hpp`（変えていない）が MinGW で通らないため、
  それを含む `build.cpp` / `project.cpp` の MinGW 検査は既存のエラーで止まる（`std::fopen(path.c_str())` が wchar_t）。
- 文書の保存のファイル操作は 1 段（一時ファイルと rename を使っていない）。途中で失敗すると一部だけ書かれる。
- `SpatialPreview` の複数リスナー（ランタイムも最も近いリスナーだけ）と Doppler（ランタイム未実装）。
- 音楽の遷移の試聴は 1 つの Segment の上の計算。プレイリストを進む・Segment をまたぐテンポ変化・ストール・割り込み中の要求は扱わない。
  ランタイムとの比較はテンポマップ（`Timeline`）と照合（`FindRule`）だけで、遷移の発火の全体（`transition.cpp`）とは比べていない。
- マテリアルの強さのカーブの評価とパッチの形式は仮（C-10、C-11）。
- 大きなプロジェクトでの速さ（`ClassifyEdit` は 2 回ビルドする。WAV の変換はキャッシュ（`DocBuildOptions::cacheDir`）を使える）。
