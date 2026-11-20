# 組 M（編集できるプロジェクトと全エディタのモデル、buildtool）の独立テスト: 満たすべき性質

独立テスト担当が、**実装（`tool/src`、`buildtool/src`）と実装担当のテストを読む前に**、仕様と公開ヘッダだけから書き出した性質。
読んだもの: `spec/interface/project-format.md`、`spec/interface/bank-format.md`（§6、§9）、設計書 2.5・4 章・5 章・8 章・9 章・10 章・11 章・14 章、
CONTRACTS.md（D-32、D-69、D-74、D-85）、`tool/include/snd/tool/{document,doc_build,event_editor,mixer_editor,curve_editor,
spatial_preview,music_editor,material_editor,patch_editor,mixer,project}.hpp`、`buildtool/include/snd/buildtool/{build,source,patch,json}.hpp`、
依頼書 M-impl.md、実装担当のメモ（作ったもの、決定の案）。

テストは `tool/test/indep_s7_*.cpp`（tool_core だけ、Tela 不要）。期待値はゴールデンに頼らず解析的に決める。
「根拠」は仕様の場所、「確かめ方」は期待値の出し方。`[穴]` は仕様が決めていない・食い違う点で、今の振る舞いを記録する。

## A. 文書の読み込みと保存（document.hpp、project-format.md §1・「ツールが書くときの決まり」）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| A1 | 読み込み → 何もせず保存: `PendingChanges` が空、`Save` でファイルの中身も更新時刻も変わらない | document.hpp「Load -> Save -> Load gives the same objects」、依頼書「変えていないファイルは書かれない」 | 一時ディレクトリに書いたプロジェクトを Load → Save、全ファイルのバイト列と mtime を前後で比較 |
| A2 | 読み込み → 保存 → 読み込みで全オブジェクトが `JsonEqual` | document.hpp | 2 つの文書のオブジェクトを GUID で突き合わせる |
| A3 | 知らないメンバ（どの階層でも）と `notes`（どの階層でも、任意の値）が、別のメンバを編集して保存しても残る | project-format.md §1「ツールは自分が知らないメンバを保存時に残すこと」「notes はどの階層にも」 | トップ・`tracks[0]`・`clips[0]`・`random`・`cone`・`music.transitions[0]`・材質表の `single` の中に未知のメンバと `notes` を置き、別のメンバを Set → 保存したテキストを再パースして残っていること、値が同じこと |
| A4 | 編集したファイルはメンバの元の順を保つ。新しいメンバは仕様の表の順で前のメンバの直後に入る | 「ツールが書くときの決まり」 | 順序を入れ替えて書いた event に Set → 保存テキストのメンバ順を見る。足したメンバ（例 `fadeOutMs`）の位置が表の順の前後関係に合う |
| A5 | 編集したファイルは 2 スペースの字下げ、末尾改行 1 つ | 同上 | 保存テキストの末尾と字下げ |
| A6 | 編集していない数値はリテラルが変わらない（`1e-7` は `1e-7` のまま、`0.10` も）。編集した数値は往復で同じ double（0.1、1e-7、2^53、2^53+1 の整数リテラル、-0.0、1e300、5e-324） | document.hpp「Numbers that were not edited keep their literal」、FormatNumber | 別メンバを編集して保存しリテラルを文字列比較。`FormatNumber(v)` を `strtod` で読み戻してビット一致（-0 は符号も）。NaN/Inf は "0" |
| A7 | 往復で値が変わらない: 大きな整数（`2^63-1` のフレーム）は編集しなければリテラルが残る | json.hpp「Numbers keep their source text」 | segment の `entryFrame` に巨大な整数を書いて別メンバを編集・保存 |
| A8 | UTF-8 の名前（日本語、絵文字）、BOM 付きファイル、エスケープ（`\"`, `\\`, `\u0001`, `\n`）が往復で同じ文字列 | project-format.md §1「UTF-8、BOM は読み飛ばす」 | 読み込んだ値が復号済みの文字列と一致。編集・保存・再読み込みで同じ |
| A9 | BOM 付きで読んだファイルは、変えなければバイト列（BOM ごと）がそのまま | A1 と同じ（変えていないファイルは書かない） | ディスクのバイト列比較 |
| A10 | 新しいオブジェクト: `<フォルダ>/<名前>.json`、`/` はサブフォルダ、使えない文字は `_`、`.` 始まりは `_` 前置、衝突は大文字小文字を無視して `_2`, `_3` | 「ツールが書くときの決まり」 | `FileNameFor` と `Create` → `PendingChanges` の Write のパス |
| A11 | 削除は保存でファイルを消す。リネーム（旧名の場所にあるとき）はファイルを移す（旧ファイルの Delete と新ファイルの Write） | 同上 | `PendingChanges` の Op とパス、Save 後のディスク |
| A12 | 読めない `*.json`（不正な JSON、オブジェクトでないもの）は書きも消しもしない | document.hpp「never written」 | 壊れたファイルを含むディレクトリで編集・保存して、そのファイルのバイト列が同じ |
| A13 | 書き込みの失敗（読み取り専用のディレクトリ）で `Save` が false、文書は壊れない（オブジェクトは編集後のまま、`IsDirty` のまま、もう一度書ける） | 依頼書「書き込みの失敗で文書が壊れない」 | chmod 0555 のサブフォルダに書くオブジェクトを編集して Save（root では権限が効かないので、効かないときは記録して飛ばす） |
| A14 | 保存したあとは clean（`PendingChanges` 空）。保存したディレクトリを読み直すと同じ内容 | document.hpp Save | 編集 → Save → PendingChanges と再 Load |
| A15 | 決定的: 同じ編集列から同じ保存テキスト | 依頼書「決定的な出力」 | 2 つの文書に同じ操作（固定の乱数の GUID）をして `PendingChanges` を比較 |

