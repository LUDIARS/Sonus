# Sonus

ゲーム向けの C++ サウンドランタイム。データ指向設計（SoA のテーブルをシステムが順に処理する）で、
オーディオ処理の時間とメモリを固定の上限内に収める。調整はスタンドアロンのオーサリングツールから、
実行中のゲームにライブ接続して行う。

- 設計書（正本）: [`spec/architecture/design.md`](spec/architecture/design.md)
- 契約（凍結済みのインターフェース・スキーマと、その変更手順）: [`spec/interface/CONTRACTS.md`](spec/interface/CONTRACTS.md)
- バンク形式: [`spec/interface/bank-format.md`](spec/interface/bank-format.md)
- ライブリンク: [`spec/interface/livelink-protocol.md`](spec/interface/livelink-protocol.md)

現在は **段階0「契約の凍結」** が終わった状態。公開 API・内部 IF・テーブルスキーマ・コマンド・バンクとライブリンクの
形式は宣言まで、モック（無音デバイス、メモリ FS、決定論ジョブ）とテスト枠組みは実装まで済んでいる。
各モジュールの本体は段階1以降に並列で実装する（設計書 15.1）。

## ビルドとテスト

必要なもの: CMake 3.24 以上、Ninja、C++20 / C11 コンパイラ（gcc 13、clang 18 で確認）。サードパーティ依存は
同梱の libopus だけ（`third_party/opus/`、ソースからビルドする。`SND_CODEC_OPUS=OFF` で外せる。下の「サードパーティのライセンス」）。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

# clang
cmake -S . -B build-clang -G Ninja -DCMAKE_CXX_COMPILER=clang++
# サニタイザ（address,undefined または thread）
cmake -S . -B build-asan -G Ninja -DSND_SANITIZE=address,undefined
cmake -S . -B build-tsan -G Ninja -DSND_SANITIZE=thread
```

| オプション | 既定 | 内容 |
| --- | --- | --- |
| `SND_BUILD_TESTS` | ON | テスト、モック、テスト枠組みをビルドする |
| `SND_NO_EXCEPTIONS` | ON | `-fno-exceptions -fno-rtti` |
| `SND_WERROR` | ON | 警告をエラーにする（`-Wall -Wextra -Wpedantic` ほか） |
| `SND_SANITIZE` | 空 | `address,undefined` または `thread` |
| `SND_RT_CHECKS` | テスト時 ON | RT 安全性の検出フック。OFF なら完全に消える |
| `SND_LIVELINK` | ON | ライブリンクを含める（リリースでは OFF） |
| `SND_BUILD_TOOL` | OFF | オーサリング GUI（未実装） |
| `SND_CODEC_OPUS` | ON | Opus コーデック（同梱の libopus 1.5.2 をビルドする）。OFF なら Opus のデコーダ・エンコーダは無い（CONTRACTS.md D-56） |

テストの便利な使い方:

```sh
./build/tests/selftest/testkit_selftest --list            # テスト一覧
./build/tests/selftest/testkit_selftest --filter Golden   # 名前で絞る
SND_UPDATE_GOLDEN=1 ctest --test-dir build -R dsp          # ゴールデン音声の基準を更新
SND_RT_ABORT=1 ./build/core/test/core_test                 # 最初の RT 違反で abort（デバッガ用）
```

## ディレクトリ構成

```
contract/            凍結した契約（ヘッダのみ）。誰も直接編集しない
  public/include/snd/      公開 C API（*.h）、C++ API（*.hpp）、拡張 IF（ext/）
  internal/include/snd/    内部 IF（internal/）、バンクとライブリンクの形式（format/）
  test/                    契約の自己検査（ハッシュ、配置、所有者、コンパイル失敗の確認）
base/ platform/ jobs/ io/ codec/ dsp/ bank/ core/ source/ music/ api/ physics/ livelink/ buildtool/
                     モジュール（設計書 12.1）。各自 include/snd/<module>/, src/, test/ を持つ
tool/                オーサリング GUI（未着手）
tests/
  framework/         テストランナー、ゴールデン音声比較、RT 安全性の記録とフック
  mocks/             無音出力デバイス、メモリ FS、決定論ジョブシステム
  selftest/          枠組みとモックのテスト
  integration/       モジュールをまたぐテスト
cmake/               共通のビルド設定（警告、サニタイザ、モジュール・テストの関数）
third_party/         同梱のサードパーティ（libopus だけ。SONUS_README.md に版と除いたファイル）
spec/                設計書と契約の仕様書
```

モジュールの依存はルートの `CMakeLists.txt`（`SND_MODULE_DEPS_*`）に 1 か所だけ書いてあり、各モジュールの
`CMakeLists.txt` は自分のソースを並べるだけでよい。全モジュールを束ねたライブラリは `sonus`（`api/compose/`）。

## 契約の凍結ルール

- `contract/`、モジュール依存表、`spec/interface/` の仕様書は凍結されている。変更は
  「変更提案 → 影響するモジュールの合意 → 版を上げて反映」の手順に限る（設計書 12.4、`CONTRACTS.md` 2 節）。
- 版は 3 系統: 契約（`SND_CONTRACT_VERSION`）、バンク形式、ライブリンク。追加だけなら minor、それ以外は major。
- 設計書で決まっていなかった点の決定（D-xx）と、設計書の矛盾・不足（G-xx）は `CONTRACTS.md` にある。
- テーブル列の所有者はコンパイル時に検査される（`WriteColumn<System, Column>` / `ReadColumn<System, Column>`）。

## サードパーティのライセンス

`SND_CODEC_OPUS=ON`（既定）でビルドした Sonus（ランタイム・`sonus-build`）は libopus を静的にリンクしている。
Sonus を組み込んだゲームやツールを配布するときは、ソース・バイナリのどちらでも、libopus の著作権表示・条件・免責
（`third_party/opus/COPYING` の全文）を配布物の文書（クレジット、ライセンス一覧など）に含めること（BSD 3 条項の第 1・2 項）。
libopus の貢献者の名前を、配布物の宣伝・推奨に許可なく使ってはならない（第 3 項）。

| ライブラリ | 版 | ライセンス | 全文 | 備考 |
| --- | --- | --- | --- | --- |
| libopus | 1.5.2 | BSD 3 条項（"Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon"） | `third_party/opus/COPYING` | Opus の特許は IETF に宣言された条件で無償許諾（`third_party/opus/LICENSE_PLEASE_READ.txt`、https://opus-codec.org/license/ ） |

`SND_CODEC_OPUS=OFF` のビルドには libopus のコードは入らない（表示も不要）。
