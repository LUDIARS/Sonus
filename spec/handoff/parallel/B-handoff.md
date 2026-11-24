# セッション B の引き継ぎ（本社の Codex へ）

- 書いた日: 2026-11-24。書いたのはセッション B の受け入れ側。
- 目的: セッション B（段階3 カバレッジ、`spec/handoff/parallel/B-coverage.md`）の残りを、本社の Codex が続けられるようにする。
- 先に読むもの: `spec/handoff/parallel/README.md`、`spec/handoff/parallel/B-coverage.md`、この文書、`spec/handoff/parallel/B/brief-common.md`。

## 1. いまの状態

| 組 | 範囲 | 状態 | ブランチ・先頭 |
| --- | --- | --- | --- |
| ② | 標準 DSP とボイスエフェクト（D-130〜D-135） | 実装・独立テスト・差し戻し 2 回まで済み。`int/s7-coverage` にマージ済み。受け入れの構成は一部が未完（下の 1.1） | `int/s7-coverage` |
| ④ | ボイス管理・シーケンス種別・通知・解析（D-140〜D-149） | 実装と差し戻し 1、独立テストの再確認 1 の途中まで。**落ちるテストが 2 件残る**。未マージ | `wip/b-g4`（c377047） |
| ① | 3D と距離カーブ（D-150〜D-159） | 未着手。実装担当への依頼文だけある（`B/brief-g1-impl.md`） | — |
| ③ | Parameter・スナップショット・ダッキング・カテゴリ（D-160〜D-169） | 未着手。依頼文も無い（5.2 に要件） | — |

- `int/s7-coverage` の出発点は 4eb2170（分類表 `spec/interface/acceptance/stage3-coverage.md`）。
- `wip/b-g4` は 4eb2170 から分かれている（組②は入っていない）。マージのときの衝突は 4.4。
- 「本社」「統合」の順（A → B → D → C）は README のまま。B は PR を出さず、ブランチを push するだけ。

### 1.1 組②の受け入れで確かめた構成（先頭 9e7b36e = マージした内容）

| 構成 | 受け入れ側が新しいビルドで回した結果 | 実装担当・テスト担当の結果 |
| --- | --- | --- |
| gcc Debug | 222 件すべて通過、警告 0 | 同じ |
| clang Debug | 222 件すべて通過、警告 0 | 同じ |
| ASan/UBSan | 222 件すべて通過 | 同じ |
| TSan | 222 件すべて通過（2 回）。足したテストの 20 回繰り返しは**未完**（途中で打ち切った） | 足したテストの 20 回繰り返しも通過 |
| Release | **未完**（ビルドの前で打ち切った） | 222 件通過、足したテストの 20 回繰り返しも通過 |
| `-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release` | **未実行** | 未実行 |
| `-DSND_CODEC_OPUS=OFF` | **未実行** | 未実行 |
| `-DSND_SIMD_SCALAR=ON` | **未完** | dsp と `indep_g2_*` が通過 |
| `-DCMAKE_CXX_FLAGS="-mavx2 -mfma"` | **未完** | 同上 |
| MinGW の構文検査 | 未実行 | 変えた・足したファイルが通過 |

**最初にやること**: `int/s7-coverage` の先頭で、上の「未完・未実行」の構成を回す。落ちたら組②の直しとして扱う。

性能（Release、128 実ボイス、1 ブロックの予算 5333 µs、b527335 で測定）: 標準 520.6 µs（9.8 %）／RT ワーカー 2 で 646.5 µs（12.1 %）。ボイスエフェクトの枠 4（既定）を使い切り 14.8 %／15.5 %、枠 16 で 22.3 %／24.2 %、枠 32 で 33.1 %／28.0 %（枠 32 は既定ではない。30 % を超えるのはこの設定の RT ワーカー 0 のときだけ）。

## 2. 進め方（1 組ぶん）