## B. Undo / Redo（document.hpp）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| B1 | どの操作の列でも、全部取り消すと元の文書（全オブジェクト JsonEqual、`PendingChanges` 空） | document.hpp「Load -> edit -> undo -> Save writes nothing」 | 固定シードの乱数で、エディタをまたいだ操作（Set / Insert / Erase / Move / Create / Remove / Rename / Duplicate / 各エディタの操作）を数百手、全部 Undo |
| B2 | 全部やり直すと取り消す前と同じ | Redo | B1 のあと全部 Redo して、取り消す前の写しと比較 |
| B3 | 1 手ずつ: k 手目まで Undo したら k 手前の状態 | 同上 | 各手のあとの写しを持ち、1 手ずつ戻して比較 |
| B4 | 取り消しの途中で新しい編集をするとやり直しが消える（`CanRedo` false） | document.hpp、EditHistory の考え方 | |
| B5 | 併合: 同じ併合キーの連続は 1 手、違うキーや 0 は別の手。`BeginGroup`/`EndGroup` の中は 1 手（入れ子も 1 手） | document.hpp | ドラッグ（MoveClip を 10 回同じキー）→ UndoCount が 1 増え、Undo 1 回で元 |
| B6 | 何も変えない操作（同じ値の Set、拒否された操作）は手を増やさない | 「No step if the value does not change」「false ... changes nothing」 | UndoCount と revision の前後 |
| B7 | 始まりに戻った手は残らない（グループの中で値を変えて元に戻す） | 「a step that ends where it started is dropped」 | |
| B8 | 取り消しの取り消し: Undo → Redo → Undo の繰り返しで状態が往復する | 依頼書「取り消しの取り消し」 | |
| B9 | エディタをまたいだ順序: event の編集 → バスの編集 → 材質の編集を Undo すると逆順に戻る | 依頼書「複数のエディタにまたがる 1 つの履歴」 | |
| B10 | Create / Remove / Rename の Undo でファイルの扱いも戻る（Create の Undo のあと PendingChanges にそのファイルが無い、Remove の Undo のあと Delete が無い） | B1 | |
| B11 | `revision` は変更ごとに増える。`lastChanged` は最後の手が変えたオブジェクト | document.hpp | |

