# セッション C 組 M（編集できるプロジェクトと全エディタのモデル、buildtool）独立テストのメモ

独立テスト担当のメモ。ブランチ `c/model`（ローカルコミットのみ、実装担当の ede5e3f の上）。実装・既存のテストは変えていない。
足したのは `tool/test/indep_s7_*`、`buildtool/test/indep_s7_*`、両方の `CMakeLists.txt` への登録、性質の一覧、このメモだけ。

## 手順

1. 実装を読む前に、仕様（project-format.md、bank-format.md §6・§9、設計 2.5・4・5・8・9・10・11・14 章、CONTRACTS の D-32・D-69・D-74・D-85）、
   公開ヘッダ、依頼書、実装担当のメモの「作ったもの」「決定の案」から性質を書き出してコミット（5eb2341、
   `tool/test/indep_s7_model_properties.md`、A〜H の 80 余り）。
2. 性質ごとにテストを書いた（d2a5cfc）。期待値は解析的（手計算の値、往復の一致、不変条件、固定シードの乱数の操作列）。
3. そのあと実装を読み、実装に固有の箇所（保存の削除、空の任意列、FieldColumns の網羅、パッチの読み込み）を狙うテストを足した（810512d、以降）。

## 作ったもの

| ファイル | 中身 |
| --- | --- |
| `tool/test/indep_s7_model_properties.md` | 性質の一覧（根拠と確かめ方） |
| `tool/test/indep_s7_fixture.hpp` | メモリ上の完全なプロジェクト（バンク、バスの木・センド・エフェクト、カテゴリ、グループ、パラメータ、スナップショット、ダッキング、常駐・ループ・プリロード有無のストリーム素材、Event、3D Event、音楽 Event・Segment・Playlist・ルール・Stinger、材質表、パッチ）と WAV の生成（smpl 付き） |
| `tool/test/indep_s7_ops.hpp` | 全エディタをまたぐ乱数の操作（26 種、Undo / Redo を含む） |
| `tool/test/indep_s7_document_test.cpp` | A（保存と読み込み）、B（Undo / Redo、性質ベース）、C（参照、リネーム、GUID） |
| `tool/test/indep_s7_editors_test.cpp` | D（各エディタの全操作と規則の違反の拒否。拒否したら文書と履歴が不変） |
| `tool/test/indep_s7_compute_test.cpp` | F（カーブ、合成、スコープ、ランプ、3D、テンポマップ、タップテンポ、ルールの照合・到達不能・並べ替え、最小先読み、遷移の試聴、材質の解決） |
| `tool/test/indep_s7_build_test.cpp` | E（検査とビルドの一致、診断の位置、ディスクとメモリのビルドの一致、決定性、バンクの値、LocateField、値/構造の分類）、F18（PHYS）、G6（XPCH と SYNP）、到達不能行とビルドの警告の一致 |
| `tool/test/indep_s7_robust_test.cpp` | H（空、型違い、巨大な数、10 万段の入れ子、20 万要素、壊れた JSON、変異 120 件、範囲外の添字） |
| `tool/test/indep_s7_defects_test.cpp` | 欠陥（tool、7 件） |
| `buildtool/test/indep_s7_patch_test.cpp` | G1〜G5（依存順を参照実装の Kahn と完全一致で 300 個の乱数 DAG、不正なグラフ、範囲と列挙、XPCH の往復、壊れたチャンク） |
| `buildtool/test/indep_s7_defects_test.cpp` | 欠陥（buildtool、1 件） |

CTest: `tool_indep_s7_{document,editors,compute,build,robust}_test`、`buildtool_indep_s7_patch_test`（通る）。
欠陥は 1 件ずつ `tool_indep_s7_Defect_*`、`buildtool_indep_s7_Defect_*`（`--filter` で 1 件だけ走らせる。失敗のまま）。
欠陥の実行ファイル全体のテスト（`tool_indep_s7_defects_test`、`buildtool_indep_s7_defects_test`）は二重に数えないよう DISABLED。

## 性質の一覧と結果

番号は `indep_s7_model_properties.md`。「通過」は構成 8 で通ったもの。

