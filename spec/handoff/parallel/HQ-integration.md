# 本社: 4 セッションの完了を待ち、統合して main に入れるまで

あなたは本社の統合担当で、受け入れの最終判断をする。4 つのセッション（A 物理、B カバレッジ、C ツール、D 拡張）が並列に作業している。全部が終わるのを待ち、統合し、実機で確かめ、`main` に入れるところまでを受け持つ。

## 0. 最初に読むもの

- `spec/handoff/parallel/README.md`: 4 セッションの共通の決まり。持ち場、決定番号、ブランチ、完了報告の形が書いてある。
- `spec/handoff/parallel/{A-physics,B-coverage,C-tool,D-extensions}.md`: 各セッションの指示書。
- `spec/handoff/hq-handover.md`: これまでの状態、本社でやること、未検証の項目。
- `spec/handoff/README.md` の 3 章: 手順、ビルド構成、決まりごと。
- `spec/interface/CONTRACTS.md`: 契約 1.2、決定 D-1〜D-104。

## 1. 待つ

| セッション | ブランチ | 完了の印 |
| --- | --- | --- |
| A | `int/s7-physics` | `spec/handoff/parallel/report-A.md` がある |
| B | `int/s7-coverage` | `spec/handoff/parallel/report-B.md` がある |
| C | `int/s7-tool` | `spec/handoff/parallel/report-C.md` がある |
| D | `int/s7-ext` | `spec/handoff/parallel/report-D.md` がある |

- 次のコマンドで、どのセッションが終わったかを確かめる。
  ```
  git fetch origin 'int/s7-*'
  for b in physics coverage tool ext; do git ls-tree -r --name-only origin/int/s7-$b spec/handoff/parallel | grep report- ; done
  ```
- 確かめるのは 1 時間に 1 回くらいにする。スケジュールの機能（send_later / loop など）があれば使い、無駄に何度も見に行かない。
- **終わったセッションから、順に受け入れの確認を始めてよい**（2 章）。マージの順番は 3 章のとおりに守る。
- 報告に「判断待ち」「ユーザーの判断が要る」とあれば、まとめてユーザーに聞く。ユーザーの決定を、セッションの代わりに勝手に下さない。受け入れ側として決めてよいのは、仕様の穴のうち技術的なものだけ。

## 2. セッションごとの受け入れ確認

報告が出たブランチから、1 つずつ確認する。

1. **報告を読む**: 作ったもの、決定（D-番号）、契約の変更点、他の持ち場への依頼、未検証の箇所。
2. **持ち場の確認**: `git diff --stat origin/int/s2-integration...origin/int/s7-<x>` を見て、持ち場の外に変更がないか確かめる。
   - 許されている例外は 2 つ: `api/compose/c_api.cpp` の末尾への追記と、`contract/` への追加。
   - 違反があれば、セッションに差し戻すか、統合のときに扱いを決める。
3. **凍結ファイル**: `tests/framework/`、`tests/mocks/`、ルートの `CMakeLists.txt`、`cmake/` に変更がないことを確かめる。
4. **既存テストのアサーション**: 書き換えられていないことを確かめる。`git diff` で既存のテストファイルの `SND_CHECK` の行の変化を見る。
5. **新しいビルドディレクトリから回す**: そのブランチ単体で、共通の決まり 5 章の完了条件を回す。
   - 7 構成
   - dsp や tool に触った場合は、その追加の構成も
   - 並行性のテストの 20 回の繰り返し
6. **要所のコードを読む**: RT 経路の確保とロック、並行部分、契約の追加の形。
7. 問題があれば、そのセッションに差し戻す。報告のブランチに依頼を書くか、ユーザーに伝えてもらう。

## 3. 統合（`int/s2-integration` に、A → B → D → C の順）

順番の理由:
- A（物理）は公開 API だけを使うので、ほかとぶつからない。
- B（core / dsp）は土台になる。
- D は dsp の登録表で B と重なるので、B のあとに入れる。
- C（ツール）は、ランタイムの機能が揃ってからつなぐ。

各ブランチについて、次の手順で入れる。

1. `git merge --no-ff origin/int/s7-<x>`
2. 衝突を解く。よく起きる場所は次のとおり。
   - **CONTRACTS.md**: 各セッションは自分の節（`## 段階 7-A` など）に書いている。節はそのまま残し、§7 の変更履歴に 1 行ずつ移す。
   - **公開ヘッダの列挙・関数**:
     - 末尾に足した値どうしがぶつかったら、統合の順に番号を振り直す。
     - 振り直した値を使うテストと実装も合わせる。
     - 合わせたことは、統合の決定として記録する（D-220 から）。
   - **`snd_config` の末尾のフィールド**: 統合の順に並べる。`struct_size` で受け付ける大きさの一覧（D-91）に、統合の前の大きさを足す。1.2 の大きさは全部受け付けたままにする。
   - **スキーマの列**: 同じ名前・同じ列が重なっていないか確かめる。
   - **dsp の登録表と CMake のソース一覧**: B と D の追記を両方残す。
   - **`api/compose/c_api.cpp` の末尾**: 各セッションの追記を両方残す。