## C. 参照、リネーム、削除、GUID（document.hpp、project-format.md §1、§5、10.1）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| C1 | `NewGuid` は `xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx`（版 4 の配置、小文字）。乱数の差し替えで決定的 | document.hpp「version 4 layout」 | 固定の乱数で 2 回作って同じ、形を正規表現相当で検査 |
| C2 | `NormalizeGuid` は大文字・`{}` 付きを小文字・括弧なしに、不正な文字列は "" | document.hpp | |
| C3 | `Resolve` はビルドと同じ: GUID（大文字・`{}` 付きも）または名前 | project-format.md §1 | |
| C4 | リネーム: 旧名が `aliases` に入る（keepAlias）、旧名で書かれた参照が GUID に書き換わる | 「ツールが書くときの決まり」、10.1 | 名前で参照する event を用意してバスをリネーム。ビルドが通る |
| C5 | リネームで他のオブジェクトの現在の名前・他のオブジェクトの別名と同じ名前は拒否、文書は不変 | project-format.md §5「別名にする旧名は…」、同じ種類で名前一意 | |
| C6 | 参照されているオブジェクトの削除は拒否（文書不変）、`force` で削除すると `BrokenReferences` に出てビルドがエラー | document.hpp、§1「unknown <種類>」 | |
| C7 | `ReferencesTo` は GUID と名前の両方の参照を返す | document.hpp | |
| C8 | `Create` は空の名前（ducking 以外）、制御文字（U+0000〜U+001F、U+007F）、既にある名前、未知の種類を拒否 | document.hpp、project-format.md §1 | |
| C9 | `Modify` で GUID や `type` を変えるのは拒否、文書は不変 | document.hpp | |
| C10 | `Duplicate` は新しい GUID と名前、中身は同じ（name / guid / aliases 以外） | document.hpp | `[穴]` aliases を複製するか |

## D. エディタの編集と規則（各ヘッダの「Rules」）