| # | 要約 | テスト | 結果 |
| --- | --- | --- | --- |
| A1 | 読み込み → 保存で中身も更新時刻も不変 | `IndepS7Doc.A1_*` | 通過 |
| A2 | 保存 → 読み込みで同じオブジェクト | `A2_*` | 通過 |
| A3 | 知らないメンバと notes がどの階層でも残る（clip、random、cone、track、action、遷移の行、材質表の中） | `A3_*` | 通過 |
| A4/A5 | メンバの元の順、新メンバは仕様の順、2 スペース、末尾改行、json::Write と同じ | `A4_A5_*` | 通過 |
| A6 | 数値の往復（0.1、1e-7、2^53±1、1e300、5e-324、最小正規数…）、整数は小数点なし、非有限は 0 | `A6_FormatNumberRoundTrips` | 通過 |
| A6 | 負のゼロ | `Defect_NegativeZeroLosesItsSign` | **欠陥** |
| A7 | 編集していないリテラル（1e-7、0.10e1、1E+2、-0、2^63−1、2^64−1）がそのまま | `A6_A7_*` | 通過 |
| A8/A9 | UTF-8（日本語、絵文字、サロゲート対）、エスケープ、BOM、BOM 付きのファイルはバイトごと残る | `A8_A9_*` | 通過 |
| A10 | ファイル名の規則と衝突（`_2`、`_3`、大文字小文字を無視） | `A10_*`（2 件） | 通過 |
| A11 | 削除・リネームの移動・移動しない場合 | `A11_*` | 通過 |
| A12 | 読めない *.json を書かない・消さない・同名を渡さない | `A12_*` | 通過 |
| A13 | 書き込みの失敗で文書が壊れず、障害を除けば保存できる | `A13_*` | 通過 |
| A13' | 削除の失敗 | `Defect_SaveIgnoresAFailedDeletion` | **欠陥** |
| A14 | 保存 → 全部取り消し → 保存でディスクが元に戻る（移動、削除、新規、値） | `A14_SaveThenUndoThenSave*` | 通過 |
| A14' | 別ディレクトリへの保存 | `A14_SaveToAnotherDirectoryIsRecorded` | 記録（仕様の穴 4） |
| A15 | 同じ編集列から同じ保存テキスト | `A15_*` | 通過 |
| B1〜B3、B6、B11 | 乱数の操作列（5 シード × 300 手、全エディタ、途中の Undo / Redo）で、各手の後に UndoCount の段の状態と一致、拒否した操作は文書・履歴不変、全部戻すと元（保存するものなし）、全部やり直すと先頭 | `B1_B2_B3_*` | 通過 |
| B4〜B11 | やり直しの消去、ドラッグ・併合キー・入れ子のグループ、併合は間に別の編集を挟まない、始点に戻った手は残らない、Undo/Redo の往復、エディタをまたいだ順、Create/Remove/Rename の取り消しとファイル、revision と lastChanged | `B4_*`〜`B11_*` | 通過 |
| B6' | 値が変わらない Set の戻り値 | `Defect_SetSameValueReturnsFalse` | **欠陥**（軽い） |
| C1〜C10 | GUID の形（版 4）と決定性、正規化、Resolve、リネームの別名と参照の書き換え、拒否、削除の拒否と force、ReferencesTo、Create の拒否、Modify で同一性を変えない、Duplicate | `C1_*`〜`C10_*` | 通過 |
| D1〜D17 | 各エディタの範囲・型・循環・重複・上限の拒否（文書不変）と全操作 | `IndepS7Edit.*`（19 件） | 通過 |
| D14' | Stem のチャンネルが素材の外 | `Defect_StemChannelsOutsideTheMaterialAccepted` | **欠陥** |
| E1 | 検査にエラーなし ⇔ ビルドできる（乱数の編集 3 シード × 6 回）、問題の guid はそのファイルのオブジェクト | `IndepS7Build.E1_*` | 通過 |
| E2/E9 | 診断のパスと行（変えていない＝ファイルの行、変えた＝保存するテキストの行）、書式、生の編集で作った規則違反・入れ子の循環もエラー | `E2_*`（3 件） | 通過 |
| E3 | メモリ上のビルド ＝ ディスクの `Build`（バンク、ids.json、ヘッダ、ストリーム、buildHash） | `E3_*` | 通過 |
| E4 | 決定性（2 回、別の文書、編集 → 取り消しで同じハッシュ） | `E4_*` | 通過 |
| E5 | 編集した値が BUSS / SEND / DUCK / CATG / CLIP / EVNT / PARM / MTRN / MTMP の該当セルに入る | `E5_*` | 通過 |
| E6 | LocateField のセル＝バンクの値、列は契約の表にある、runtimeId | `E6_*` | 通過（表にある項目） |
| E6' | 音楽・Segment・距離カーブの項目 | `Defect_LocateFieldMissesMusicSegmentAndDistanceCurveMembers` | **欠陥** |
| E6'' | バンクに無い任意列の liveWritable | `Defect_LocateFieldLiveWritableForAbsentColumn` | **欠陥** |
| E7/E8 | 値の編集＝Value（宛先も正しい）、構造＝Structure、notes・パッチの position＝None、使われないバスは None、新しいバンク、DiffBuilds(b, b)＝None、ValueEdit の宛先＝LocateField | `E7_*`（2 件）、`G6_*` | 通過 |
| F1〜F5 | カーブの評価、4.3 の合成、スコープの優先 8 通り、宣言外のスコープ、距離入力、ランプ | `IndepS7Calc.F1_*`〜`F5_*` | 通過 |
| F2' | パンの合成、書いた compose が効かない | `F2_PanCompositionIsRecorded`、`F2_WrittenCompose*` | 記録（仕様の穴 1） |
| F6/F7 | 距離、逆距離、maxDistance の境界、外挿、距離カーブ、2D、コーン 7 角度、方位、パン、リスナーの回転、同位置、表示の変換と Zoom・Fit、試聴の要求 | `F6_*`（3 件）、`F7_*` | 通過 |
| F8〜F10 | テンポマップ（拍子変化、6/8、テンポ変化、≥、グリッド）、往復（小節線上の拍子変化） | `F8_F9_*`、`F10_TempoMapRoundTrips` | 通過 |
| F10' | 小節の途中の拍子変化、2 分音符の拍 | `F10_MidBar*`、`F10_HalfNote*` | 記録（仕様の穴 2、3） |
| F11 | タップテンポ（最小二乗、オフセット、同時刻）、グリッド（4/4、6/8、bpm 0） | `F11_*` | 通過 |
| F12〜F14 | 照合、到達不能（ビルドの警告と行ごとに一致、25 表）、到達不能のない乱数の表 60 個で AutoSort が照合の結果を変えない | `F12_F13_*`、`F14_*`、`F13_UnreachableRowsAreTheBuildWarnings` | 通過 |
| F15/F16 | 最小先読み（1 ブロック＋プライム、prepare、最悪のルール、sameBar と entryPreload、設定）、試聴（発火、フェードの秒、つなぎ、late の 3 規則、テンポ、sameBar の小節） | `F15_*`、`F16_*`（2 件） | 通過 |
| F17/F18 | 材質の解決（9.3 の順、対称、1 回）、代替の一覧、強さ（仮）、PHYS の配列と並べ替えの追従 | `F17_*`（2 件）、`F18_*` | 通過 |
| G1〜G6 | 依存順（参照実装と完全一致、入力は前、出力は最後、決定的）、不正なグラフ 15 種、範囲・列挙・カーブ点、未知のパラメータは警告、XPCH の往復と §9 の配置、壊れたチャンク（全切り詰め、件数、版）、SYNP との対応 | `IndepS7Patch.*`（7 件）、`G6_*` | 通過 |
| G5' | 範囲外の入力番号・プログラム番号 | `Defect_ReadPatchChunkAcceptsOutOfRangeIndices` | **欠陥** |
| H1〜H3 | 壊れた入力で落ちない（全エディタ・検査・ビルド・計算を全オブジェクトに） | `IndepS7Robust.*`（6 件） | 通過（ASan/UBSan でも） |
| H' | 材質の無い材質表のビルド | `Defect_EmptyMaterialTableBuildIsUndefinedBehaviour` | **欠陥**（UBSan の構成で） |