README §3 と `spec/handoff/README.md` 3 章のとおり。役割は 3 つで、**実装する者とテストを書く者は別**にする。

1. **実装**: 依頼文（`B/brief-g1-impl.md` の形）と `B/brief-common.md` を渡す。作業ブランチ `wip/b-g<組>` を `int/s7-coverage` から作る。フルセットで作り、自分のテスト（`core/test/core_g<組>_*_test.cpp`）まで入れる。
2. **独立テスト**: `B/brief-tester.md` を渡す。
   - 実装を読む前に、設計書と公開ヘッダと決定から「満たすべき性質」を書き出す。
   - 新しいファイル `core/test/indep_g<組>_*_test.cpp`（dsp は `dsp/test/`）だけを足す。実装と実装者のテストは変えない。
   - 欠陥は `Defect_` で始まる落ちるテストとして残し、自分では直さない。仕様に答えが無いものは `SpecHole_`（今の振る舞いを記録する通るテスト）にして、選択肢を報告する。
3. **受け入れ側の決定**: `Defect_` と `SpecHole_` の 1 件ずつに決定を下す（直す／今の振る舞いを仕様にする／持ち場の外への依頼にする）。決定には D 番号を振る。
4. **差し戻し**: 同じ実装担当が直す。決定と食い違うテストは、**書いた側**（テスト担当）が直す。
5. **再確認**: テスト担当が、直ったことを確かめ、`Defect_` を外して改名し、`SpecHole_` を決定どおりかを見る検査に直し、直しで変わった所を新しく突く。新しい欠陥が出たら 3 へ戻る。
6. **受け入れ**: `Defect_` が 0 件になったら、受け入れ側が**新しいビルドディレクトリ**で 3 章の構成を全部回し、直しの差分を自分で読む。通ったら `git merge --no-ff` で `int/s7-coverage` へ入れて push する。
7. 文書を更新する（6 章）。

守ること:

- **既存のテストのアサーションは書き換えない**（README §4）。食い違いは依頼にする（組④の「奪われた再生の終了理由」が実例。4.2）。
- 書いてよいのは `core/`、`dsp/`、`api/compose/c_api.cpp` の末尾、`contract/` への追加だけ。細かい範囲は `B/brief-common.md` §1。
- 新しいコマンド種別は足さない。予約欄の variant の割り当ては `B/brief-common.md` §2（使用済み: `SetBusVolume.reserved` 下位 8 ビット = 1、`SetSwitch.reserved[0]` = 1、`SetParam.reserved[0]` = 1、`Play.flags` ビット 16〜31）。
- 資源: ビルドする組は同時に 2 つまで。`-j2`。重いコマンド（cmake の configure / build、ctest）は 1 つずつ `flock /tmp/snd-heavy.lock <コマンド>` で包む。TSan の ctest は `-j1`。コンテナはメモリ不足で何度か落ちている。
- 決定論の検査は、設計書 2.5 の条件（負荷 LOD 無効 = `tuning.lod_down_blocks = 0`、遅れチャンクなし）で行う。

## 3. 受け入れテストの条件

### 3.1 組ごと

- 独立テストに `Defect_` が 0 件。`SpecHole_` は、決定を下して検査に直したか、「保証しない」と決めて記録のまま残したかのどちらか。
- 次の構成で、警告ゼロ（`-Werror`）・全件通過:
  1. gcc Debug
  2. clang Debug（`-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`）— gcc で出ない未使用変数の警告が出る。テスト担当にも回させる。
  3. `-DSND_SANITIZE=address,undefined`
  4. `-DSND_SANITIZE=thread`
  5. `-DCMAKE_BUILD_TYPE=Release`
  6. `-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release`
  7. `-DSND_CODEC_OPUS=OFF`
  8. dsp に触った組: `-DSND_SIMD_SCALAR=ON` と `-DCMAKE_CXX_FLAGS="-mavx2 -mfma"`
