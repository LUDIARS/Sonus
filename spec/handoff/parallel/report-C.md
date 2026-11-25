# セッション C の報告: 段階5 ツールの完成（全画面、記録と再生）

- ブランチ: `int/s7-tool`（出発点 `ff5df53`）。先頭のハッシュは末尾の「ブランチの先頭」に書く。
- 決定: D-170〜D-189（D-181 は欠番）。正本は `spec/interface/CONTRACTS.md` の「段階 7-C」。
- 進め方: 4 つの組（L = livelink、M = モデルと buildtool、P = ツール側のライブ機能、U = Tela の画面）に分け、組ごとに「実装（Opus）→ 独立テスト（別の Opus。実装を読む前に性質を書き出す）→ 欠陥と仕様の穴への決定 → 差し戻し → 再確認」を回した。受け入れ側（このセッション）は決定と取り込みと最終確認だけを行い、コードは書いていない。
- 組ごとの詳しい記録（作ったもの、決定の案、構成の結果、未検証）: `spec/handoff/parallel/C-notes/`（`livelink.md`、`model.md`、`live.md`、`ui.md` と、独立テスト担当の `*-indep.md`）。性質の一覧は各テストディレクトリの `indep_s7_*properties.md`。

## 1. 作ったもの

設計書 11 章の残り全部。画面の中身（モデル）はすべて UI に依存しない核 `Sonus::tool_core` に置き、Tela に依存するのは `tool/ui` と `tool/app` だけ。

### 組 L: ライブ接続の残り、記録と再生（`livelink/`）

- **プロトコル 1.1**（`spec/interface/livelink-protocol.md` §8、`contract/internal/include/snd/format/livelink_protocol.hpp`）。minor の取り決めで 1.0 の相手と両方向に動く。
- **接続時の照合**: バンクの一覧（BankList）、決定の保留（差分をすべて送る／拒否する、をツールが選べる）、端の規則。
- **差し替え**（ReplaceChunk の受け付け表）、**試聴の全停止**（試聴だけを止める）、**構造の変更**（予備容量の数え、戻しの記録、切断時の Undo。core への最後の一歩は継ぎ目 `IStructEditor`）、**ホットリロード**（ReloadBank と途中経過）。
- **持ち越し 4 件**: 項目単位の Revert（RevertValue）、セルの読み返し（ReadCell）、Client の更新番号（`Sequences`、`Copy*Since`）、`BlockTiming.budgetMicroseconds` の定義。
- **記録と再生**: 開始の種類と開始時の状態、差し替えと構造の変更のデータを記録に入れて再生、2 つの述語（再生どうしの一致、記録した実行との一致）、決定のログと出力のハッシュ。
- **プロファイラ**: StreamList、BlockDetail。

### 組 M: 編集できるプロジェクトと全エディタのモデル、buildtool（`tool/include`・`tool/src`、`buildtool/`）

- **`ProjectDocument`**: 読み込み・編集・1 本の Undo / Redo・保存（知らないメンバと notes を残す、変えていないファイルは書かない、失敗を隠さない、名前を付けて保存）・GUID・参照・リネームと別名。
- **`doc_build`**: buildtool をメモリ上の文書に走らせる検査とビルド、編集の分類（値の編集／構造の変更）、メンバ → バンクのセルの対応表（全メンバ）。
- **エディタのモデル**: `EventEditor`（トラックとクリップ、シーケンス種別、ランダム幅、アクション、タイムライン）、`MixerEditor`（バスの木、センド、エフェクト、スナップショット、ダッキング、カテゴリ、ボイスリミット、ライブの値の書き戻し）、`ParameterEditor` / `CurveEditor`（スコープ、追従速度、合成の規則の確認）、`SpatialPreview`（3D 減衰）、`SegmentEditor` / `PlaylistEditor` / `MusicEditor`（テンポ・拍子・マーカー、Stem、State、遷移ルール表、遷移の試聴、最小先読み）、`MaterialEditor`（単体音、組の上書き、代替、強さのカーブ）、`PatchEditor`。
- **buildtool**: メモリ上の供給元とメモリへの出力、スキーマの列挙、種類 `patch` と仮の任意チャンク `XPCH`。
- ランタイムと同じ値になることを比べるテスト（`tool_runtime_compare_test`: カーブ、3D、合成、テンポマップ、ルールの照合）。