## 欠陥の一覧

| テスト | 再現 | 期待 | 実際 | 根拠 | 重さ |
| --- | --- | --- | --- | --- | --- |
| `tool_indep_s7_Defect_SaveIgnoresAFailedDeletion` | オブジェクトを Remove（またはリネームで移動）し、消すべきファイルが消せない状態で Save | Save が false、削除は保留のまま（dirty） | `fs::remove` の失敗を無視して true、clean になる。移動のときは旧ファイルが残り、次の Load で同じ GUID の 2 ファイル（片方は重複として捨てられる） | document.hpp「Applies PendingChanges… Afterwards the document is clean」、依頼書「書き込みの失敗で文書が壊れない」 | 中 |
| `tool_indep_s7_Defect_LocateFieldMissesMusicSegmentAndDistanceCurveMembers` | `LocateField` に `music.transitions[0].fadeOutBeats`、`music.stingers[0].timingCount`、`tempo[0].bpm`、`markers[0].frame`、`bars`、`distanceCurves.volume.points[1][1]` | バンクの MTRN / MSTG / MTMP / MMRK / MSEG / CPNT のセル | 空（`FieldColumns` にこれらの項目が無い。ほかに stems、asset、synth も無い） | doc_build.hpp、M-impl「どのオブジェクトの編集が…どの列に当たるかを引ける対応表」 | 中（組 P が音楽の値を送れない。ClassifyEdit は構造の変更として扱うので誤送信はない） |
| `tool_indep_s7_Defect_LocateFieldLiveWritableForAbsentColumn` | 全行が既定値でビルドが省いた任意列（CLIP pitchCents / fadeInMs / randFilterHz / layerMax、EVNT comboWindowMs）を LocateField | バンクに列が無ければ liveWritable = false | true。ランタイムの `FindWritableColumn` は無い列に SND_ERR_NOT_FOUND を返すので、組 P の SetValue は失敗する。同じ編集を DiffBuilds は構造の変更（列が増える）にしていて、両者が食い違う | doc_build.hpp CellRef::liveWritable、bank/src/prepared.cpp | 中 |
| `tool_indep_s7_Defect_StemChannelsOutsideTheMaterialAccepted` | ステレオ素材の Segment で `SetStem(0, {1, 2})`、モノラルの別ファイルで `AddStem` / `SetStem({0, 2, asset})` | 拒否 | 受け入れる（AddStem は Segment 自身の素材だけを確かめ、SetStem は確かめない） | music_editor.hpp「stems inside the material's channels」 | 小 |
| `tool_indep_s7_Defect_NegativeZeroLosesItsSign` | `FormatNumber(-0.0)`、`SetNumber(…, -0.0)` → 保存テキスト | 読み戻すと -0 | "0"（`if (v == 0.0) return "0"`） | 依頼書 M-test「負のゼロ」、document.hpp「reads back to the same double」 | 小 |
| `tool_indep_s7_Defect_SetSameValueReturnsFalse` | 読み込んだ値と同じ値を Set | true、手は増えない | false（手は増えない） | document.hpp「No step if the value does not change. false: no object / bad path」 | 小（ヘッダの文言を直すのでもよい。どちらにするか決めてほしい） |
| `tool_indep_s7_Defect_EmptyMaterialTableBuildIsUndefinedBehaviour` | 材質表の材質を全部消して（材質エディタでできる）ビルド。`-DSND_SANITIZE=address,undefined` の構成で | 未定義動作なし | `buildtool/src/build.cpp:2075` の `put(vec.data(), 0)` が `memcpy(…, nullptr, 0)`（UBSan: null pointer passed as argument 2）。ほかの構成では通る（UBSan の構成でだけ失敗）。この行は組 M 以前（ff5df53）からあるが、組 M の材質エディタで到達できる。同じ箇所で UBSan の構成の `IndepS7Build.E1_CheckAgreesWithBuildOverRandomEdits` も失敗する（乱数の編集が材質を全部消す） | 依頼書「壊れた入力…落ちない（ASan / UBSan）」 | 中（未定義動作） |
| `buildtool_indep_s7_Defect_ReadPatchChunkAcceptsOutOfRangeIndices` | XPCH のノードの `inputs[0]` を 1000（範囲外）や 2（後ろのノード）、対応表の program を 7（2 個中）にして ReadPatchChunk | false | true（読んだ側が添字の範囲外を引く） | bank-format.md §9 | 小（仮の形式。ただし D の正式版でも同じ検査が要る） |

