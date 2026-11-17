# libopus（Sonus に同梱した版）

Sonus のサードパーティ依存は、この libopus だけ（`spec/interface/CONTRACTS.md` D-56）。
ライセンスは BSD 3 条項（`COPYING`）。Opus の特許は IETF に宣言された条件で無償許諾されている（`LICENSE_PLEASE_READ.txt`、
https://opus-codec.org/license/ ）。

## 取得元と版

| 項目 | 値 |
| --- | --- |
| 取得元 | https://github.com/xiph/opus |
| タグ | `v1.5.2`（`git clone --depth 1 --branch v1.5.2`） |
| コミット | `ddbe48383984d56acd9e1ab6a090c54ca6b735a6`（2024-04-10） |
| 取得日 | 2026-11-16 |

ソースは無変更（`.c` / `.h` は取得したまま）。足したのはこのファイルだけ。

## 除いたもの

ビルドに使わないものを除いた（元は約 17 MB、同梱は約 3.3 MB）:

- `.git/`
- `dnn/`（約 11 MB。深層 PLC・DRED・OSCE の重みとコード。Sonus は `ENABLE_DEEP_PLC` / `ENABLE_DRED` / `ENABLE_OSCE` を定義しない）
- `doc/`, `tests/`, `celt/tests/`, `silk/tests/`, `training/`, `scripts/`, `m4/`, `meson/`, `cmake/`
- 上流のビルドシステム: `CMakeLists.txt`, `meson.build`, `meson_options.txt`, 各ディレクトリの `meson.build`,
  `Makefile.*`, `configure.ac`, `autogen.*`, `*.pc.in`, `opus.m4`, `update_version`, `releases.sha2`, `ChangeLog`, `NEWS`, `README.draft`
- 使わない実装: `silk/fixed/`（固定小数点版）, `celt/mips/`, `silk/mips/`, `celt/dump_modes/`
- デモ: `src/opus_demo.c`, `src/opus_compare.c`, `src/repacketizer_demo.c`, `celt/opus_custom_demo.c`

`celt/x86/`, `celt/arm/`, `silk/x86/`, `silk/arm/` は残したが、ビルドしない（下記）。

残したもの: `include/`, `celt/`, `silk/`（`silk/float/` を含む）, `src/`, `COPYING`, `AUTHORS`, `README`,
`LICENSE_PLEASE_READ.txt`, ソース一覧の `*_sources.mk` / `*_headers.mk`。

## ビルド設定（`third_party/CMakeLists.txt`）

上流のビルドシステムは使わず、`third_party/CMakeLists.txt` が静的ライブラリ `sonus_opus` を作る。
ソースの一覧は上流の `*_sources.mk`（autotools / CMake / meson と共通）から読む:
`CELT_SOURCES`, `SILK_SOURCES`, `SILK_SOURCES_FLOAT`, `OPUS_SOURCES`, `OPUS_SOURCES_FLOAT`。
ただし `OPUS_SOURCES` のうちアンビソニックス（`opus_projection_*`, `mapping_matrix.c`）は使わないので除く。

| 設定 | 値 | 理由 |
| --- | --- | --- |
| 数値 | 浮動小数点版（`FIXED_POINT` なし） | Sonus の内部形式は float32。デコードは `opus_multistream_decode_float` |
| スタック確保 | `VAR_ARRAYS`（C99 の可変長配列） | 作業領域はスタックに置かれ、malloc しない。`NONTHREADSAFE_PSEUDOSTACK` は使わない（大域の作業領域を malloc し、スレッド安全でない）。`USE_ALLOCA` もスタックだが VLA で足りる |
| `OPUS_BUILD` | 定義 | 上流のビルドと同じ |
| `ENABLE_HARDENING` | 定義 | 上流の既定。内部の整合性検査（入力の誤りはエラーコードで返り、ここには来ない） |
| `HAVE_LRINTF`, `HAVE_LRINT` | 定義 | glibc / 主要な libc にある |
| SIMD | 使わない（`OPUS_X86_*` / `OPUS_ARM_*` を定義せず、実行時 CPU 判定 `OPUS_HAVE_RTCD` も無し） | 移植可能な C だけで、どの x86-64 / ARM64 でも同じ結果。速度は十分（下記） |
| 深層学習の拡張 | 無し（`ENABLE_DEEP_PLC`, `ENABLE_DRED`, `ENABLE_OSCE` を定義しない） | `dnn/` を同梱しない |
| カスタムモード | 無し（`CUSTOM_MODES` を定義しない） | 48 kHz の標準モードだけを使う |
| 警告 | `-w`（Sonus の `-Wall -Werror` を掛けない） | Sonus のコードではない |
| `-ffp-contract=off` | 指定 | Sonus と同じ。FMA の縮約で符号化結果が機械ごとに変わらないように |
| サニタイザ | `SND_SANITIZE` の構成ではその指定を libopus にも付ける | デコーダの状態（デコーダプールのスロット）の外を触らないことを ASan / UBSan / TSan でも確かめる |

`SND_CODEC_OPUS=OFF` では `sonus_opus` を作らない（ルートの `CMakeLists.txt`）。

実測（gcc 13、x86-64、Debug ビルドの libopus）: デコードのスタック使用量はスレッド全体で約 32 KB（RT スレッドの保証 256 KiB の 1/8）。
デコードは 1 チャンネル・20 ms のパケットあたり約 0.1 ms（実時間の約 0.5%）。値は `codec_opus_test` の `Opus.StackUsageAndSpeed` が出力する。

## 版を上げるとき

1. 新しいタグを同じ手順で取得し、上の「除いたもの」を同じように除いてこのディレクトリを置き換える。
2. この表（タグ、コミット、取得日）と `third_party/CMakeLists.txt` の `PACKAGE_VERSION` を直す。
3. `codec/include/snd/codec/encode.hpp` の `kOpusEncoderVersion`（差分ビルドのキャッシュ）を上げる。
4. `codec_opus_test`（`OPUS_GET_LOOKAHEAD` = 312 を確かめている）と統合テストを全構成で回す。
