# セッション A（段階4 physics）の報告

- ブランチ: `int/s7-physics`（`int/s2-integration` の `ff5df53` から）
- 受け入れを行った先頭: `8ba0318`（この報告と `CONTRACTS.md` の節は、その上の文書だけのコミット）。最初の受け入れは `1a087ce`（PR #2 を作った時点）で、その後にテストの追加と D-125 が入った
- 進め方: 実装は実装担当のエージェント、テストは実装を見ていない別のエージェント（独立テスト）、決定と受け入れは受け入れ側。
  差し戻しは 3 回（＋小さな修正 1 回）、独立テストの確認は 5 回。3 回目の差し戻しは、追加したテストが見つけた欠陥への対応（D-125）。

## 1. 作ったもの

設計書 9 章の全部。持ち場 `physics/` の中だけで、契約（`contract/`）と凍結ファイルは変えていない。

| もの | 場所 |
| --- | --- |
| 新しい C API `snd_physics_*`（設定、表、対応表、カーブ、統計、観測） | `physics/include/snd/physics/physics.h`（冒頭のコメントが動作規則の正） |
| C++ の薄いラッパー `snd::phys`（ヘッダだけ） | `physics/include/snd/physics/physics.hpp` |
| 本体（凍結の `snd_phys_create` / `destroy` / `submit_contacts` / `play_footstep` の本実装を含む） | `physics/src/physics.cpp`（`physics_stub.cpp` は削除） |
| 実装担当のテスト 8 本（`physics_*_test`） | `physics/test/physics_*_test.cpp`、`phys_rig.hpp` |
| 独立テスト 11 本（`physics_indep_*`）と性質の一覧（P01〜P53） | `physics/test/indep_*`、`INDEP_PROPERTIES.md` |

機能:

- マテリアル対応表（9.1）: ゲームの物理マテリアル → サウンドマテリアル、表面パラメータ（1 材質 4 つまで）。
- 衝突（9.2〜9.4）: `SubmitContacts` の Impact / Break。Event の代替順（組 → 単体の重ね → 全体の既定 → 鳴らさない）、
  強さのカーブ（音量・ベロシティ・ピッチ）、間引き 4 段（閾値 → 格子でまとめ → クールダウン → 予算）。
- 継続する接触（9.5）: Slide / Roll の追跡（開始・更新・タイムアウト・追い出し・やり直し）。
- 足音（9.2）: 重みの大きい 2 つまで、モーション別 → 材質共通 → 全体の既定。
- 位置だけのワンショット `snd_physics_play_at`。
- バンク形式 2.0 の `PHYS` チャンクの読み込み（`snd_physics_load_bank_table`、`snd_phys_create` の `bank` / `table`）。
- 統計（`snd_physics_stats`）と観測コールバック（`snd_physics_record`）。
- 音声側が止まっている間（suspend など）は接触の記録をやめる（D-125）。

## 2. D-番号ごとの決定

本文は `spec/interface/CONTRACTS.md` の「段階 7-A」。D-110〜D-125 を使った（D-126〜D-129 は未使用）。

| # | 内容 |
| --- | --- |
| D-110 | API の置き場所と型（凍結の `snd_phys` を再利用し、`snd_physics_*` を足す） |
| D-111 | 設定・スレッド・メモリ（`struct_size` と `version` の規則、作ったスレッドだけ、生成時に 1 回だけ確保） |
| D-112 | データの置き場所（API が正、形式 2.0 の `PHYS` を読む。足りない分は A-001） |
| D-113 | 表と代替の順、材質不明の規則 |
| D-114 | 対応表と表面パラメータ |
| D-115 | 強さのカーブと y の範囲 |
| D-116 | 時刻（µs の整数、dt の上限） |
| D-117 | 間引き 4 段の境界の規則 |
| D-118 | Break の扱い |
| D-119 | 位置つきの再生と Emitter の保持・解除、取れないときは捨てる |
| D-120 | physics が渡す Parameter の ID |
| D-121 | 足音 |
| D-122 | 継続する接触の追跡の順 |
| D-123 | 戻り値と無効な入力 |
| D-124 | 統計と観測 |
| D-125 | 音声側が止まっている間は記録しない（`block_index` が `stall_calls` 回進まなければ停止中） |