### 組 P: ツール側のライブ機能、試聴、記録の再生（`tool/include`・`tool/src`、`tool/preview/`）

- **`LiveSync`**: 照合と選択、編集・Undo・Redo のライブ反映（SetValue → ReplaceChunk → StructEdit → ホットリロード）、間引きと送り直し、項目ごとの状態と問題の一覧、未対応の一覧、ホットリロードの経過、予備容量、再接続。
- **プレビューエンジン**（`Sonus::tool_preview`）: ツールの中で動くランタイム。プロセス内の転送路でライブリンクをつなぐので、ゲームが無くても編集の反映・試聴・プロファイラ・記録が同じ経路で動く。
- **`AuditionRouter`**: Event、3D（動かしながら）、音楽の遷移、パラメータとステートの試聴。宛先はゲームかプレビュー。
- **`ProfilerStreams`**: CPU と内訳、メモリの区分、ストリーム、LOD、ストール、イベントログ。
- **記録の再生**: 2 回再生して比べる、記録した実行と比べる、WAV に書き出す、期待のファイルと照らす（テスト用）、不具合報告のフォルダの書き出しと読み戻し。UI なしの入口 `sonus-replay`。
- 項目単位の Revert と読み返し（`CellSync`、`MixerModel::RevertOnly`、`Session::RevertItem` / `ReadBack`）。

### 組 U: Tela の画面（`tool/ui`、`tool/app`）

- 12 画面: Mixer、Buses、Event、Params、3D、Music、Material、Patch、Profiler、Replay、Link、Problems。
- 共通: 画面の切り替え、Undo / Redo、保存と未保存の印、文字入力の回避（`TextEntry`）、ドラッグのルーター、問題の一覧、ヘッドレスの `--screen <名前>`（全画面）。
- `sonus-tool` のオプション: `--preview`、`--send-diffs`、`--reload-dir` / `--reload-prefix`、`--report-dir`。

### テスト

- 実装担当のテストと、独立テスト担当のテスト（`indep_s7_*`: livelink、tool のモデル、buildtool、ライブ、UI）。独立テストが見つけた欠陥は計 **48 件**（L 2、M 10、P 15、U 11 と、仕様の穴 40 件余り）で、すべて決定のうえ直し、`Defect_*` のテストは通る状態で残してある。
- ファズ（14 章）: 壊れたライブリンクのメッセージ（1.1 の全種別、両方向）、壊れた記録、壊れた XPCH、壊れたプロジェクトの JSON、壊れた不具合報告と設定のテキスト。ASan / UBSan で報告なし。

## 2. 決定（D-番号ごと）

正本は CONTRACTS.md。ここは見出しだけ。

| # | 見出し |
| --- | --- |
| D-170 | ライブリンク 1.1 と minor の取り決め、照合の端の規則、Client の更新番号と保持数、Server は 1 つ、既存のテスト 3 か所の更新 |
| D-171 | 項目単位の Revert・読み返し・試聴の全停止を、既存のコマンドの印で運ぶ |
| D-172 | 構造の変更と予備容量（継ぎ目 `IStructEditor`、戻しの記録の枠、Remove の扱い） |
| D-173 | 差し替えの受け付け表とホットリロード |
| D-174 | `budgetMicroseconds` の定義、プロファイラ 1.1 |
| D-175 | 記録 1.1 と再生（2 つの述語、`Finished`、決定のログと出力のハッシュ） |
| D-176 | 文書の保存 |
| D-177 | 編集の規則と履歴 |
| D-178 | 検査、値の編集と構造の変更、対応表 |
| D-179 | ツールの計算は今のランタイムに合わせる |
| D-180 | マテリアル表とパッチ（仮の形式 `XPCH`） |
| D-181 | 欠番 |
| D-182 | 接続時の照合と再接続（ツール側） |
| D-183 | 編集のライブ反映 |
| D-184 | ホットリロードの状態、プレビューエンジン、試聴（遷移の試聴はランタイムの規則が正） |
| D-185 | 記録の再生と不具合報告、`sonus-replay` |
| D-186 | 画面の構成と取り消し、入力中のキー、Esc、選択 |
| D-187 | 入力（文字、数値、ドラッグ、ホイール） |
| D-188 | ミキサーと文書、ライブの上書きの引き継ぎ |
| D-189 | 要素の ID と表示 |