- 足した・変えたテストを、TSan（`-j1`）と Release で `ctest --repeat until-fail:20 -R <名前>`。
- 新しいファイルと変えたファイルを MinGW で構文検査（`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror`。スレッドを使うヘッダは `-posix` 版）。
- 新しい設定が全部既定値なら、既存のテストとゴールデンがそのまま通る（variant 0 は従来とビット単位で同じ）。
- 新しい経路を全部通る台本で、次の 3 つを確かめるテストがある:
  - 直列で 2 回回した結果と、RT ワーカー 2 で回した結果が、同じビット。
  - 初期化の後のメモリ確保が 0。
  - RT 安全性の検出が 0。
- 性能: Release で、新しい処理を載せた 128 実ボイスの負荷テストが 1 ブロックの予算の 30 % 未満。数値を報告に書く。ほかのビルドと重ならないとき（flock の中）に測る。

使っているスクリプトの形（構成を 1 つずつ、ビルドディレクトリは `/tmp/snd-<組>-<構成>`、終わったら消す）:

```
cmake -S <worktree> -B /tmp/snd-<組>-<構成> -G Ninja -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache [構成のオプション]
cmake --build /tmp/snd-<組>-<構成> -j2
ctest --test-dir /tmp/snd-<組>-<構成> -j2 --output-on-failure
```

### 3.2 セッションの終わり（B-coverage §4）

- 設計書 7 章の表の**全行**に機能テストが対応している。`stage3-coverage.md` に「行 → テスト名」を書き、対象外の行には理由を書く。
- 4 組を全部マージした先頭で、3.1 の構成を全部回す。
- 設計書 14 章: 実ボイス 128 本で 1 ブロックが予算の 30 % 未満、初期化後の確保 0、RT 安全性の検出 0。ベンチマークの数値を報告に書く。
- 決定論（2.5）が、新しく入れたシステムを全部載せた状態で成り立つ。
- `spec/handoff/parallel/report-B.md` を書いて push する（README §7 の項目）。

## 4. 組④の続き（`wip/b-g4`）

実装の報告は `B/report-g4-impl.md`（「差し戻し 1」まで）。テスト担当の報告ファイルは無い（書けなかった）。テストは `core/test/indep_g4_{seq,voice,analysis,system}_test.cpp` と `indep_g4_fixture.hpp`。

### 4.1 残っている落ちるテスト（c377047）

| テスト | 内容 | 受け入れ側の決定 |
| --- | --- | --- |
| `IndepG4Action.Defect_ActionOnlyEventEndsBeforeItsLaterActions` | クリップが無くアクションだけの Event は、始まってすぐ COMPLETED で終わり、後のアクションが動かない | **直す**: 最後のアクションが効くまで再生を続ける。Stop されたら残りは効かない |
| `IndepG4Action.Defect_ActionsStarveWhilePauseAndResumeAlternate` | 親の一時停止と再開を 1 ブロックおきに繰り返すと、再開ブロックに当たったアクションが次のブロックへ送られ、そこで次の一時停止に取り消される。繰り返しのあいだ 1 つも効かず、最後の再開の後にまとめて効く | **未決定**。テスト担当の期待は「5 つが順に、それぞれトラック時間から 1 ブロック以内に効く」。受け入れ側の勧めは直す（再開ブロックのアクションをそのブロックで適用する） |

再確認 1 は、gcc Debug までしか回っていない（TSan の繰り返し、Release、ASan、clang、MinGW は途中で止まった）。直しの後に全部回す。

### 4.2 本社への依頼になっている件（core は変えていない）