## 仕様の穴（期待値を決め打ちせず記録）

1. **カーブの合成（実装担当の決定 9）**: 設計 4.3 は「その他は乗算」、ランタイムはパンを加算し、CURV.compose 列を読まない。
   ツールはランタイムと同じに計算する（パン 0.5＋0.5 = 1）。さらに、音量カーブに compose = multiply を書くとエディタは受け入れて
   表示もする（compose() = 4）が、結果は dB 加算のまま（-6 dB ×2 → -12 dB）。
   - 選択肢: (a) 4.3 を「パンは加算」に直し、compose はランタイムが固定で扱う対象には書けない（エディタが拒否するか、無視される旨の警告）、
     (b) ランタイム（B）が compose を読む。
   - **推奨 (a)**（いまのランタイムに合わせて 4.3 と project-format.md の既定を直し、エディタは効かない compose を拒否または警告する）。
     11 章「ツールで聞いた音とゲームで鳴る音を一致」に沿う。決定 9 の「ランタイムと同じに計算」には賛成。
2. **小節の途中の拍子変化**: 小節位置は連続（その点で端数）なので、その後の「小節・拍・tick」の意味が 8.1 で決まっていない。
   往復の誤差は最大 290 tick（記録）。ランタイムの Timeline も同じ式。推奨: 拍子の変化点は小節線に限る（エディタとビルドで警告）か、
   変化点で小節を数え直すと決める。