## 3. 契約の変更点（統合担当が衝突を解くための一覧）

- **契約の版（1.2）とバンク形式の版（2.0）は変えていない。新しいコマンド種別は足していない。** 公開ヘッダ（`contract/public`）、`commands.hpp`、スキーマは変えていない。`api/compose/c_api.cpp` も変えていない。
- **`contract/internal/include/snd/format/livelink_protocol.hpp`（追加だけ）**: `kProtocolMinor` 0 → 1。`MessageType` の末尾に RevertValue 0x0019、ReadCell 0x001A、QueryBanks 0x001B、ReloadBank 0x001C、CellValue 0x0108、BankList 0x0109、StreamList 0x010A、BlockDetail 0x010B、ReloadStatus 0x010C。`ProfilerMask` に bit7・bit8。構造体 `ReloadBank`、`CellValue`、`BankEntry`、`StreamInfo`、`BlockDetail`、`ReloadStatus`、`RecordLiveData`、`RecordBegin`、`RecordCell`、`RecordOverride` と列挙・定数（`kRecordLiveData`〜`kRecordOverride`、`kStructEditTag`、`MinorOf`、`ProfilerMaskOf`）。既存の値・大きさ・`static_assert` は変えていない。
- **`spec/interface/livelink-protocol.md`**: 1.1（§3・§4・§6・§7 の追記と §8）。
- **`spec/interface/project-format.md`**（凍結対象外）: 種類 `patch` と `synth.patch`、ツールが書くときの決まり（保存、ファイル名、予約名）。「`patch` は段階 6 まで不可」は「`synth.patch` の参照が要る」に変えた。
- **`spec/interface/bank-format.md`**: §9（仮の任意チャンク `XPCH`）を末尾に追記。版は変えていない。セッション D の `PTCH` が入ったら `XPCH` は外す（依頼 C-10）。
- **`spec/interface/CONTRACTS.md`**: 「段階 7-C」の節だけ（§7 へ移す 1 行は節の末尾）。
- **持ち場の外で変えたファイル（1 つの例外）**: `tests/integration/livelink_override_test.cpp`、`indep_livelink_protocol_test.cpp`、`indep_livelink_profile_record_test.cpp` の計 3 か所のアサーション。プロトコルを 1.1 に上げる・試聴の全停止を動かす・差し替えを再生する、という指示書の内容そのものと食い違うため、手順書（`spec/handoff/README.md` 3 章の 4「決定と食い違うテストの更新」）に従って独立テスト担当が更新した（各行に「D-170」のコメント。経緯は依頼 C-09）。ほかのセッションが同じ 3 ファイルを変えていれば、ここだけ衝突する。
- **ビルド**: ルートの `CMakeLists.txt` と `cmake/` は変えていない。`tool/CMakeLists.txt` に、依存表の外のライブラリ `sonus_tool_preview`（`Sonus::tool_preview`、ランタイムの全モジュールにリンク）、`sonus-replay`、`sonus_tool_ui_replay` を足した。`SND_BUILD_TOOL_CORE=ON` で核・プレビュー・`sonus-replay` ができ、Tela は要らない。
- buildtool の公開ヘッダ: `BuildOptions` の末尾に `source` / `memoryOutput`、新しいヘッダ `source.hpp`、`patch.hpp`、`schema.hpp`。CLI の振る舞いは変わらない。

## 4. 他の持ち場への依頼（`spec/handoff/parallel/requests/`）