- **奪われた再生の終了理由**: 設計書 2.1 と `types.h` は、リミットグループで奪われた再生を `SND_END_STOLEN`／詳細 `LIMIT_GROUP` としている。既存の `CoreVoice.LimitGroupNeverRejectsAndOldestSteals`（`core/test/core_voice_test.cpp:149`）は `SND_END_COMPLETED` を期待している。既存のアサーションは書き換えない決まりなので、core は COMPLETED のままにし、独立テストは `IndepG4Steal.SpecHole_StolenPlaybackEndReason` で今の値を記録している。直すなら、core の 2 か所と、この既存テストの期待を同時に変える（差分は `B/report-g4-impl.md` の「差し戻し 1」にある）。本社が既存テストを直してよいなら、Codex がそのまま入れればよい。

### 4.3 組④の決定の案（受け入れたら CONTRACTS.md の「段階 7-B」へ書く）

| # | 決定 |
| --- | --- |
| D-140 | 仮想ボイスの戻り方は `EVNT.virtualReturn` で決める。「戻らない」は終了理由 STOLEN／詳細 VOICE_LIMIT。「先頭から」はボイスの開始フレームから。「続きから」は先読み分だけ先へ飛ばない。一時停止中のボイスは「戻らない」で終わらせず、再開のときに実ボイスになれなければ終わる |
| D-141 | 重み 0 以下のクリップは選ばない。全部 0 なら等確率。入れ子の子の乱数は「親の再生のシード + 子の Event id」から作る（トップレベルの列は変わらない） |
| D-142 | `CLIP.loopCount`: 0 = 無限、1 = 素材の既定、N ≥ 2 = ループ区間を N 周して N 周目のループ終端で終わる（テールなし）。`CLIP.fadeOutMs` は最後のフレームでサンプル単位の線形、最後で 0 |
| D-143 | レイヤーのクロスフェードは等パワー。保証するのは 2 枚の重なりだけ（3 枚以上は保証しない） |
| D-144 | 入れ子の Event: 子に Event 単位の上限と再トリガーは掛けない。リミットグループと優先度は子の Event のもの。1 ボイスが奪われても再生は続く |
| D-145 | アクション: Play／StopEvent はサンプル単位で正確。SetParam／ApplySnapshot は「その時刻以降に始まる最初のブロックの先頭から」（早くは効かない）。親の一時停止でトラック時間は止まる。親の停止後と、拒否された再生のアクションは効かない。予約付きの開始でも「親の開始 + 時刻」。子が上限を超えたら警告 LIMIT_REACHED（1 ブロックに 1 件まで）。アクションだけの Event は最後のアクションまで続く（4.1） |
| D-146 | `snd_play_fade`（フェードイン付きの開始、`Play.flags` のビット 16〜31 にミリ秒）: サンプル単位の線形。再生した時間で進む（一時停止で止まり、仮想のあいだは進む）。最大 65.535 秒 |
| D-147 | ラウドネスとスペクトルを測るのは、マスターと `kBusMeter` のバス（上限 8、バスの行の順）。測らないバスのラウドネスは NaN |
| D-148 | `snd_get_spectrum`（読み手のスレッドで FFT） |
| （付記） | 効果音のマーカー: `clock_sample` は出力に出る時刻（比 1 で厳密、それ以外 ±1 フレーム）。仮想のあいだに越えたマーカーも通知する。奪い方「最も優先度が低い」で新規がいちばん低ければ新規を拒否する |

受け入れる既知の制限: 音楽時間（NEXT_BAR など）の予約開始では、最初のブロックのアクションが最大 1 ブロック遅れる。

### 4.4 マージのときの衝突

`wip/b-g4` は組②を含まない。先に `int/s7-coverage` を `wip/b-g4` へマージして衝突を解く。

- `contract/internal/include/snd/internal/schema.hpp`: 両方が `R(Render, Voice_playbackSlot, Current)` を足している。1 行にまとめる。
- 末尾追記どうしの衝突（両方を残す）: `sonus.h`、`sonus.hpp`、`api/compose/c_api.cpp`、`commands.hpp`、`core/CMakeLists.txt`、`core/test/CMakeLists.txt`、`core_impl.hpp`、`create.cpp`、`sys_render.cpp`、`sys_apply.cpp`、`bus_graph.cpp`、`recorder.cpp`、`core.hpp`。