指示書と食い違ったので決めたこと:

- 指示書は `snd_physics_*` を新しく置くよう求めるが、凍結ヘッダに `snd_phys_*` が既にある。型を二重にせず、凍結の型と 4 関数をそのまま本実装にした（D-110）。
- 指示書は「バンクに入れるなら依頼」とするが、`PHYS` チャンクは形式 2.0 に既にあり buildtool も出している。形式を変えずに読み、表せない分だけ A-001 で依頼した（D-112）。

独立テストの後に改めた決定（最初の決定 → 改めた後）:

- Emitter が取れないとき `snd_play_at` に落とす → 捨てて `dropped_no_emitter` に数える。
- 材質不明は全体の既定 → 片方だけ不明なら分かっている側の単体。
- 単発の Emitter は再生の直後に破棄 → 保持し、音声側が再生を適用したのを観測してから破棄（core の欠陥 A-004 のため）。
- 保持の環が満杯なら古いものを早めに破棄 → 破棄せず新しい組を捨てる。
- 大きい `struct_size` は断る → 受けて既知の部分だけ読む（`version` の違いは断る）。
- 音声側が止まっていても接触を記録する → 記録しない（D-125）。追加したテストが、suspend 中に接触を渡し続けると `snd_resume` が通らなくなるのを見つけたため。
  設定の末尾に `stall_calls`、統計に `dropped_audio_stalled` と `audio_stalled_calls` を足した（どちらも physics の自分のヘッダで、契約ではない）。

## 3. 契約の変更点

**無し。** 統合で解く衝突は、`CONTRACTS.md` の末尾への節の追加だけのはず。

- `contract/` 以下: 変更なし（ヘッダ、版、コマンド種別、スキーマ、バンク形式）。
- 凍結ファイル（`tests/framework/`、`tests/mocks/`、ルートの `CMakeLists.txt`、`cmake/`）: 変更なし。
- `api/compose/c_api.cpp`: 変更なし（新しい関数は physics のライブラリの中で定義した）。
- 持ち場の外で変えたファイル: `spec/handoff/parallel/requests/A-001`〜`A-005`、`spec/handoff/parallel/report-A.md`、`spec/interface/CONTRACTS.md`（自分の節）だけ。
- `physics/CMakeLists.txt` と `physics/test/CMakeLists.txt` は持ち場の中で変えた（`physics_stub.cpp` → `physics.cpp`、テストの追加）。

## 4. 他の持ち場への依頼

| 番号 | 宛先 | 内容 |
| --- | --- | --- |
| A-001 | bank / buildtool / contract | バンク形式 2.1 の `PHYS` の拡張（接触種別ごとの組の上書き、モーション別の足音、点列のカーブ、ゲーム材質の対応表と表面パラメータ、閾値） |
| A-002 | api / core | ハンドルを返す `snd_play_at_ex`（入れば Emitter の保持が要らなくなる） |
| A-003 | tool / livelink | 代替が起きたことの一覧のツール表示と、ライブリンクで運ぶ口（physics は観測コールバックまで） |
| A-004 | core | 欠陥 3 点: (1) Emitter を同じ更新で作成・移動・再生・破棄すると位置が効かない、(4 節) 前のブロックから生きている Emitter でも再生と破棄が同じまとまりだと時々位置を失う（Release）、(5 節) Emitter の破棄の後に始まる子の再生が位置を引き継がない。ほかに `snd_play_at` の暗黙の Emitter が同じプールから取られ、取れないと通知なしでリスナーの位置で鳴る件 |
| A-005 | core | 欠陥: suspend 中にコマンドを記録し続けると、`snd_resume` が `COMMAND_BUFFER_FULL` のまま通らなくなる（physics を使わなくても公開 API だけで再現する） |

