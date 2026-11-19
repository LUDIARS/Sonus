# 本社への引き継ぎ（2026-11-19）

ここまでの全部は `int/s2-integration`（093827f）に入っている。リモートの `main` は README だけで、本体はまだ入っていない。
本社でのマージ、実機確認、負荷下での再確認をこの文書の順に進める。

## 1. ブランチ

`int/s2-integration` は、下の各ブランチを `--no-ff` でこの順にマージしたもの。
各ブランチは実装担当と独立テスト担当の両方を通し、受け入れ確認も済ませてある。

| 順 | ブランチ | 内容 | 決定 |
| --- | --- | --- | --- |
| 1 | `int/contract-1.1` | 契約 1.1、bank / io / jobs の統合（bank の依存解決の修正 7e2155b を含む） | D-1〜D-55 |
| 2 | `int/s2-core` | core（テーブル、コマンド、ボイス、バス、Parameter、カテゴリ） | |
| 3 | `int/s2-source` | Sample / Stream ソース | |
| 4 | `int/s2-opus` | Opus コーデック（libopus、BSD-3-Clause） | D-56〜D-60 |
| 5 | `int/s2-platform` | 出力バックエンド（ALSA / WASAPI / Core Audio）とオーディオスレッド | D-61〜D-66 |
| 6 | `int/s2-livelink` | ライブリンク | D-83〜D-87 |
| 7 | `int/s2-tool` | ツール（ミキサー・プロファイラ、UI は Tela） | D-67 |
| 8 | `int/s4-music` | music（トランスポート、テンポマップ、予約、Stinger） | D-68〜D-82 |
| 9 | `int/s2-api` | C API の本実装と構成ルート。契約 1.2（版の数字はユーザー承認で 1.2 に上げた） | D-88〜D-99 |
| 10 | `int/s6-embed` | サブプロジェクトとして取り込める形、Tela なしのツールの核（`Sonus::sonus` / `Sonus::tool_core`） | D-100〜D-101 |
| 11 | `int/s6-host-api` | ホスト向けの C API（内蔵の無音デバイス、ファイルシステムの根、再生ごとの音量とピッチ） | D-102〜D-104 |

`main` に入れるときは、`int/s2-integration` を 1 本の PR にして取り込めばよい。
各ブランチを個別に入れる場合は、この表の順に入れる。

## 2. 本社でやること

### 2.1 全構成の再実行（Rv でローカル）

```
cmake -S . -B <dir> -G Ninja && cmake --build <dir> && ctest --test-dir <dir> --output-on-failure
```

- 構成:
  - gcc Debug
  - clang Debug
  - `-DSND_SANITIZE=address,undefined`
  - `-DSND_SANITIZE=thread`
  - Release
  - `-DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release`
  - `-DSND_CODEC_OPUS=OFF`
  - `-DSND_BUILD_TOOL_CORE=ON`
  - `-DSND_BUILD_TOOL=ON`（Tela を `CMAKE_PREFIX_PATH` で渡す）
- このセッションでの最後の結果は、警告ゼロ・全件通過。テストの件数はツールなしで 213 件、ツールの核で 218 件、ツール入りで 232 件。
- `platform_device_play_test` は実デバイスが要るので、ここでは SKIP になっている。

### 2.2 負荷下での再確認（このコンテナは 4 コアで、強い負荷を作れなかった）

並行性のテストを TSan と Release で `ctest --repeat until-fail:50`、CPU を埋めた状態で回す。特に次のテスト。

- core のゲートを使うテスト: `LateChunksHoldVoicesAndAlternateSets`、`HeldVoiceFadesThenContinues`。CloseGate / OpenGate で決定的にしてある。
- ライブリンク: `IndepLivelinkProfiler.MetersAndVoicesMatchTheOutput`。Ack と受信数で同期する形に直した。スレッドを飢えさせる shim で 200/200 通過。
- api: `api_indep_*`、`api_lifetime`、`api_threads`、`api_synth`、`api_host_output`、`api_playback_gain`。
- platform: `platform_indep_audio_driver_test`。OutputClock の seqlock を直したもの。
- 調べたいもの: TSan で `base_adv_queue_test` が 120 秒のタイムアウトに 1 回当たった（警告なし）。

### 2.3 実機の確認（このコンテナでは確かめられていない）

| 対象 | 確かめること |
| --- | --- |
| Windows（MinGW、WASAPI） | ビルド（ここでは MinGW の `-fsyntax-only -Werror` だけ）。WASAPI の出力、デバイスの変更と LOST、排他モード。D-103 の Win32 ファイルシステム（`\\?\` の長いパス、UNC、ディレクトリごとに大文字小文字を区別する設定）。`api_host_fs_test` は symlink を作るので権限が要る |
| macOS / iOS（Core Audio） | ビルド（OBJCXX）。出力とルートの変更、iOS のセッションの中断と Resume。D-103 の O_PATH が無い経路（realpath、lstat、O_NOFOLLOW、dev / inode の照合） |
| Linux（ALSA） | 実デバイスでの再生（`platform_device_play_test`）。アンダーランからの追い付き |
| 性能（設計書 14 章） | 実ボイス 128 本で 1 ブロックが予算の 30% 未満。初期化後の確保ゼロ、RT 安全性の検出ゼロ |
| ツール | Windows と macOS のホスト（Tela::Windows / Tela::MacOS）で画面が出て操作できること |

### 2.4 Ergo 連携

- LUDIARS/Ergo#99 は CI が通っていて、衝突なし。レビュー待ち。
- 中身:
  - ergo_audio の Sonus バックエンド（`-DERGO_AUDIO_BACKEND=sonus`）
  - Studio の sonus プラグイン
- Ergo は `ERGO_SONUS_DIR` で Sonus のチェックアウトを指す。`main` に Sonus が入ったら、その場所を指す。
- 確かめていないこと:
  - Windows / macOS での Ergo のビルド
  - 窓のある Studio での表示
  - ergo_audio（Sonus 版）のゲームと Studio を実際につなぐ組み合わせ

## 3. Tela への提案（Tela 側の課題として本社へ）

ツールを作る中で、Tela に欲しいと分かったもの 9 件。

1. canvas の exclusive を retained で持つ
2. ElementPointer
3. wheel、修飾キー、click_count
4. text_input
5. 例外を使わない検査 API
6. Linux の文字の描画
7. MacOSView
8. 要素の見た目の指定
9. drawing の差し替え

## 4. 持ち越し（段階3 以降）

- 未着手:
  - physics
  - 段階3 のカバレッジ
  - ツールの完成
  - 段階6 の拡張
- 契約 1.2 の候補で見送ったもの（理由は CONTRACTS.md「契約 1.2 の候補で入れなかったもの」）:
  - API の入口の範囲の絞り込み
  - BankManager の fail-fast
  - source の終わりの理由
  - VoiceStatic の列
  - デバイスの問い合わせ
  - livelink の追加メッセージ
  - music の P1〜P3、P5、P7〜P10
  - バンク形式 2.1
- 受け入れた振る舞い（注記済み）:
  - 一時停止したボイスはリングを返す。そのため再開の直後は無音になる（D-97）。
  - user buffer の音量・ピッチは、子の再生には掛からない（D-104）。
- 運用ファイル（CLAUDE.md、`spec/domains`、Excubitor カタログ、CI）は作っていない。後で別に作る。
- `spec/handoff/` は引き継ぎ用の一時置き場なので、本社の作業が終わったら消す。