| 番号 | 依頼先 | 内容 | それまでの扱い |
| --- | --- | --- | --- |
| C-01 | core（B） | 予備容量の範囲で構造の変更を反映する関数 | 継ぎ目の既定は UNSUPPORTED。ツールはホットリロードに回す |
| C-02 | core（B） | 試聴の再生ハンドルを記録の時点で返す | 全停止だけ（試聴ごとの停止は無い） |
| C-03 | bank / core / api | 鳴ったままのホットリロード、バイト列からの読み込み | そのバンクの音を止めてパスから読み直す |
| C-04 | core（B） | 記録の開始時の core の状態の書き出しと読み戻し | 途中からの記録は「記録した実行と一致しない場合がある」と報告 |
| C-05 | core（B） | 再生で LOD の段とストールを再現する手段 | 再現せず、条件として報告 |
| C-06 | core / source / jobs（B） | ストリームの詳細、ワーカーごとの時間 | スロットごとの Event・状態・ストールの数だけ |
| C-07 | core / music（B） | 決定のタップ（選ばれた Clip、遷移） | 決定のログは Play / 試聴 / Stop / 終わり / イベント / ボイスの要約 |
| C-08 | （組 P で対応済み） | ツールが使う 1.1 の API の一覧 | 済み |
| C-09 | tests の持ち主 / 統合担当 | 1.1 と食い違う既存のテスト 3 か所 | D-170 で更新済み（上の 3 章） |
| C-10 | D / 統合担当 | `PTCH` の形式とランタイムの Patch ソース | 仮の `XPCH` に出す。ライブは未対応として一覧 |
| C-11 | A | 強さのカーブの評価式、組の上書きと接触の種類、材質表のライブ反映の口 | 評価は仮。ライブは未対応として一覧 |
| C-12 | B / 統合担当 | カーブ・3D・テンポマップ・照合の計算を公開の関数に | 同じ式をツールに持ち、比べるテストで守る |
| C-13 | B / 統合担当 | カーブの合成が設計 4.3 とランタイムで食い違う（パンの加算、`compose` を読まない、スコープの宣言） | ツールはランタイムに合わせ、警告を出す |
| C-14 | 統合担当（契約、music） | 拍の中の tick の頭打ち（2 分音符の拍）、小節の途中の拍子変化 | ツールはランタイムと同じ値、エディタで制限 |
| C-20 | api / 統合担当 | ツールの中でランタイムを組み立てる口 | `tool/preview` が D-93 の順を写して組み立てている |
| C-21 | core / 統合担当 | 再生でブロック番号をそろえる（描かずにバンクを登録する）口 | 上限つきの空回し |

livelink の中で次に足すとよいもの（依頼ではなく、次の minor の候補。`C-notes/live.md`）: ゲームの予備容量の使用量を返すメッセージ、記録の開始ブロックを Ack で返す、バンクを外すメッセージ、ReadCell のまとめ読み、ReplaceChunk が拒んだ列の名前、世代つきの `DecideSync`。

## 5. ライブでつないでいない箇所