### 4.5 組④の、持ち場の外への依頼の材料（依頼ファイルはまだ書いていない）

- livelink（C）: `Play`／`PlayOneShot` の `flags` のビット 16〜31 を規則表で通す。
- bank／buildtool: 効果音のマーカーの列（`ASST.markerFirst/Count` と表）、`ACTN` のスコープとフェード時間の列、`TRAK.flags` の意味、`CLIP.loopCount` の文書、レイヤーが 3 枚重なるときの警告。
- source（D）: 開始の要求にループ回数の欄、描画の結果にエラーの印（core には `SND_END_ERROR` を出す経路が無い）。
- api: `snd_play_fade` と `snd_get_spectrum` の api 層のテスト。

## 5. 残りの組の実装の条件

### 5.1 組①（3D と距離カーブ）

依頼文 `B/brief-g1-impl.md` をそのまま使える。作るもの: ドップラー、距離カーブ（音量・LP・spread・3D ブレンド・リバーブ送り）、コーン、複数リスナー（最寄り・重み付き・画面分割）、遮蔽・閉塞、範囲音源（線・面・箱）、リバーブゾーン、ボイスからの補助送り。ルーム／ポータルは作らない（段階6、セッション D）。

### 5.2 組③（Parameter・スナップショット・ダッキング・カテゴリ）

依頼文はまだ無い。`stage3-coverage.md` の §2 ③ と §3 から、作るものは次のとおり。組②の「寄与」の口（`core/src/fx_control.cpp`、D-130）を使う。

- **Parameter**: `CURV.compose` を読む。4.3 の「その他は乗算」。`PARM.scopeMask` を強制する（設定外のスコープの値をどう扱うかを決める）。3 点以上のカーブと shape のテスト。`Send`／`EffectParam` を宛先にするカーブは、宛先の列が無いので依頼にする（`requests/B-2.md` に書いてある）。
- **スナップショット**: `SNPV.targetKind` 1（エフェクトのパラメータ）と 2（センドの量）を効かせる。2 つ以上の重ねがけ。アクションの `ApplySnapshot` に時間を付ける。
- **ダッキング**: サイドチェーン型を、入力レベルに比例する形にする（今はしきい値を超えたら一定量）。攻撃・解放の時間のテスト。
- **カテゴリ**: ソロ（公開関数を足す。コマンドは `SetCategoryVolume.reserved` = 1）。フォーカスを失ったときのカテゴリ消音（`kCategoryMuteOnFocusLoss`。api からフォーカスの状態を渡す経路は依頼）。`kEventIgnorePause` のテスト。
- variant の割り当て: `SetParam.reserved[0]` 4〜5、`SetSwitch.reserved[0]` 4〜5、`SetBusVolume.reserved` 下位 8 ビット = 2、`ApplySnapshot.reserved`、`SetCategoryVolume.reserved` = 1。

### 5.3 作らないと決めたもの（報告に書く）

- バスグラフの独立した枝の、RT ワーカーでの並列化（設計書 4.6）。振る舞いは変わらず、負荷は予算の 10〜15 % なので作っていない。

## 6. 文書の状態

| 文書 | 状態 |
| --- | --- |
| `spec/interface/CONTRACTS.md` の「段階 7-B」 | 組②（D-130〜D-135）だけ書いた。組④は 4.3 の案を、受け入れた後に移す |
| `spec/handoff/parallel/requests/B-1.md`（livelink）、`B-2.md`（bank／buildtool）、`B-3.md`（api）、`B-5.md`（契約の構造体と ext.h） | 組②の分だけ。組④の分（4.5）と、組①③の分を足す。`B-4.md`（source）は未作成 |
| `spec/interface/acceptance/stage3-coverage.md` の「行 → テスト名」 | 出発点のまま。組ごとに足したテストを追記する（組②のテストは `core/test/core_fx*_test.cpp`、`indep_g2_*_test.cpp`、`dsp/test/effects_coverage_test.cpp`、`indep_g2_dsp_test.cpp`） |
| `spec/handoff/parallel/report-B.md` | 未作成 |
| `spec/handoff/parallel/B/` | 実装担当・テスト担当への決まり（`brief-*.md`）と、担当の報告（`report-g2-impl.md`、`report-g2-test.md`、`report-g4-impl.md`） |