3. **2 分音符・全音符の拍**（beatUnit 2、1）: 拍の中の位置は 1919 tick まであるが、`snd_musical_time.tick` は 0〜959（types.h）で、
   ランタイムもツールも 959 で頭打ち（3/2 の tick 1500 → 959）。契約の型の制約。推奨: 契約の注記を「拍の中の tick（4 分音符 960）」にして
   頭打ちをやめる（1.3 で）、またはこの拍子の Musical が近似であることを明記する。ツールはランタイムと一致しているので組 M の欠陥にはしない。
4. **別ディレクトリへの保存（SaveTo）**: オブジェクトは全部書くが WAV / ブロブを写さず、以後そのディレクトリで動くので、
   保存前はビルドできた文書が保存後に 8 件のエラーになる（記録）。推奨: 参照されている素材も写す（「名前を付けて保存」）か、
   SaveTo を「オブジェクトの書き出し」と明記して directory() を変えない。
5. **ファイル名の Windows 予約名**: `CON`、`NUL`、`COM1`、`LPT1` などの名前は `events/CON.json` になり、Windows で書けない。
   大文字小文字の衝突判定は ASCII だけ（`Lower`）で、macOS / Windows の Unicode の大文字小文字の同一視は見ない。
   推奨: 予約名は `_` を前置する（project-format.md の決まりに追記）。
6. **GUID の生成器が同じ値しか返さないとき**、`Create` は終わらない（`NewGuid` が重複のたびに引き直す無限ループ）。
   テスト用の差し替えでしか起きないので欠陥にはしない。推奨: 一定回数で諦めて "" を返す。
7. **材質の強さのカーブ（仮）**: NaN の衝撃量で velocity が NaN（`std::clamp` が NaN を通す）。0 と負は有限。A の評価式が決まるまで記録のみ。
8. **材質を全部消せる**: 材質エディタは最後の材質も消せるが、ビルドは「a material table needs "materials"」のエラーにする（検査が出すので E1 は成り立つ）。
   推奨: エディタが最後の 1 つの削除を断る（規則の違反は編集の前に拒否する、という他のエディタの流儀に合わせる）。
9. **AutoSort と到達できない行**: 到達できない行が無い表では並べ替えても照合の結果は変わらない（60 表で確認）。到達できない行が
   ある表では、並べ替えが「到達できない行」を上げて照合の結果を変えうる。推奨: エディタは到達できない行があるとき AutoSort の前に警告する。

## 実装担当の「決定の案」への賛否

