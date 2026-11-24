# セッション B（段階3 カバレッジ）: 実装担当・独立テスト担当に共通の決まり

受け入れ側（親セッション）が書いた。各担当は、自分への依頼文とこの文書を全部読んでから始める。

## 0. 読むもの（この順）

1. `spec/handoff/parallel/README.md`（4 セッション共通の決まり）と `spec/handoff/parallel/B-coverage.md`
2. `spec/handoff/README.md` の 3 章（役割と手順、決まりごと）
3. `spec/interface/acceptance/stage3-coverage.md`（出発点の調査。行ごとの実装状況、足りないもの、テスト基盤 §4、dsp の登録表 §5）
4. `spec/architecture/design.md` の 1.2、2.1、2.3〜2.5、3 章、4 章、7 章、14 章
5. `spec/interface/CONTRACTS.md`（大きい。D-15, D-17, D-18, D-35, D-36, D-43, D-44, D-48, D-54 と、自分の範囲に関わる決定を grep して読む）
6. 契約のヘッダ: `contract/public/include/snd/*.h`、`contract/internal/include/snd/internal/*.hpp`、`contract/internal/include/snd/format/bank_format.hpp`、`spec/interface/bank-format.md`

## 1. 書いてよい場所

- 持ち場: `core/`（src, include, test）と `dsp/`（src, include, test）。
- `api/compose/c_api.cpp`: **末尾への追記だけ**（新しい公開関数の配線）。ほかの行は変えない。
- `contract/`: **追加だけ**。
  - 公開ヘッダ（`sonus.h` / `sonus.hpp` / `config.h` / `types.h`）: 新しい関数、列挙の末尾への値の追加、`snd_config` の末尾（`config.h` の X マクロの末尾）への項目の追加。新しい構造体の型は足さない（引数はスカラーと既存の型だけ）。
  - `schema.hpp`: 新しい列と、新しい読み出し宣言。既存の列の所有者は変えない。
  - `commands.hpp`: **新しいコマンド種別は足さない**。既存コマンドの予約欄を variant として使う（下の割り当て表）。構造体の大きさと既存の欄の意味は変えない。予約欄に名前を付け直すのは可（D-89 の前例）。
  - `bank_format.hpp`、`livelink_protocol.hpp`、`source_if.hpp`、`music_if.hpp`、`notify.hpp` は変えない。必要なら「持ち場の外への依頼」として報告に書く。
  - 版の数字（`version.h`）は上げない。
- **触らない**: `tests/framework/`、`tests/mocks/`、ルートの `CMakeLists.txt`、`cmake/`、`bank/`、`buildtool/`、`livelink/`、`source/`、`music/`、`platform/`、`io/`、`codec/`、`jobs/`、`base/`、`physics/`、`tool/`、`api/`（上の例外を除く）、`tests/integration/`。
- `spec/interface/CONTRACTS.md`、`spec/interface/acceptance/stage3-coverage.md`、`spec/handoff/parallel/requests/`、`report-B.md` は受け入れ側が書く。担当は書かず、報告に材料を出す。
- **既存のテストのアサーションは書き換えない**（`Defect_*` も含む）。自分の変更で既存のテストが落ちたら、(a) 自分の変更が既存の約束を壊した → 変更を直す、(b) 既存のテストが、今回決めた新しい仕様と食い違う → 直さずに報告する（受け入れ側が判断する）。
- `dsp/` の「エフェクトの登録表」（`stage3-coverage.md` §5 の 5 か所）と、`dsp/CMakeLists.txt`・`dsp/test/CMakeLists.txt` のソース一覧は、**末尾に追記するだけ**。並べ替えない（セッション D も同じ場所に足す）。`core/CMakeLists.txt` と `core/test/CMakeLists.txt` の一覧も末尾追記にする。
- LUDIARS の運用ファイル（CLAUDE.md、`spec/domains`、Excubitor カタログ、CI）は作らない。