## 7. 確かめられていないこと

- 組②: 1.1 の未完・未実行の構成。api 層（公開関数 3 つの引数の検査と一巡）。キーバスのバンクだけを外した場合。遅れチャンクがあるときの直列と並列の一致（2.5 の条件の外）。
- 組④: 4.1 のとおり。加えて、ピッチがカーブで動くときの N 回ループの切り位置、`Play.flags` のビット 2〜15、api 層。
- `jobs_rt_claim_steal_test` が Release の全件で 1 回だけ落ちた（組②の実装担当の実行）。作り直して 20 回繰り返しても再現せず、ほかの実行でも出ていない。`jobs/` は B の持ち場の外。

## 8. できていないことの一覧（2026-11-24 に作業を止めた時点）

受け入れ側は、ここで作業を止めた。裏で回していた組②の受け入れも打ち切った。B-coverage §4 の完了条件は、どれも満たしていない。

### 8.1 組②（マージ済みだが、受け入れが終わっていない）

- 受け入れ側が回していない構成: Release の全件、`-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release`、`-DSND_CODEC_OPUS=OFF`、`-DSND_SIMD_SCALAR=ON`、`-DCMAKE_CXX_FLAGS="-mavx2 -mfma"`、MinGW の構文検査。
- 受け入れ側が回していない繰り返し: 足したテストの 20 回繰り返し（TSan `-j1` と Release）。TSan は途中で打ち切り、結果は無い。
- `-DSND_BUILD_TESTS=OFF` と `-DSND_CODEC_OPUS=OFF` は、担当も受け入れ側も、誰も回していない。
- 性能の数値は実装担当が b527335 で測ったもの。受け入れ側は測り直していない。マージした先頭（9e7b36e の内容）でも測っていない。
- 直しの差分のうち、受け入れ側が自分で読んだのは Echo／Delay の頭打ち（`dsp/src/fx_delay.cpp`）と、未使用の定数の削除だけ。
- `stage3-coverage.md` の「行 → テスト名」に、組②のテストを書いていない。
- api 層のテスト、livelink の規則表、bank／buildtool の列は依頼（`requests/B-1.md`、`B-2.md`、`B-3.md`、`B-5.md`）を書いただけで、相手の持ち場には何も入っていない。

### 8.2 組④（`wip/b-g4`、未マージ）

- 落ちるテスト 2 件を直していない（4.1）。うち 1 件は決定も下していない。
- 「奪われた再生の終了理由」を直していない（4.2）。本社の判断待ち。
- `SpecHole_` 4 件に決定を下していない: `SpecHole_ThreeOverlappingLayers`、`SpecHole_TrackTimeOfALoopingOrPitchedParent`、`SpecHole_StolenPlaybackEndReason`、`SpecHole_FadeInOfARestartedVoice`。
- テスト担当の再確認 1 が終わっていない（gcc Debug まで）。テスト担当の報告ファイルも無い。
- 受け入れ側は、組④を新しいビルドで 1 構成も回していない。直しの差分も読んでいない。性能も測っていない。
- `int/s7-coverage` の取り込みと衝突の解消をしていない（4.4）。
- 決定 D-140〜D-148 は案のまま。`CONTRACTS.md` に書いていない（4.3）。
- 持ち場の外への依頼を書いていない（4.5）。`requests/B-4.md`（source）は作っていない。