| 箇所 | 今の扱い | つなぐ条件 |
| --- | --- | --- |
| 構造の変更（バス・エフェクト・センドの追加と削除）が core に反映されること | ランタイムは UNSUPPORTED。ツールはホットリロードに回す。経路は偽の編集器でテスト済み | C-01 |
| バスの追加を StructEdit で送ること | 今のバンク形式ではマスターの行番号が動くので、常にホットリロード | バンク形式か C-01 の設計しだい |
| ライブで足したオブジェクトの値の編集 | ホットリロードが要る状態として出す | C-01 |
| マテリアル表（PHYS） | 文書とビルドまで。ライブは未対応として一覧（`LiveSupportOf`） | A の統合、C-11 |
| 波形生成パッチ | 文書、検査、`XPCH` まで。音は出ない | D の統合、C-10 |
| ランタイムが未対応のカーブの対象（send、effectParam、spread、reverbSend、blend3D）、`compose`、スナップショットの effectParam / sendLevel | ツールは計算を見せるだけ。ライブは未対応として一覧 | B の統合、C-13 |
| 試聴ごとの停止 | 全停止だけ | C-02 |
| ゲームが宛先のときの、エミッタとリスナーを動かす試聴、ブロックに合わせた遷移の試聴 | プレビューエンジンが宛先のときだけ。ゲームでは理由つきで無効 | プロトコルの追加 |
| 鳴ったままのホットリロード、バイト列の転送 | 音を止めてパスから読み直す | C-03 |
| 途中から始めた記録の core の状態、LOD・ストールの再現、決定のログの Clip・遷移 | 条件として報告 | C-04、C-05、C-07 |
| ストリームの先読みの残り・状態の詳細、ワーカー別の CPU | 出していない | C-06 |
| 言語バンクのプレビューでの読み込み、文書からバンクを消したときにゲームから外すこと | 一覧に出すだけ | livelink の追加 |
| `Config.dev_reserve.events`（イベントを足す） | 対応する op が無い | 構造の変更の種類の追加 |
| ドップラー（`SpatialPreview` の速度） | モデルにはあるが画面に出していない | ランタイムの対応 |

統合後につなぐ場所は 1 か所にまとめてある: 未対応の分類は `tool::LiveSupportOf`、構造の変更は `livelink::IStructEditor` の実装を `RuntimeDesc::structEditor` に渡す。

## 6. Ergo Studio の sonus プラグインに足せる画面

核（`Sonus::tool_core`）だけで動くモデルを、そのまま使える。再生とプレビューは `Sonus::tool_preview`（Tela なし）。

| 画面 | 使うモデル |
| --- | --- |
| Event エディタ | `EventEditor`（`ComputeTimeline`、`Cone`）、`ProjectDocument`、`CheckDocument` |
| ミキサーの構造（センド、エフェクト、スナップショット、ダッキング、カテゴリ） | `MixerEditor`、`WriteBackMixer`、ライブの値は `MixerModel`（`RevertOnly`、`Rebind`） |
| パラメータとカーブ | `ParameterEditor`、`CurveEditor`、`ComposeEvent`、`RampTrajectory` |
| 3D 減衰のプレビュー | `SpatialPreview`、`AuditionRouter` |
| 音楽 | `SegmentEditor`、`PlaylistEditor`、`MusicEditor`（ルール、到達できない行、並べ替え、遷移の試聴、最小先読み）、`TempoMap`、`TapTempo` |
| マテリアル表 | `MaterialEditor`、`EvaluateStrength` |
| パッチ | `PatchEditor` |
| プロファイラ（CPU、メモリ、ストリーム、LOD、イベントログ） | `Profiler`、`ProfilerStreams` |
| 記録と再生、不具合報告 | `recording_report`、`replay_host`（`Sonus::tool_preview`）、`sonus-replay` |
| 接続（照合、選択、ホットリロード、予備容量） | `Session`、`LiveSync`、`Connection` |
| 問題の一覧、保存、Undo / Redo | `ProjectDocument`、`CheckDocument` / `DistinctIssues` |

Tela に依存しない UI の部品（`tool/ui` にあるが Tela を使っていない）: `TextEntry`、`control_math`（`TimeAxis`、`PlotAxes`、`NumberDrag`、`FaderDbToPos`）。プラグインでも使うなら核へ移すとよい（今回は動かしていない）。

モデルに足すとプラグインが楽になるもの（`C-notes/ui.md`「モデルへの要望」の残り）: クリップの素材を替える関数、種類ごとの作成の関数、JSON のパスを読まずに済む読み出しの関数、記録した実行のログを作る関数、変わったオブジェクトだけの検査（今は編集ごとに文書全体をビルドする）。

## 7. Tela への提案

`hq-handover.md` 3 章の 9 件（そのまま有効。今回も全部に回避を入れた）:

1. canvas の exclusive を retained で持つ
2. ElementPointer
3. wheel、修飾キー、click_count
4. text_input
5. 例外を使わない検査 API
6. Linux の文字の描画
7. MacOSView
8. 要素の見た目の指定
9. drawing の差し替え