| # | 賛否 | 理由・代案 |
| --- | --- | --- |
| 1 保存 | 条件付き賛成 | 規則どおりに動くことを A1〜A15 で確認。ただし負のゼロ（欠陥）、削除の失敗を無視（欠陥）。非有限の値を `Number` が黙って 0 にするのは危ない: 生の `Set` でも非有限は拒否するのを推奨（エディタは拒否している）。書き込みは 1 段（一時ファイル＋rename でない）なので途中で落ちると一部だけ書かれる（実装担当の未検証のとおり）。 |
| 2 ファイル名 | 賛成 | 仕様の穴 5（Windows 予約名、Unicode の大文字小文字）を足すこと。 |
| 3 リネームと参照 | 賛成 | C4、C5、B11 で確認。 |
| 4 削除 | 賛成 | C6 で確認。 |
| 5 Undo / Redo | 賛成 | 乱数の操作列で各段の状態まで一致。履歴の上限（1024 手）を超えると「全部戻すと元」は成り立たなくなるので、上限をヘッダに書くこと。ミキサーの履歴を別にするのに賛成（D11 で書き戻しが 1 手）。 |
| 6 検査 | 賛成 | E1・E2・E9・F13 で一致を確認。CheckDocument は毎回フルビルド（WAV の変換を含む）なので、画面で打鍵ごとに呼ぶならキャッシュを使うこと。 |
| 7 値と構造の分類 | 賛成 | DiffBuilds は保守的で正しい（空の任意列も構造の変更にする）。LocateField の liveWritable がそれと食い違うのが欠陥。 |
| 8 対応表 | **反対（不完全）** | 音楽・Segment・Stem・距離カーブの点・素材・合成の項目が無い（欠陥）。実装担当のテスト `FieldTableLocatesEveryCell` は表にある項目しか確かめておらず、表の網羅を確かめていない。 |
| 9 カーブの合成 | 計算はランタイムに合わせるのに賛成 | 仕様の穴 1。効かない compose を書かせないこと。 |
| 10 スナップショット | 賛成 | D10 で確認（重みの丸め、NaN → 0）。 |
| 11 クリップの長さ | 賛成 | D5 で `loop`・smpl・ピッチ・無限・入れ子を確認。 |
| 12 使わないメンバ | 賛成 | D3。 |
| 13 音楽の既定ルール | 賛成 | F12。 |
| 14 最小先読み | 賛成 | F15。 |
| 15 遷移の試聴 | 賛成（範囲つき） | F16。最初の Segment の上だけの計算であることを画面に出すこと。 |
| 16 マテリアル | 賛成 | F17・F18。組の上書きが接触の種類によらないのは PHYS の制約（C-11 のとおり）。 |
| 17 パッチ | 賛成 | G1〜G6。未知のパラメータは §1 どおり警告。XPCH の読み込みの検査が欠陥。 |
| 18 衝突判定に読めないファイル | 賛成 | A12。 |

## 実装担当のテストの弱い箇所

- `Document.NumberFormatRoundTrips` に負のゼロ、非正規数、2^53+1 が無い（負のゼロの欠陥を見逃している）。
- `Document.FieldTableLocatesEveryCell` は表（FieldColumns）にある項目だけを、先に既定値以外を入れてから確かめる。表に無い項目（音楽ほか）と、
  列がバンクに無いとき（liveWritable の欠陥）を確かめていない。名前は「全項目」だが表の網羅は確かめていない。
- Undo / Redo は手で選んだ列だけ。乱数の操作列（性質ベース）、拒否した操作で文書が不変であること、保存を挟んだ取り消しが無い。
- 保存の失敗（書き込み・削除）、別ディレクトリへの保存のテストが無い。
- Stem のチャンネルの範囲を SetStem と別ファイルで確かめていない。
- 遷移の試聴はランタイムの `transition.cpp` と比べていない（実装担当も未検証に書いている）。

## 机上で検証した並行部分と結論