## 2. 予約欄の variant の割り当て（組どうしで重ならないように受け入れ側が決めた）

| コマンドの欄 | 0（従来） | 割り当て |
| --- | --- | --- |
| `SetParam.reserved[0]` | 通常の Parameter | 1 = ボイスエフェクトのパラメータ（組②）。2〜3 = 組①（Emitter の形など）。4〜5 = 組③ |
| `SetSwitch.reserved[0]` | 通常のスイッチ | 1 = ボイスエフェクトの種別（組②）。2〜3 = 組①（ゾーンの送り先バスなど）。4〜5 = 組③ |
| `SetBusVolume.reserved`（下位 8 ビットが variant） | バスの音量 | 1 = バスのエフェクトのパラメータ（組②）。2 = 組③ |
| `ApplySnapshot.reserved` | 従来どおり | 組③ |
| `SetCategoryVolume.reserved` | カテゴリの音量 | 1 = ソロ（組③） |
| `Play` 系・`Stop`・`SetScheduledStop` の予約欄 | 従来どおり | 組④（フェードインの指定など） |
| `SetListener.reserved`、`SetTransform` の予約欄 | 従来どおり | 組① |

- variant が 0 のときの振る舞いは、ビット単位で従来と同じにする（1.1 / 1.2 の記録は 0 を書いている）。
- 知らない variant の値は、何もせず捨てる（落ちない）。
- 予約 ID（`SND_ID_RESERVED_*`）は空きが無いので新しく取らない。
- 使った variant は、報告の「契約の変更点」に全部書く（ライブリンクの規則表 `livelink/src/command_check.cpp` への追記は、受け入れ側が C への依頼にする）。

## 3. 実装の決まり

- C++20、例外なし・RTTI なし、警告ゼロ（`-Werror`）、サードパーティ依存なし。
- 原則 1（設計書 1.2）: オーディオスレッドと RT ワーカーの経路では、メモリ確保・ロック・ファイル IO・書式付きログをしない。新しい領域は全部、初期化時（`create.cpp`）に `Config` の予算から `IAllocator` で取る。バンクごとに要る領域はバンクの登録準備（通常ワーカー）で取る。
- 予算を超える要求は確保失敗にせず、仮想化・間引き・理由付きの拒否で吸収する（原則 2）。
- D-43: チャンク（並列描画）が触ってよいメモリは、そのチャンクのレンダー項目、そのチャンクのバス別アキュムレータ、スレッド別アリーナ、RealVoice のスロット番号で引く状態だけ。新しいボイス別の状態は RealVoice のスロットで引くプールに置き、ブロックごとの入力は直列区間でレンダー項目に写す。
- 列の所有者（D-18, D-35）: 列への書き込みは `WriteColumn` / `SND_W`、読み出しは宣言済みの `ReadColumn` / `SND_R` を通す。1 列 1 書き手を守る。
- 決定論（2.5）: 乱数は Playback のシードから作る。加算の順序は固定。新しいシステムも「同じコマンド記録なら同じビット」を守る（並列描画と直列描画でも同じビット）。
- 短いフェードは `tuning.shortFadeMs`（D-15）。値の切り替えでクリックを出さない（ブロック内は線形補間）。
- フルセットで実装する。最小の縦切りで止めず、依頼文に書いた機能を全部、配線とテストまで入れる。できなかったもの・迷ったものは隠さず報告に書く。
- コードのコメントは既存に合わせて英語。決定を参照するときは `D-xxx`（依頼文で渡した番号）を書く。自分で決めたことは、報告に「決定の提案」として書く（番号は受け入れ側が振る）。

## 4. ビルドとテスト（資源の制限を必ず守る）

メモリ不足でコンテナが落ちたことがある。もう 1 体のエージェントが同時にビルドしている。

```
cmake -S <worktree> -B /tmp/snd-<組>-<構成> -G Ninja -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache [構成のオプション]
cmake --build /tmp/snd-<組>-<構成> -j2
ctest --test-dir /tmp/snd-<組>-<構成> -j2 --output-on-failure
```