拒否したときは false、文書は不変（全オブジェクト JsonEqual、UndoCount 不変）。

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| D1 | Event: startMs・フェード・retriggerMs・fadeOutMs・comboWindowMs・maxDistance・ランダム幅・weight は >= 0（負・NaN・Inf は拒否）、loopCount は 0 以上の整数（1.5 は拒否）、priority 0..255、maxInstances は 0 以上の整数、layer min <= max、cone 0 <= inner <= outer <= 360 | event_editor.hpp | 境界の値（0、255、256、-1e-300、NaN、+Inf、360、360.0001）|
| D2 | Event: クリップの源は 1 つ。入れ子に自分・自分を含む Event を置くのは拒否（直接・間接の循環）。種類違いの参照（Event の位置にバス）は拒否 | event_editor.hpp、4.2 | A→B→C を作り C に A を入れる |
| D3 | Event: 使わないメンバ（例: random 以外での weight）は残り、`UnusedMembers` に出る。種別を戻せば消える | event_editor.hpp | |
| D4 | Event のトラック・クリップ・アクションの全操作（追加、削除、並べ替え、トラック間の移動 = 1 手）と、ビルドしたバンクの `TRAK`/`CLIP`/`ACTN` の行数・列の値 | 依頼書 | |
| D5 | タイムライン: クリップの長さ = フレーム / レート、ループ（`loop`）があれば「頭からループ終端＋区間 × (loopCount − 1)＋残り」、ピッチで 2^(−cents/1200) 倍、loopCount 0 と synth は無限、重なりの区間 | event_editor.hpp、メモ決定 11 | 自作の WAV（48000 Hz、48000 フレーム = 1000 ms）で 1000 ms、ループ [12000, 36000) loopCount 3 で 1000 + 500×2 = 2000 ms、+1200 セントで半分 |
| D6 | ミキサー: マスターは 1 つ、新しいバスに親が要る、親の循環・センドを含む循環・自分へのセンドは拒否 | mixer_editor.hpp、project-format.md §4 | |
| D7 | ミキサー: `GraphOrder` は入力が先・マスターが最後・同順位は名前順。ビルドの `BUSS` の行の順と一致 | project-format.md §4 | 名前とセンドの組み合わせで期待の順を手で出す |
| D8 | エフェクトのパラメータは dsp の範囲に丸め、整数は丸め（`MixerModel::Normalize` と同じ）。未知の種別は有限値なら何でも、非有限は拒否 | mixer_editor.hpp | `NormalizeEffectParam` と MixerModel の Normalize を比較 |
| D9 | カテゴリの循環拒否、子や参照のあるバス・カテゴリの削除拒否、maxVoices >= 1 の整数、ダッキングは bank が要る・source の種類（sidechain = バス、rule = カテゴリ）・attack/release >= 0 | mixer_editor.hpp | |
| D10 | スナップショット: `ApplySnapshots` は busVolume = プロジェクトの音量 + Σ(w × value)、重みは 0..1 に丸め、effectParam/sendLevel は w ごとに目標へ寄せる | mixer_editor.hpp、D-85 | 重み 0.5、1.5（→1）、-1（→0）、NaN |
| D11 | `WriteBackMixer`: ライブのフェーダー・エフェクト値・カテゴリのフェーダーが 1 手で文書に入る。ミュート・ソロは書かない。Undo 1 回で戻る | mixer_editor.hpp、11.2 | |
| D12 | カーブ: x の昇順を保つ（隣を越える移動は拒否、同じ x は可）、2 点未満にならない、shape >= 0、非有限は拒否 | curve_editor.hpp | |
| D13 | パラメータ: スコープは空でない部分集合（0 と 8 以上のビットは拒否）、continuous は min < max、既定値は範囲内、rampRate >= 0 | curve_editor.hpp | |
| D14 | 音楽: テンポ点の tick は別々、bpm 1e-3..1e5、beatsPerBar >= 1、beatUnit ∈ {1,2,4,8,16,32}、マーカー名一意・素材の中、entry < exit <= frames、bars >= 1、State 名一意で "*" 不可、ルールは State か "*" | music_editor.hpp | |
| D15 | 音楽: `RenameState` で遷移・初期 State も追従（1 手）。参照のある State の削除は拒否 | music_editor.hpp | |
| D16 | 材質: 末尾の追加は番号不変、挿入・移動・削除は `Renumbering` を返し、`single`/`footsteps`/`curves`/`pairs` が追従（1 手）。組は対称（(a,b) と (b,a) は 1 つ） | material_editor.hpp、9.3 | |
| D17 | パッチ: ノード数 32 まで（33 個目は拒否）、id 一意で空でない、出力ノード 1 つ、パラメータ範囲、列挙は名前、接続は信号の型が合う・1 入力 1 本・自己接続と循環は拒否 | patch_editor.hpp、project-format.md patch | |