3. マージの直後に、少なくとも gcc Debug と Release で全件通ることを確かめる。

4 つ全部を入れ終えたら、次を行う。

4. **版を上げる**: `SND_CONTRACT_VERSION_MINOR` を 3 にする（契約 1.3）。
   - CONTRACTS.md の冒頭と §7 に書く。
   - ライブリンクのプロトコルとバンク形式の版も、各セッションが上げていれば、それぞれの md の互換性の節と合っているか確かめる。
5. **依頼を片付ける**: `spec/handoff/parallel/requests/*.md` を全部読む。
   - 持ち場をまたぐ変更を、統合担当が行う。たとえば次のようなもの。
     - マテリアル表やパッチのバンク形式
     - buildtool
     - タイムストレッチをボイスにつなぐ配線
     - platform の入力デバイス
   - 大きいものは、別の組として Opus の実装担当と独立テスト担当に任せる。手順は `spec/handoff/README.md` の 3 章のとおり。
   - D のルーム／ポータルの設計メモ（`requests/D-room-portal.md`）は、B が統合された上で、別の組として着手するかをユーザーに聞く。
6. **C の画面とランタイムをつなぐ**: C の報告に「ランタイムに無いので、モデルと形式までで止めた」と書いてある画面を、A・B・D の機能とつなぐ。

## 4. 統合後の確認（本社の環境で）

- **全構成**: 共通の決まり 5 章の全部を回す。
  - 7 構成
  - SIMD の 2 構成
  - `SND_BUILD_TOOL_CORE=ON`
  - `SND_BUILD_TOOL=ON`
  - MinGW の構文チェック
- **負荷をかけた再確認**（`hq-handover.md` 2.2）: CPU を埋めた状態で、並行性のテストを TSan と Release で `--repeat until-fail:50` で回す。
  - TSan で `base_adv_queue_test` が 1 回だけ時間切れになったことがあり、原因はまだ調べていない。
- **実機**（`hq-handover.md` 2.3）:
  - Windows（WASAPI、D-103 の Win32 ファイルシステム、ツールの窓）
  - macOS / iOS（Core Audio、D-103 の O_PATH が無い経路、ツールの窓）。**Mac では一度もビルドしていない**ので、最初のコンパイルエラーは直しながら進める。
  - Linux の ALSA の実デバイス（`platform_device_play_test`）
- **性能**（設計書 14 章）:
  - 実ボイス 128 本で、1 ブロックの処理時間が予算の 30% 未満
  - 初期化後の確保がゼロ
  - RT 安全性の検出がゼロ
  - B と D が報告したベンチマークを、本社の機械で測り直す。
- **機能カバレッジ**: B の `spec/interface/acceptance/stage3-coverage.md` で、設計書 7 章の全行にテストが対応していることを確かめる。
- 実機で見つかった欠陥は、`Defect_` のテストを残して直す（実装担当と独立テスト担当に任せる）。

## 5. main に入れる

- 統合の記録を `spec/interface/acceptance/stage3-6.md` に書く。中身は次のとおり。
  - 受け入れの結果
  - 実機の結果
  - 持ち越し事項
- `spec/handoff/` を消す。引き継ぎのための一時置き場なので、必要な記録は先に `spec/interface/acceptance/` に移しておく。
- `int/s2-integration` から `main` への PR を作る。
  - 説明に書くこと: マージした順と各ブランチ、契約 1.3 の要点、全構成と実機の結果、未検証の項目。
- **マージはユーザーの承認を得てから行う。**

## 6. Ergo との連携

- LUDIARS/Ergo#99（ergo_audio の Sonus バックエンドと、Studio の sonus プラグイン）が `main` に入っているかを確かめる。
- Sonus の `main` に統合が入ったら、Ergo の `ERGO_SONUS_DIR` でそれを指してビルドし、Ergo のテストを回す。
  - 回す構成: 既定、`ERGO_AUDIO_BACKEND=sonus`、`ERGO_WITH_SONUS=ON`
- C の報告にある「Ergo Studio の sonus プラグインに足せる画面」と、ergo_audio にバンクとイベントの API を足す件（今はマスターバスにしか鳴らない）は、Ergo の別の PR として扱う。着手するかはユーザーに聞く。

## 7. ユーザーに聞くこと（まとめて 1 回で）

- 各セッションの報告に出た「判断待ち」
- ルーム／ポータルの組に着手するか
- 運用ファイル（CLAUDE.md、`spec/domains`、Excubitor のカタログ、CI）を作るか
- 設計書 15.2 の未決事項: 空間オーディオ、機種ごとの予算、移行ツール
- MSVC への対応の要否（今は gcc / clang / MinGW だけ）
- `main` へのマージの承認

## 8. 資源と記録

- **資源**: ビルドを走らせるエージェントは、同時に 1〜2 体まで。ビルドは `-j2`、ctest は `-j2`。本社の機械に余裕があれば、ここは緩めてよい。
- **コミット**: 区切りごとにコミットし、`int/s2-integration` に push する。統合担当は `int/s2-integration` に push してよい。
- **進み具合の報告**: 節目（各セッションの受け入れ、各マージ、実機確認、`main` への PR）ごとに、短くユーザーに報告する。