- ビルドは必ず `-j2`、ctest は `-j2`（TSan の繰り返しは `-j1`）。構成は 1 つずつ回す。同時に 2 つ以上のビルドを走らせない。
- ビルドディレクトリは `/tmp/snd-<組>-<構成>`。使い終わった構成は消す（日常の作業用の gcc Debug 1 つだけ残してよい）。
- 長いコマンド（1 構成のビルドと全テストは約 10 分）はバックグラウンドで走らせ、ログの先頭に `git rev-parse HEAD` を書く。フォアグラウンドの時間制限に注意する。
- 日常の確認は gcc Debug で、関係するテストだけ（`ctest -R <名前>`）でよい。
- 完了条件（報告の前に全部通す。警告ゼロ・全件通過）:
  1. gcc Debug
  2. clang Debug（`-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`）
  3. `-DSND_SANITIZE=address,undefined`
  4. `-DSND_SANITIZE=thread`
  5. `-DCMAKE_BUILD_TYPE=Release`
  6. dsp に触ったとき: `-DSND_SIMD_SCALAR=ON` と `-DCMAKE_CXX_FLAGS="-mavx2 -mfma"`
  7. 並行性に関わるテストを足した・変えたとき: TSan と Release で `ctest --repeat until-fail:20 -R <名前>`（TSan は `-j1`）
  8. 新しいファイルと変えたファイル: MinGW の構文検査（`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror` に、そのファイルのコンパイルと同じ `-I` と `-D` を付ける。gcc Debug の `compile_commands.json` か `ninja -t commands` から取る。スレッドを使うヘッダで win32 版が通らないときは `x86_64-w64-mingw32-g++-posix` を使い、その旨を報告に書く）
  - `-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release` と `-DSND_CODEC_OPUS=OFF` は受け入れ側が回す（時間があれば回してよい）。
- 性能（設計書 14 章）: Release で `CoreLoad.Real128VoicesWithinBudget`、`IndepLoad.Real128VoicesUnder30PercentAndFourTimesTheLimit`、`dsp_pipeline_bench` 相当が通ること。出た数値（1 ブロックの時間と予算比）を報告に書く。新しく足した処理が既定で全ボイス・全バスに掛かるなら、その負荷もベンチに含める。
- 初期化後の確保ゼロと RT 安全性の検出ゼロ（`CoreMemory.NoAllocationAfterInitialisation`、`CoreRt.RenderPathHasNoRtViolations` の形）を、新しい経路にも掛ける。

## 5. コミット

- 自分の worktree のブランチにだけ、区切りごとにローカルコミットする（コンテナはいつ落ちてもおかしくない。エージェントの利用上限で止まることもある）。**push はしない**。ほかのブランチ・ほかの worktree には触らない。
- メッセージは日本語。末尾に次の 2 行を付ける（モデル名は書かない）。

```
Co-Authored-By: Claude <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01K4Pd9PDdP5abfoHnj6zaKe
```

## 6. 報告

最後の返事と同じ内容を、依頼文で指定したファイル（スクラッチパッド）にも書く。途中でも、区切りごとにそのファイルを更新する（落ちたときに続きが分かるように）。書くこと:

- 作ったもの（機能 → ファイル・関数 → 自分で足したテスト名）
- 決定の提案（仕様に無くて自分で決めたこと。選んだ理由と、ほかの選択肢）
- 契約の変更点（足した関数・列挙値・`snd_config` の項目・スキーマの列と読み出し宣言・variant。ファイルと行）
- 持ち場の外への依頼になるもの（バンクの列、buildtool、livelink、source、api など。何が要るか、それまでどう代替したか）
- 構成ごとの結果（構成、テストの件数、通過・失敗、コミットのハッシュ）と、性能の数値
- できなかったこと・未検証の箇所・既知の制限（隠さない）
- 既存のテストで、新しい仕様と食い違うと思うもの
- ブランチの先頭のハッシュ