## E. 検査とビルド（doc_build.hpp）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| E1 | エディタが「問題なし」（`CheckDocument` にエラーなし）の文書は必ず `BuildDocument` が ok、逆にビルドできない文書には同じファイル・同じ箇所のエラーがある | 依頼書「エディタが問題なしと言う文書は必ずビルドでき…」 | 乱数の編集列の各段階で `CheckDocument` のエラー数と `BuildDocument.ok` の一致 |
| E2 | 診断の `guid` と `path` が問題の箇所を指す（未知の参照 → その参照のメンバのパス）。変えていないオブジェクトの行・列はファイルの行・列 | doc_build.hpp | 壊れた参照を `tracks[0].clips[0].asset` に書き、DocIssue の path と行・列 |
| E3 | ディスクのビルド（`sonus-build` と同じ `Build` を projectDir で）とメモリ上のビルドが同じバンクのバイト列 | source.hpp「The rules and the diagnostics are the same」 | Save したディレクトリを `buildtool::Build` し、`BuildDocument` のバンクと比較 |
| E4 | 決定的: 同じ文書から同じバイト列・同じ buildHash | project-format.md §7 | 2 回ビルド。文書を作り直しても同じ |
| E5 | 編集した値がバンクの該当チャンク・行・列に入る: バスの volumeDb → `BUSS.volumeDb`、クリップの startMs → `CLIP.startMs`、パラメータの rampRate → `PARM.rampRate`、遷移の fadeOutBeats → `MTRN.fadeOutBeats`、セグメントのテンポ → `MTMP.bpm`、材質表の Event → `PHYS` の配列、ダッキングの amountDb → `DUCK.amountDb` | bank-format.md §6 | ParsedBank で読む（F32 は float に丸めた値と一致） |
| E6 | `LocateField` が返すセルの値が、実際のバンクのそのチャンク・列・行の値と同じ（対応表が実際のバンクと合う） | doc_build.hpp | 全 `FieldColumns` のうち文書にあるメンバについて `Cell` を引いて `bits` と比較 |
| E7 | 値だけの編集（バスの volumeDb、クリップの volumeDb、遷移の fadeInBeats）は `ClassifyEdit` = Value で、`ValueEdit` のセルが編集したセル。構造の変更（クリップの追加、バスの追加、名前の変更）は Structure。notes・パッチの `position` だけの編集は None | doc_build.hpp | |
| E8 | `DiffBuilds(b, b)` は None で空 | 同上 | |
| E9 | 診断の `DocIssue::text` が buildtool の `ファイル:行:列: error: ...` の形 | doc_build.hpp | |

## F. 計算で出すもの

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| F1 | カーブの評価: 最初の点以前は最初の y、最後以降は最後の y、間は y0 + (y1−y0)·t^shape（左の点の shape）、x 重複の点は t = 1 | curve_editor.hpp | 手計算の値。NaN の入力で落ちない |
| F2 | 合成 4.3: 音量 dB は加算、ピッチ セントは加算、ローパスは最小、ハイパスは最大、他は乗算 | 設計 4.3 | 1 つの対象に 2 本のカーブ |
| F3 | スコープの優先: playback → emitter → global → 既定 | 4.3 | 各組み合わせ（8 通り） |
| F4 | 宣言外のスコープの値は `outsideScope` に出る | curve_editor.hpp | |
| F5 | ランプ: 1 ブロックで rampRate × blockFrames / sampleRate だけ近づき、目標を越えない。0 は即時、switch は即時 | 4.3、EvaluateLogicParams | 手計算の列 |
| F6 | 3D: 距離、逆距離 1/max(d,1)、maxDistance を越えると 0、コーンの角度と減衰（inner 以内 0 dB、outer 以上 outerDb、間は線形）、方位 atan2(x, z) | spatial_preview.hpp、4.5 | 手計算の配置 |
| F7 | 3D: リスナーの向き（ヨー）で方向が回る（リスナーを 90° 回すと前のエミッタが横に） | 同上 | |
| F8 | テンポマップ: 一定テンポの秒 ⇄ tick、拍子の変化をまたぐ小節位置（4/4 2 小節 → 3/4: 小節 2 の頭は 7680 tick、小節 3 は 7680+2880）、6/8 の拍 = 480 tick、テンポ変化をまたぐ秒（積分） | 8.1、D-71 | 手計算 |
| F9 | 拍頭ちょうどの NextBeat / NextBar はその点（>=、D-69） | D-69 | |
| F10 | `TickAtSeconds(SecondsAtTick(t)) == t`（往復）、`TickOfMusical(Musical(t)) == t` | 同上 | 乱数の t |
| F11 | タップテンポ: 等間隔のタップ（0.5 秒）で 120 BPM、オフセットは [0, 拍) | 8.7 | |
| F12 | 遷移の照合: 上から最初の一致、`*` はワイルドカード、無ければ -1 | 8.5 | |
| F13 | 到達できない行: 前の行に覆われる行が `Unreachable` に出る（`*→*` の後ろの行、同じ from/to の 2 行目、`A→*` の後ろの `A→B`） | 8.5 | |
| F14 | 特定度の並べ替えは、照合の結果（どのルールの**内容**が選ばれるか）を変えない（到達できない行がない表で） | 8.5、依頼書「特定度の並べ替えが照合の結果を変えないこと」 | 乱数の表で全 from/to について並べ替え前後の選ばれた行の内容を比較 |
| F15 | 最小先読み = 1 ブロック（blockFrames / sampleRate）＋プライム時間。メモリ上の素材 0、prepare 0、頭から始めるストリームで preloadMs > 0 なら 0、それ以外は streamPrimeMs | 8.2、music_editor.hpp | 256/48000 = 5.333 ms |
| F16 | 遷移の試聴: 次の小節の発火位置、フェードの秒（拍 × 60/bpm）、遅すぎるとき late 規則 | 8.5、8.7 | 120 BPM で 1 拍 0.5 秒 |
| F17 | 材質の解決（9.3）: 組 → 単体 → 全体の既定、同じ Event は 1 回、何も無ければ none | 9.3 | |
| F18 | 組の上書きは `PHYS` の `pair[min*N+max]` に入る | 9.3、bank-format §6.2 | バンクの PHYS を自分で読む |

