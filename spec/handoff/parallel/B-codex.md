# Codex への指示書（セッション B の続き）

LUDIARS/sonus の `int/s7-coverage` ブランチで、セッション B（段階3 カバレッジ）の残りを仕上げてください。

## 最初に読むもの（この順に全部）

1. `spec/handoff/parallel/README.md`（4 セッション共通の決まり）
2. `spec/handoff/parallel/B-coverage.md`（セッション B の範囲と完了条件）
3. `spec/handoff/parallel/B-handoff.md`（いまの状態、進め方、受け入れテストの条件、残りの作業）。できていないことの一覧は 8 章
4. `spec/handoff/parallel/B/brief-common.md`（書いてよい場所、variant の割り当て、ビルドの制限）

食い違いがあれば、README と B-coverage が正です。

## 役割の分け方

- 1 つの組につき、**実装するセッション**と**独立テストを書くセッション**を必ず分ける。同じセッションに両方をさせない。
- 受け入れ（決定を下す、差し戻す、全構成を回す、マージする）は、さらに別のセッションが行う。受け入れのセッションは、実装もテストも書かない。
- 実装のセッションには `B/brief-common.md` と組の依頼文を、テストのセッションには `B/brief-common.md` と `B/brief-tester.md` を渡す。
- テストのセッションは、実装を読む前に、仕様と公開ヘッダから「満たすべき性質」を書き出す。欠陥は `Defect_` で始まる落ちるテストとして残し、自分では直さない。

## やること（この順）

1. **組②の受け入れの残り**: `int/s7-coverage` の先頭で、`B-handoff.md` 1.1 の「未完・未実行」の構成を回す。落ちたら組②の直しとして、2 章の流れで扱う。
2. **組④を仕上げる**（`wip/b-g4`）:
   - `B-handoff.md` 4.1 の落ちるテスト 2 件を直す。2 件目は決定がまだなので、先に決定を下す（勧めは「直す」）。
   - 4.2 の「奪われた再生の終了理由」は、既存テスト `core/test/core_voice_test.cpp:149` の期待を直してよいかを本社が決める。直すなら core の変更と同じコミットで入れる。直さないなら今のまま（独立テストは今の値を記録している）。
   - テストのセッションに再確認をさせ、`Defect_` が 0 件になったら、4.4 の手順で `int/s7-coverage` を取り込んで衝突を解き、受け入れてマージする。
   - 4.3 の決定を `spec/interface/CONTRACTS.md` の「段階 7-B」へ書き、4.5 の依頼を `requests/B-*.md` に足す。
3. **組①**（3D と距離カーブ）: 依頼文は `B/brief-g1-impl.md`。ブランチ `wip/b-g1` を `int/s7-coverage` から作る。
4. **組③**（Parameter・スナップショット・ダッキング・カテゴリ）: 作るものは `B-handoff.md` 5.2。先に依頼文を `B/brief-g3-impl.md` として書いてから始める。ブランチは `wip/b-g3`。
   - 組①と組③は並べて進めてよい（ビルドする組は同時に 2 つまで）。
5. **仕上げ**: 4 組を全部マージした先頭で、`B-handoff.md` 3.2 を満たす。`spec/interface/acceptance/stage3-coverage.md` の「行 → テスト名」を埋め、`spec/handoff/parallel/report-B.md` を書く。

## 受け入れの条件（要点。全文は `B-handoff.md` 3 章）

- 独立テストに `Defect_` が 0 件。
- gcc Debug、clang Debug、ASan/UBSan、TSan、Release、`-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release`、`-DSND_CODEC_OPUS=OFF` の 7 構成で、警告ゼロ・全件通過。dsp に触った組は `-DSND_SIMD_SCALAR=ON` と `-mavx2 -mfma` も。
- 足した・変えたテストを、TSan（`-j1`）と Release で 20 回繰り返して通す。MinGW の構文検査も通す。
- 直列 2 回と RT ワーカー 2 でビットが一致する。初期化後の確保が 0。RT 安全性の検出が 0。
- Release で、新しい処理を載せた 128 実ボイスの 1 ブロックが予算の 30 % 未満。
- 受け入れのセッションは、担当の報告をうのみにせず、新しいビルドディレクトリで自分で回し、直しの差分を自分で読む。

## 守ること

- **push してよいのは `int/s7-coverage` と、作業ブランチ `wip/b-g<組>` だけ**。`main` と `int/s2-integration` には push しない。ほかのセッションのブランチはマージしない。PR は出さない（統合は本社が A → B → D → C の順で行う）。
- **既存のテストのアサーションは書き換えない**（`Defect_*` も含む）。決定と食い違うテストは、書いた側に直させる。既存テストと設計が食い違うときは、依頼（`requests/B-*.md`）にする。
- 書いてよいのは `core/`、`dsp/`、`api/compose/c_api.cpp` の末尾、`contract/` への追加だけ。持ち場の外の変更は依頼にする。
- 新しいコマンド種別は足さない。契約の版は上げない。凍結ファイル（`tests/framework/`、`tests/mocks/`、ルートの `CMakeLists.txt`、`cmake/`）に触らない。
- 決定は `CONTRACTS.md` の「段階 7-B」にだけ書く。番号は組④ D-140〜149、組① D-150〜159、組③ D-160〜169。
- ビルドは `-j2`。重いコマンドは 1 つずつ `flock /tmp/snd-heavy.lock <コマンド>` で包む。TSan の ctest は `-j1`。ビルドディレクトリは `/tmp/snd-<組>-<構成>` に作り、終わったら消す。
- コミットのメッセージは日本語。区切りごとにコミットして push する（コンテナはいつ落ちてもおかしくない）。
- できなかったこと、確かめられなかったことは、隠さず報告に書く。

## 終わりの報告

`spec/handoff/parallel/report-B.md` に、README §7 の項目（作ったもの、D-番号ごとの決定、契約の変更点、他の持ち場への依頼、構成ごとの結果、未検証の箇所、ブランチの先頭のハッシュ）を書いて push する。