### 8.3 組①（3D と距離カーブ）

- 何も作っていない。ブランチ `wip/b-g1` も無い。有るのは実装担当への依頼文（`B/brief-g1-impl.md`）だけ。
- 依頼文の中の「`scratchpad/`」のパスと「push しない」は、前の進め方のままになっている。`spec/handoff/parallel/B/` と、`B-codex.md` の push の決まりに読み替える。
- 依頼文は、組④が後からマージされる前提で書いた。組④を先に入れるなら、その注意書きは要らない。

### 8.4 組③（Parameter・スナップショット・ダッキング・カテゴリ）

- 何も作っていない。依頼文（`B/brief-g3-impl.md`）も書いていない。有るのは 5.2 の要件の箇条書きだけ。
- 5.2 は分類表から拾ったもので、設計書 4.3 と突き合わせて漏れを確かめてはいない。

### 8.5 セッション全体

- `spec/handoff/parallel/report-B.md` を書いていない。
- `spec/interface/acceptance/stage3-coverage.md` の「行 → テスト名」を埋めていない（出発点のまま）。
- 3.2 の条件（4 組をマージした先頭での全構成、決定論、確保 0、RT 違反 0、性能）は、何も確かめていない。
- バスグラフの枝の並列化（5.3）は作っていない。
- 独立テストの担当には、clang のビルドをさせていなかった（組②で clang だけの警告が受け入れのときに出た）。`B/brief-tester.md` にこの注意を足していない。

## 9. 受け入れ再開後の引き継ぎ（2026-11-24）

§8 は前担当の作業終了時点の記録。再開後の構成ごとの実測は `report-B.md` に記録する。

- 本社はリミットグループに最後のボイスを奪われた Playback の STOLEN / LIMIT_GROUP を承認した。§4.2 の 1 箇所と B-6 の追加 7 箇所が対象。独立担当は 157、158 行もグループ奪取であることを通知で確認した。実装と検査をまとめたローカルコミットは `3814ea0`。組④の作業先は `a8a40fd`。
- 組②の元の先頭 `0ffb97a` で Release 全件、関連 13 実行ファイル各 20 回、テストなし Release と Opus なしを実行した。容量と MinGW の差し戻し後のコードは `17e0eb8`。受け入れ側の新規 gcc Debug では 222 成功・実デバイス 1 スキップ・失敗 0、MinGW POSIX は 37/37。
- 組①と③には、実装担当と独立担当の試験および報告がある。4 組の組み合わせを専用の作業ツリーで先に再現し、フィルタ終端とアクションによるスナップショットを検査した。正式な受け入れは組②修正、④、①、③の順で行う。
- 性能が失敗したときは同じ実行ファイルを排他で 5 回計測し、中央値を 30% と比較する。タイミング起因の失敗は 20 回、環境起因はコマンドと失敗内容を 3 回分記録する。LeakSanitizer はこの環境のプロセス監視下で起動できないため、ASan/UBSan に detect_leaks=0 を指定する。
- 認証または自動審査のため、作業先頭と GitHub に反映できた先頭が異なる箇所は報告書のハッシュを参照する。

- §7 の可変ピッチ有限ループと Play.flags の予約ビットは、組④の独立担当が `7d70613` で検査し、結果を `57e68c1` に記録した。共有キーバスは統合試作 `0d993be` で定義元バンクの解放・生存対象・キー再駆動を音で検証した。
- 組②既マージ版は scalar・AVX2/FMA 各222登録、失敗0。組②修正版は Release 全223登録、失敗0、変更対象14ターゲット×20回成功。組④は8バスの解析付き128声で RT ワーカー2の5回中央値33.2%となり差し戻した。再計測の値は report-B.md に分けて記す。


## 10. 作業環境の切断時点から再開する手順

