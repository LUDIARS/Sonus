# sonus-build

プロジェクト（1 オブジェクト 1 ファイルの JSON、`spec/interface/project-format.md`）からバンク
（`spec/interface/bank-format.md`）を作る CLI。設計書 10 章。

```sh
sonus-build --project game/audio --out build/banks [--platform LNX_] [--release] [--strip-strings] \
            [--cache build/sonus-cache] [--ids-header sonus_ids.h] [--quiet]
```

| オプション | 内容 |
| --- | --- |
| `--project <dir>` | プロジェクトのディレクトリ（必須） |
| `--out <dir>` | 出力先（必須）。バンク、`streams/`、`ids.json`、ゲーム用ヘッダ |
| `--platform <TAG>` | `ANY_`（既定）`WIN_` `LNX_` `MAC_` `IOS_` `AND_`。ヘッダの機種タグと、プロジェクトの機種別変換設定の選択 |
| `--release` | 開発ビルドにしない（`GUID` チャンクと `kFileDevBuild` を付けない）。既定は開発ビルド |
| `--strip-strings` | `STRS` / `NAME`（名前表）を入れない |
| `--cache <dir>` | 差分ビルド: 波形の変換結果を内容ハッシュで保存し、次回は再エンコードしない |
| `--ids-header <rel>` | ゲーム用ヘッダの出力先（`--out` からの相対、既定 `sonus_ids.h`、空で出さない） |
| `--quiet` | 診断だけを出す |

終了コード: 0 成功、1 ビルドエラー（何も書かない）、2 引数の誤り。

診断は `ファイル:行:列: error: 理由` / `warning:` の形で標準エラーに出る（ファイルはプロジェクトからの相対パス）。
エラーの例: 参照の解決失敗、ID の衝突（プロジェクト全体）、入れ子 Event の循環、バスグラフの循環、バンク依存の循環、
依存していないバンクのオブジェクトの使用、エンコーダの無いコーデック、WAV の不備。警告の例: 一致しえない遷移ルール行（8.5）、
未知のメンバ、DSP の状態サイズの算出関数が無い（暫定値を使う）。

## ライブラリとして使う

`snd/buildtool/build.hpp` の `snd::buildtool::Build(options, diagnostics, &result)`。差し込み口は `snd/buildtool/plugins.hpp`:

- `EncoderRegistry`: コーデックの fourcc → エンコーダ。組み込みは `PCMF`（float32）と `PC16`（int16）。ADPCM / Opus は codec
  モジュールのエンコーダを `Register()` する。登録の無いコーデックはビルドエラー。エンコーダの `version` は差分ビルドのキーに入る。
- `DspSizer`: エフェクト 1 個の状態サイズ（`EFFX.stateBytes`、`DSPN` の合計）。dsp モジュールが提供する。無ければ 1 個
  16 KiB の暫定値で、警告を 1 回出す。

## 変換の範囲（段階1）

- WAV: PCM 16/24/32bit、IEEE float 32/64bit、WAVE_FORMAT_EXTENSIBLE、`smpl` チャンクのループ点。8bit・圧縮 WAV は未対応。
- サンプルレート変換は線形補間（決定論的。音質は段階1相当で、帯域制限なし）。
- チャンネル変換: 同数はそのまま、1 → N は複製、N → 1 は平均、N → M（M < N）は先頭 M、M > N は残りを無音。
- コーデック: `PCMF`, `PC16` のみ組み込み。