- 組 M のモデルはスレッドを使わない（document.hpp「Not thread-safe; the UI thread owns it」）。並行性のテストの対象は無い。TSan と 20 回の繰り返しは回していない。
- 注意点: `BankImage::view()` は const だが中で `parsed_` を遅延生成する（mutable の shared_ptr）。同じ `DocBuild` を 2 つのスレッドから
  見ると競合する。組 P がビルドをワーカーで作って UI で読むなら、渡す前に `view()` を 1 回呼ぶか、ヘッダに「スレッド間で共有しない」と書くこと。
- `ProjectDocument` の既定の乱数は時刻とアドレスから。文書をコピーすると状態ごと写るので、2 つのコピーは同じ GUID を作る（別の文書としては問題ない。
  同じプロジェクトに 2 つのコピーから新規オブジェクトを足して合わせると衝突しうる）。

## 回した構成と結果

ビルドは `$S/model-indep/b-*`、-j2、1 つずつ、終わったら消した。警告ゼロ（-Werror）。

| 構成 | コミット | 実行 | 結果 |
| --- | --- | --- | --- |
| 8: `SND_BUILD_TOOL_CORE=ON`（gcc Debug） | 810512d | 全部（ctest -j2） | 239 件中 231 通過。失敗 8: 欠陥 7 件（意図どおり）と `tool_live_mixer_test`（既存、組 M の独立テストと無関係。単独で再実行して通過、`--repeat until-fail:5` も通過。並行ビルドの負荷のもとでの一過性と見るが原因は未確認）。`platform_device_play_test` は既存の Skipped。 |
| 3 ＋ `SND_BUILD_TOOL_CORE=ON`（ASan/UBSan） | 3854b9d | `-R 'tool\|buildtool'`（35 件） | 欠陥 7 件（意図どおり）と `tool_indep_s7_build_test`（E1 が上の UB の箇所に当たる。欠陥 `Defect_EmptyMaterialTableBuildIsUndefinedBehaviour` と同じ原因）以外は通過。8 件目の欠陥テストは次のコミット（下の行）で足し、その構成で失敗（意図どおり）を確認。`tool_live_mixer_test` は通過 |
| 5 ＋ `SND_BUILD_TOOL_CORE=ON`（Release） | bb5c465 | `-R 'tool\|buildtool'` | 35 件。失敗は欠陥 8 件だけ（7 件は意図どおり。`Defect_EmptyMaterialTableBuildIsUndefinedBehaviour` は当時 `b.ok` も確かめていて Release で失敗したため、その確かめを外した（空の表はビルドのエラーになるのが正しい。欠陥は途中の未定義動作だけ）。外したあとの Release での単独実行は通過、UBSan での失敗は 3854b9d の後の確認どおり同じ経路）。`tool_live_mixer_test` を含むほかは全部通過 |
| clang Debug ＋ `SND_BUILD_TOOL_CORE=ON` | f46d4e0 | `-R 'tool\|buildtool'` | 欠陥 7 件（意図どおり）以外の組 M の独立テストは全部通過。既存の `tool_live_mixer_test` が失敗（構成 8 でも 1 回。単独では通る） |
| MinGW `-fsyntax-only -Werror -Wall -Wextra -fno-exceptions -fno-rtti` | d52907f | 新しい .cpp 全部（tool 7、buildtool 2） | 通過（`%zu` を直したあと） |

回していない構成: 1、2（clang 以外の通常）、4（TSan、対象なし）、6、7、9（Tela。組 M の独立テストは Tela を使わない）。

## 未検証の箇所

- 3D とカーブの合成は解析的な期待値だけで、本物の core の出力とは比べていない（実装担当の runtime_compare_test がある）。
- 遷移の試聴を music の `transition.cpp` と比べていない。テンポマップと照合のランタイムとの比較は実装担当のテストに任せた。
- Windows / macOS での実行（MinGW の構文検査だけ）。ファイル操作のテスト（A13、削除の欠陥）は Linux で、読み取り専用の代わりに
  「ディレクトリが同名のファイル／空でないディレクトリ」で失敗を作った（root では権限が効かないため）。
- 履歴の上限（1024 手）を超える列、非常に大きなプロジェクトの速さ。
- `tool_live_mixer_test`（既存）が構成 8 と clang で 1 回ずつ -j2 の全体実行で失敗した原因（出力が残っていない。単独・ASan・Release では通過）。