## G. パッチ（patch.hpp、5.1）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| G1 | 依存順: どのノードも入力より後、出力ノードが最後、同順位はファイルの順。同じ文書から同じ配列 | 5.1、patch.hpp | 乱数の DAG で `Compile` の結果を検査 |
| G2 | 循環、33 個のノード、出力ノード 0 個・2 個、型の合わない接続、1 入力に 2 本は `Compile` がエラー | project-format.md patch | |
| G3 | 出力に届かないノードは警告して除く | 同上 | |
| G4 | `WritePatchChunk` → `ReadPatchChunk` の往復で同じプログラム（ノード、入力、パラメータ、点） | bank-format.md §9 | |
| G5 | 壊れた XPCH（短い、件数が大きい、ノードの範囲外）は `ReadPatchChunk` が false で落ちない | 14 章ファズ | |
| G6 | ビルドしたバンクの XPCH の対応表が SYNP の kind = 3 の行を指す | bank-format.md §9 | |

## H. 壊れた入力（ASan / UBSan で落ちない）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| H1 | 不正な JSON、オブジェクトでないもの、型の違うメンバ（`tracks: 5`、`clips: "x"`、`points: [[1]]`）、深い入れ子（10 万段）、巨大な配列、巨大な数（1e400、-1e400、2^64）、空のプロジェクトを読み、各エディタ・検査・ビルド・計算を呼んでも落ちない | 依頼書「壊れた入力」、14 章 | 終了すること、検査がエラーを出すこと |
| H2 | 有効な文書を固定シードで変異させて（文字の削除・置換・挿入）、読み込み・検査・ビルドが落ちない | 14 章 | |
| H3 | 範囲外の添字（トラック 99、クリップ -1 相当の巨大値）はエディタが false で落ちない | 各ヘッダ | |

## 再確認（D-176〜D-180 の後。決定 `_briefs/M-fix.md` と変わった公開ヘッダから）