受け入れの途中で Linux 作業環境が利用不能になった。修正を正式にマージした組は0。受け入れ報告・本社への依頼と組②修正のソースは GitHub から取得できる。組①・③・④の最新ソースと4組の横断試作は、次のローカル記録まで進んでいる。

| 対象 | ローカル | GitHubで確かめた作業ブランチ先頭 |
| --- | --- | --- |
| 組② | `77c6ccbf35c4dd49375453ec1a90c331fcecb587`。受け入れ用ツリーの `cf6cb2b` と同じツリー | `ab5259512d7602daef00b4d2aea80f65b95e542a`。コード・テストは受け入れの対象と同じ |
| 組④ | `d93d9318e546acd3b9ddf99a3d36f8bedfdc835e`。本社承認の期待値を実装とまとめた `3814ea0`、可変ピッチと予約ビット試験 `7d70613`、Sinc 修正 `481c997` を含む | `50555f390423992a9c58a3dbb518c4f8b75d7222` |
| 組① | `94bd6b9`。音の停止フレームを直達・送り両方に適用した `d6d2e80` を含む | `7aea75c88f6bab3f085e856c74c277fde1f423e4` |
| 組③ | `0e5f2e74cf1f00b4c8e6cf152f07d4e9f30dc4ee`。共有バスのFX・送りと旧形式Panの加算順を含む | `700b4fd7c34d7598fea7c24be31ae7cc30dbc157` |
| 4組の試作 | `fc728ce268b9c20f56fd2e4305a19db6efbdafa8`。追加の共有キーバンク試験 `0d993be` と上記の組④を含む | 正式な統合ブランチには未マージ |

元の作業環境では、ルート `/workspace/scratch/34000a45b93b` の `sonus-session-B.bundle` に4作業ブランチ、ローカル int、試作のHEADを収録した。基点は `4eb2170af8f1631aecf79736c2589cf48fdf8a90`。試作 `fc728ce` を収録後に `git bundle verify` が成功している。環境が復元されたら、最初に `git bundle list-heads` と `git bundle verify` で内容を確かめる。元環境が使えない場合は、同じ基点を含む新規クローンに取得したbundleを読み込む。公開側だけにある報告コミットは取り込んでから続ける。

- 組②の修正版の残りは Opusなし全件、Clang、ASan/UBSan、TSan全件と反復、scalar、AVX2/FMA。旧版TSanの変更対象20回も最終サマリーを確認する。`b-toolchain/logs/{g2old,g2fix}` は受け入れ担当のログ。終了記録のない構成は新規に実行する。
- 組④はSinc修正で `dsp/` も変わったので9構成が要る。実装担当は8バス解析・128声のワーカー2で、修正前の5回中央値33.2%から修正後20.7%を報告した。受け入れ担当の旧新比較は `b-toolchain/logs/g4-owner-{kernel,load}`。切断時点では再測定結果を確認できなかった。
- 組①で組④の停止時刻を取り込む際は、試作 `b196bec` の処理と `5eef4bd`・`fc5befc` の独立試験を参照する。組③は試作 `a38fc49` の受理前後のスナップショット遷移、`fb29c3b` のアクション試験、`2f264fb` の全組横断試験、`0d993be` の共有キー解放試験を保つ。意味のある衝突は各実装担当に戻す。
- 作業先に残した `/tmp/snd-b-contracts-g{4,1,3}.md` の決定を各組の受け入れ後に CONTRACTS.md「段階 7-B」へ反映する。分類表§8はローカル int の `590b19b` にあり、試作に存在するテスト59件が設計§7の28行に対応した。受け入れた組と対象外の理由を確定する。
- 委任・ビルド・push の制限は変わらない。作成済みソースを転送する操作などが自動審査に拒まれ、通常のgit pushもHTTPS認証で失敗した。未公開ソースが新環境から取得できない場合は、公開済みの先頭とこの引き継ぎを使い、実装・独立テストを別の担当が復元する。