A-005 が直るまでの physics の動き: 停止中は接触を記録しない（D-125）ので、physics の接触が原因では起きない。足音と `snd_physics_play_at` はゲームが自分で呼ぶものなので停止中も記録する
（2000 フレームの suspend では再開できたが、記録のうち 3744 件はコマンド領域が満杯で失敗し `api_failures` に数えた）。

A-004 が直るまでの physics の動き: 単発の Emitter を保持して、適用を観測してから破棄する。時間をおいて入れ子の Event を鳴らすアクションを持つ Event を
対応表に登録すると、その子の音は位置を失う（`physics.h` に「制限」として書いた）。

## 5. 構成ごとの結果

受け入れ側が、先頭 `8ba0318` で、新しいビルドディレクトリから 1 構成ずつ回した結果（`-j2`、ctest `-j2`）。

| 構成 | ビルド | テスト |
| --- | --- | --- |
| gcc Debug | 警告ゼロ | 229 件中 229 件通過 |
| clang Debug | 警告ゼロ | 229 件中 229 件通過 |
| `-DSND_SANITIZE=address,undefined` | 警告ゼロ | 229 件中 229 件通過、サニタイザの報告なし |
| `-DSND_SANITIZE=thread` | 警告ゼロ | 229 件中 229 件通過、TSan の報告なし |
| Release | 警告ゼロ | 229 件中 229 件通過 |
| `-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release` | 警告ゼロ | （テストなし。ビルドだけ） |
| `-DSND_CODEC_OPUS=OFF` | 警告ゼロ | 229 件中 229 件通過 |

- どの構成でも `platform_device_play_test` は SKIP（出力デバイスが無い）。ctest は通過に数える。
- **「全件通過」には、core の既知の欠陥の側で通っている記録用テスト 5 本を含む**（下の表）。
- 229 件の内訳: 元からある 211 件（うち `physics_test` 1 件は実装担当のスモークテストに置き換え）、実装担当のテスト 7 本の追加、独立テスト 11 本。
  physics のテストは 19 本。
- 繰り返し: physics のテスト 19 本すべてを、TSan と Release で `ctest --repeat until-fail:20 -j1` にかけ、失敗 0。
  スレッドを使うテスト（`IndepApi.WrongThread`、`IndepApi.ObserverReentry`、`IndepReview.DestroyOnAnotherThreadDoesNothing`、実装担当の別スレッドのテスト）はこの中に入る。
- MinGW: `physics/` のソースとテスト 22 ファイルすべてを、ビルドと同じ引数に `-fsyntax-only -Werror` をつけて
  `x86_64-w64-mingw32-g++ -std=c++20`（C のファイルは `x86_64-w64-mingw32-gcc -std=c11`）で通した。
- 独立テストの乱数での突き合わせ（参照モデル、種 6 通り × 300 フレーム）は、PLAY 記録の列と統計 12 項目が一致（独立テスト担当の報告）。

最初の受け入れ（`1a087ce`）の後に足したテスト:

- `physics_indep_final`（14 本）: 前の報告の 6 節で未確認とした箇所と、カバレッジで通っていなかった行。観測で受け取ったハンドルの音量の上書き、
  解除の確認が出力をビット単位で変えないこと、2000 フレームの停止、スティンガーと音楽の子の再生、資源の枯渇、コマンド領域の満杯、C++ ラッパー。
- `physics_indep_stall`（7 本）: D-125。
- カバレッジ（gcc Debug、gcov、独立テスト担当の計測。D-125 の前の `physics.cpp`）: 行 98.43% → 99.18%（1338 行中 1327 行）、分岐 92.23% → 94.30%。
  通っていない 11 行は、公開 API からは到達できない防御の行（その時点の行番号で 308、551〜552、816〜817、944、957、1450〜1453）。D-125 の後は測り直していない。

記録用テスト（正しい結果 (a) でも core の既知の欠陥 (b) でも通り、どちらだったかを出力に書く）の今の結果（Release、`8ba0318`）:

| テスト | 結果 | 依頼 |
| --- | --- | --- |
| `IndepPlay.CoreA004_EmitterCreatedAndDestroyedInOneUpdate` | (b) リスナーの位置で鳴る（L 0.3536 / R 0.3536） | A-004 1 節 |
| `IndepRecheck.CoreA004_NestedActionPlayAfterEmitterRelease` | (b) 子の音がリスナーの位置で鳴る | A-004 5 節 |
| `IndepFinal.CoreA004_StingerAfterEmitterRelease` | (b) スティンガーがリスナーの位置で鳴る | A-004 5 節 |
| `IndepFinal.CoreA004_MusicSecondSegmentAfterEmitterRelease` | (b) 音楽は Emitter を持ったままでも 1 つ目・2 つ目のセグメントともリスナーの位置で鳴る | A-004 5 節 |
| `IndepFinal.CoreA005_ResumeRefusedAfterRecordingWhileSuspended` | (b) `snd_resume` が 100 回試しても `COMMAND_BUFFER_FULL` | A-005 |
| `IndepStall.CoreA005_FootstepsAndPlayAtWhileSuspended` | (a) 再開できた（`api_failures` 3744） | A-005 |

core が直ったら、正しい結果だけを通す検査に戻すこと。

## 6. 未検証の箇所

- **実機・実デバイスでの確認はしていない。** テストはすべて `SND_OUTPUT_MANUAL` と手動で進める出力。`platform_device_play_test` はこの環境に出力デバイスが無く SKIP。
- **Windows / macOS では、ビルドも実行もしていない。** MinGW は構文検査（`-fsyntax-only`）だけ。
- **解除の観測の前提は core の実装の読みで、契約ではない。** 「`snd_get_playback_state` が NONE 以外なら、その再生を適用したブロックのスナップショットが見えている」「同じ値の音量を記録しても何も変わらない」は、
  確認の有無で出力がビット単位で同じことまでテストで確かめたが、core のスナップショットの作りが変わると崩れうる。
- **停止の検出（D-125）の穴。** `block_index` は `snd_update` のときにしか変わらない。1 回の `snd_update` のあいだに `snd_phys_submit_contacts` を `stall_calls`（既定 64）回以上呼ぶゲームでは、
  音声側が動いていても停止中と判定して接触を捨てる（`stall_calls` で調整できる）。実デバイスで、ブロック長とフレームの頻度が違うときの誤検出は試していない。
- `snd_phys_submit_contacts` のたびに `snd_get_stats` を呼ぶ負荷は測っていない。
- 音楽の Event を対応表に登録したときは、Emitter を持っていてもリスナーの位置で鳴った（上の表）。音楽の transition / bridge、`snd_set_music_state` と保持の関係は確かめていない。
- 停止中の足音と `snd_physics_play_at` は記録し続けるので、長い suspend のあいだ毎フレーム呼ぶとコマンド領域が満杯になり失敗する（`api_failures`）。A-005 が直るまで、再開できるかは core のコマンド領域の大きさに依る。
- 性能（接触の数に対する時間）は測っていない。作業領域は固定容量で、溢れは統計に数える。
- 既存テストの件数: 引き継ぎ書は 213 件とするが、`ff5df53` を数えると 211 件だった（211 + 実装担当 7 + 独立 11 = 229。実装担当の 8 本目 `physics_smoke_test` は元からある 1 件の置き換え）。差の 2 件の理由は調べていない。
- コミットの題と署名: `e991e1a`、`4f344ee`、`8ba0318` は題が「作業途中の控え（未検証）」のままだが、`8ba0318` の中身が上の受け入れを通した最終の状態。
  独立テスト担当の最初のコミット（`d68817c` など）は、署名の行が指定と違う名前になっている。どちらも履歴は書き換えていない。

## 7. ブランチの先頭

- 受け入れを行ったコード: `8ba0318`
- この報告を含む先頭: push 後の `int/s7-physics` の先頭（この文書と `CONTRACTS.md` の節だけを足したコミット）