テストは `tool/test/indep_s7_recheck_test.cpp`、`indep_s7_threads_test.cpp`、`buildtool/test/indep_s7_recheck_test.cpp`。

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| R1 | 保存の途中で 1 つの書き込みが失敗: `Save` false、`lastSave().failures` がそのパスだけ、dirty、保留はその書き込みと（書き込みが全部済むまで待つ）削除だけ、保留でないファイルはディスクが `TextOf` と同じ、一時ファイルが残らない、障害を除けば次の `Save` で全部済み読み直しと一致 | D-176d | 対象のパスを空でないディレクトリにして失敗を作る |
| R1' | 一時ファイルの場所が塞がれていても、半端に書かれたファイルを作らない | D-176d | `<file>.sonus-tmp` を空でないディレクトリに |
| R1'' | 消せなかったファイルは保留のまま、その名前は他のオブジェクトに渡らない、次の `Save` で消える | D-176d | |
| R1''' | 移すファイルの新しい側が書けなければ古い側を消さない（ディスクに同じ GUID が 2 つ残らない） | D-176d | |
| R2 | 名前を付けて保存: 素材・ブロブが同じ相対パスで写る、プロジェクトの外は `outside`、読めないものは `missing`、既にあるファイルは上書き、一時ファイルなし、写したものを読むと同じオブジェクト、ビルドできる | D-176e | |
| R2' | 写せないとき: false、`directory()` は元のまま、元の場所にまだ保存できる | D-176e | |
| R2'' | 同じディレクトリへの `SaveTo` は `Save` と同じ（削除・移動の古い側も消える） | D-176e | |
| R3 | Windows の予約名（大文字小文字を問わず、拡張子の前、サブフォルダの部分も）に `_`、それ以外（COM10、COM0、CONSOLE、XCON）はそのまま | D-176f | |
| R4 | 値が変わらない設定は全エディタで true、手も dirty も増えない（55 の設定関数） | D-177b | |
| R5 | 非有限の数値は全経路で拒否（生の Set・入れ子の値・Insert・Modify・Create の初期値・全エディタの数値の設定、NaN / ±Inf）、文書不変 | D-176c | |
| R6 | GUID は 16 回の衝突で諦める（15 回までは成功）、文書不変 | D-177c | `IndepS7Doc.C1_*` |
| R6' | 最後の材質の削除は拒否・文書不変、材質 0 件の表（生の編集）はビルドのエラーで UB なし | D-177d、D-180d | `Defect_EmptyMaterialTable*`（名前は前のまま） |
| R6'' | stem: 素材の範囲（別ファイルはそのファイル）、境界、2^32 で回らない、読めない素材は受け入れて `Problems()` に出る | D-177d | |
| R7 | 小節線に当たらない拍子の変化は拒否、拍子を変えないテンポの変化はどこでも可 | D-179c | `IndepS7Edit.D14_*` |
| R7' | 読み込んだ小節の途中の変化は `Problems()` に出て、計算はランタイムの `Timeline` と同じ、ビルドの診断は変わらない | D-179c | music の Timeline と比べる |
| R7'' | 2 分音符・全音符の拍は 959 で頭打ち、`MusicalIsApproximate` が真 | D-179d | |
| R7''' | パンは加算、書いた compose は受け入れて計算はランタイムの規則、`ComposeIgnored` / `composeIgnored` に出る | D-179a、b | |
| R7'''' | 到達できない行のある表の AutoSort は理由つきで拒否、文書不変 | D-179e | |
| R7''''' | 強さのカーブの非有限の入力は 0 と同じ結果 | D-180b | |
| R8 | buildtool のスキーマの全メンバに効果（Column / NotInBank / Structure）が付く、notes はどこでも NotInBank、Column の数＝FieldColumns | D-178b | |
| R8' | Column のメンバ（文書の全葉）のセルがバンクのビットと同じ（F32 は値も）、liveWritable なら列がある、数値の編集の DiffBuilds の値の編集は LocateField の live のセルと一致し、live のセルの単独の編集を DiffBuilds がチャンクの置き換えにしない | D-178b、c | |
| R8'' | NotInBank の編集はバンクを変えない | D-178b | |
| R9 | ビルドしたバンクを複数スレッドから同時に読む・コピーする、手で詰めた BankImage の最初の `view()` の同時呼び出しで競合しない | D-178d | TSan、20 回 |
| R10 | 壊れた XPCH の変異 2 万件: 受け入れたものは §9 を満たす（入力は前のノード、プログラム番号は範囲内、種類・点の数）、null / 0 バイトは拒否 | D-180c | ASan / UBSan |
