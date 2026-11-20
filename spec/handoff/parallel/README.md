# 残作業の並列実行（4 セッション共通の決まり）

4 つのセッションで、残りの段階を並列に進める。セッションは別のアカウントで動かす。各セッションは、自分の指示書と、この共通の決まりを全部読んでから始める。

| セッション | 指示書 | 範囲 | 持ち場（書いてよいディレクトリ） | 決定番号 |
| --- | --- | --- | --- | --- |
| A | `A-physics.md` | 段階4 physics | `physics/` | D-110〜D-129 |
| B | `B-coverage.md` | 段階3 カバレッジ | `core/`、`dsp/` | D-130〜D-169 |
| C | `C-tool.md` | 段階5 ツールの完成 | `tool/`、`livelink/`、`buildtool/` | D-170〜D-189 |
| D | `D-extensions.md` | 段階6 拡張（ソースとタイムストレッチ） | `source/`、`dsp/` の新しいファイルだけ | D-190〜D-219 |

## 1. 出発点とブランチ

- リポジトリ: `LUDIARS/sonus`。出発点は `int/s2-integration` の 40314d9（またはそれ以降の先頭）。
- 各セッションは、自分のブランチを切ってそこにだけ push する。
  - A: `int/s7-physics`、B: `int/s7-coverage`、C: `int/s7-tool`、D: `int/s7-ext`
- `main` と `int/s2-integration` には push しない。統合は本社（または統合担当）が、A → B → D → C の順でまとめて行う。
- 他のセッションのブランチを、自分のブランチにマージしない。土台が要るときは、統合されたあとの `int/s2-integration` を取り込む。

## 2. 最初に読むもの

- `spec/handoff/README.md` の 3 章: 役割と手順、ビルド構成、決まりごと。ここに書いた手順で進める。
- `spec/handoff/hq-handover.md`: 今の状態、持ち越し、未検証の項目。
- `spec/architecture/design.md`: 担当の章と、12 章（モジュール分割）、14 章（テスト）、15 章（段階）。
- `spec/interface/CONTRACTS.md`: 契約 1.2 と、D-1〜D-104 の決定。

## 3. 進め方（各セッションが受け入れ側になる）

- セッション本人は**受け入れ側**になる。実装は Opus のエージェント、独立テストは実装者とは別の Opus のエージェントに任せる。
- 1 つの組の流れは `spec/handoff/README.md` 3 章のとおり。
  1. 実装する。
  2. 独立テストを付ける。
  3. 欠陥と仕様の穴に決定を下して、差し戻す。
  4. 再確認する。
  5. 全構成で受け入れる。
- 独立テストの担当は、実装を読む前に、仕様と公開ヘッダから満たすべき性質を書き出す。欠陥は `Defect_` のテストとして残し、自分では直さない。
- **フルセットで実装する**。最小の縦切りで止めず、設計書の該当する章の機能を全部、配線とテストまで入れる。
- 区切りごとにコミットして push する。コンテナはいつ落ちてもおかしくない前提で進める。

## 4. 変更してよいもの・いけないもの

- **持ち場の外は書かない**。持ち場の外に変更が要るときは、変更せずに、`spec/handoff/parallel/requests/<セッション>-<番号>.md` に依頼を書き、報告にも入れる。
  - 例外は `api/` だけ。新しい公開関数の配線は、自分のブランチで `api/compose/c_api.cpp` の末尾に足してよい。ほかの行は変えない。
- **契約（`contract/`）**: 追加（minor）だけ許す。
  - 公開ヘッダ: 新しい関数と、列挙の末尾への値の追加。構造体は、`struct_size` で守られる `snd_config` の末尾にだけ足す。
  - 内部の契約: 新しいコマンド種別は**足さない**（4 つのセッションが同時に足すと衝突する）。既存のコマンドの `variant` か予約の欄を使う。どうしても要るときは依頼に書く。
  - 新しいスキーマの列は足してよい。既存の列の所有者は変えない。
  - 版の数字は上げない。統合のときに、まとめて 1.3 に上げる。
- **CONTRACTS.md**: 自分の決定は、自分の節（`## 段階 7-<セッション>`）にだけ書く。決定番号は上の表の範囲から振る。§7 の変更履歴の 1 行は、自分の節の中に書く（統合のときに §7 へ移す）。
- **凍結ファイル**（`tests/framework/`、`tests/mocks/`、ルートの `CMakeLists.txt`、`cmake/`）には触らない。
- **既存のテストのアサーションは書き換えない**。`Defect_*` も含む。決定と食い違うテストは、テストを書いた側に直させる。
- LUDIARS の運用ファイル（CLAUDE.md、`spec/domains`、Excubitor カタログ、CI）は作らない。

## 5. 完了条件（各組とセッションの終わり）

```
cmake -S . -B <dir> -G Ninja && cmake --build <dir> -j2 && ctest --test-dir <dir> -j2 --output-on-failure
```

- 7 構成すべてで、警告ゼロ（-Werror）・全件通過にする。
  - gcc Debug
  - clang Debug（`-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`）
  - `-DSND_SANITIZE=address,undefined`
  - `-DSND_SANITIZE=thread`
  - Release
  - `-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release`
  - `-DSND_CODEC_OPUS=OFF`
- dsp に触る組では、`-DSND_SIMD_SCALAR=ON` と `-DCMAKE_CXX_FLAGS="-mavx2 -mfma"` も回す。
- tool に触る組では、`-DSND_BUILD_TOOL_CORE=ON` と `-DSND_BUILD_TOOL=ON` も回す。Tela は LUDIARS/tela をビルドし、`CMAKE_PREFIX_PATH` で渡す。
- 並行性のテストは、TSan と Release で `ctest --repeat until-fail:20 -R <名前>` を通す。
- 新しいファイルと変えたファイルは、MinGW（`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror`）でも通す。

## 6. 資源の制限

メモリ不足でコンテナが何度か落ちたので、次を守る。

- ビルドを走らせるエージェントは、同時に 1〜2 体までにする。
- ビルドは `-j2`、ctest は `-j2`（TSan の繰り返しは `-j1`）。構成は 1 つずつ回す。
- ビルドディレクトリはリポジトリの外（`/tmp/...`）に作り、終わったら消す。
- 長いコマンドはバックグラウンドで走らせ、ログに先頭のコミットを書く。

## 7. 終わりの報告

`spec/handoff/parallel/report-<セッション>.md` を書いて push する。書くこと:

- 作ったもの
- D-番号ごとの決定
- 契約の変更点（統合担当が衝突を解くための一覧）
- 他の持ち場への依頼
- 構成ごとの結果
- 未検証の箇所
- ブランチの先頭のハッシュ

コミットのメッセージは日本語で書き、末尾にそのセッションのツールが求める署名を付ける。