今回の画面で足りなかったもの（10〜20。回避はすべてツールの中に入れた）:

| # | 欲しいもの | ツールの中の回避 |
| --- | --- | --- |
| 10 | 文字でないキーのイベント（Enter、Esc、矢印、Delete、F キー、修飾）を、フォーカスのある要素へ | ホストでキーを観測して `App::Key` に渡す |
| 11 | ウィンドウのショートカットを止める口（Windows の Esc で閉じる、F11） | Esc は常にツールが先に取り、Tela に渡さない |
| 12 | 重ね表示（ポップアップ、モーダル）の層 | 画面全体の排他入力のボタンを下敷きに、一覧を宣言の最後に置く |
| 13 | 無効の状態（押せないボタン） | 文字を「(ラベル)」にする |
| 14 | スクロールする入れ物（切り抜きとホイール） | ページ送りと、図の上のホイールを自分で受ける |
| 15 | 要素の数（4096）と図形の数（256 / 4096 点）の上限の引き上げ、または仮想化 | レーンごとに図を分ける、見えている物だけ、ページ送り、間引き |
| 16 | 文字の幅の計測 | バイト数で切る |
| 17 | ホバーとツールチップ | なし（値は欄や表に出す） |
| 18 | 要素ごとのカーソルの形 | なし |
| 19 | 要素の間のドラッグ＆ドロップ | 離した点を画面の側で端子の位置と照らす |
| 20 | canvas の中の当たり判定（図形ごと） | 図形ごとに文字なしの排他入力のボタンを下に置く |

文字入力（4 と 10）は影響がいちばん大きい。今は名前・数値・参照のすべてを `TextEntry` と候補の一覧で回避している。

## 8. 構成ごとの結果

（最終確認の結果。末尾に書く）

## 9. 未検証の箇所

- **Windows / macOS での実行**: MinGW の構文検査（`-fsyntax-only -Werror`）だけ。保存の rename による上書き、予約名の実ファイル、Esc がウィンドウを閉じないこと、`host_macos.mm`（Objective-C++ のコンパイラが無く検査していない）は未確認。変えていない `buildtool/src/detail/file_io.hpp` が MinGW で通らない（`fopen` に wchar_t のパス。以前から）ので、それを含む `build.cpp` / `project.cpp` の構文検査はそこで止まる。
- **実デバイスの音**: コンテナにデバイスが無い。プレビューの出力、Listen（`OutputPlayer`）、試聴は無音のデバイスと手動の出力で、出力のサンプルを解析的に確かめた。
- **TCP の本物のゲーム**: 1.1 のテストはメモリの転送路。1.0 のゲームは手で組んだ相手で確かめた。
- **構造の変更が core に反映されること**（C-01 まで偽の編集器）。予備容量を超える StructEdit のツール側の経路。
- **言語バンクと、依存のあるバンクのホットリロード**。実際にストリームを読む再生での StreamList。ストリーム素材のプレビュー（Opus の pre-roll は `StreamPrimeMs` に入っていない）。
- **読み込み中でまだ一覧に無いゲームのバンクと同じパスへのリロード**（前からの登録と見分けられない）。
- **大きなプロジェクトでの速さ**: 検査とライブ反映が編集ごとに文書全体をビルドする。画面は上限を越えないこと（Event 数百〜数千、遷移ルール 100 行以上、ノード上限のパッチ）だけ確かめた。
- **文字の描画**（Pictor）と、画素の色の区分。画像のテストは図形の位置と大きさを画素で数えている。
- **遷移の試聴**: 1 つの Segment の上の計算。プレイリストを進む場合、Segment をまたぐテンポ変化、ストール、割り込み中の要求は扱っていない。
- **`DecideSync` の世代の照合と送信の間のごく短い隙間**（Client に世代つきで答える口が無い）。
- 仮のもの: 強さのカーブの評価（C-11）、パッチの形式（C-10）、ストリームのプライムの既定 30 ms。

## 10. ブランチの先頭

（末尾に書く）
