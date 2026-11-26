# D-07 Granular・Patch のゲーム再生と検証

2026年11月26日、LUDIARS/Sonus の main と照合した差分。基点は `b512b8f3642a71adc4d58f3c73a8e83ebe761c3a`。ゲーム側は従来どおりバンクをロードし、イベントを `snd_play` で再生する。公開 C API の変更はない。

## D-06・D-07 と main の照合

| 経路 | main にすでにあったもの | 差分 |
| --- | --- | --- |
| バンク形式とライター | SYNP 列 0〜8、列省略時の既定値、PTCH の範囲参照、CRC | 任意列 9〜16 を追加。素材参照・チャンネル・レート・バイト数と PTCH の内容を共通のバンク検査で確認。バンク形式を 2.1 に上げた（列・列挙値の追加なので minor。bank-format.md §7） |
| buildtool | 名前付きグラフの依存順コンパイル、エディタ用 XPCH | 依存順の決定を共用。D-06 のインライン JSON 全11命令から PTCH v1 を生成。名前付きグラフは変換可能なものを同じエンコーダに通し、同じバンクで重複参照されても同一スライスを使う |
| source | Granular の粒生成、Patch の命令実行・ゲート・検証、各音源の独立試験 | PTCH の符号化・検証を共有の契約ヘッダへ移設し、source の既存入口を維持。Granular に仮想中の変調と、先読み境界からの復帰を追加 |
| core の再生 | イベントの合成音源分岐、既存リサンプラ、パラメータのスコープ・平滑化、バンクの解放待ち | ロード時に構築した記述子を音源に渡す。Granular は素材クロック、Patch は内部クロックで進める。4種類の加算曲線の両端値を直列段階で渡す |

**確認順**　`contract/internal/include/snd/format/{bank_format,bank_check,patch_program}.hpp` → `buildtool/src/{build,patch}.cpp` → `core/src/{bank_registry,sys_event,sys_dsp,sys_render,sys_voices}.cpp` → `source/src/granular_kind.cpp` → `api/test/api_granular_patch_test.cpp`。JSON 属性とバンクの詳細は差分中の `spec/interface/project-format.md` と `bank-format.md` に記載した。

## 再生・寿命・互換性

- バンク登録時に、既存のバンク所有メモリへ 64 バイト境界で GranularParams と PatchParams の配列を置く。素材とプログラムは同じ世代のバンクが所有する。既存の再生参照とエポックに解放を委ね、遅延ワーカーの描画完了まで元データを保持する。
- Granular の素材は常駐 PC16 / PCMF。素材レートと内部レートの比をピッチに一度だけ掛ける。仮想中も既存リサンプラと同じ Q32 の進み量を使い、復帰時は小数位相と先読み済みの素材フレームを引き継ぐ。短い履歴の容量は初期化時のリサンプラ先読み量と設定チャンネル数で決まる。
- 変調先は密度、粒長、位置、ピッチ揺らぎ。既存パラメータ評価で得たブロック両端値を描画アイテムにコピーし、粒の発生時に反映する。未指定時の加算量は 0。ワーカーでのバンク表の探索、新たなヒープ確保、ロック取得は追加していない。
- 旧 SYNP の省略列に D-07 の既定値が効く。XPCH しかない旧エディタバンクはロードでき、他のイベントは再生可能。実行プログラムのない旧 Patch イベントは従来の開始失敗通知を返す。旧形式の試験は、エディタが出力した XPCH とリンク表を残し、旧ライターが書かなかった情報を取り除いて CRC を更新したバンクで実施した。
- 名前付きグラフは固定周波数オシレータ、既定 seed の白色ノイズ、amp、mix、ADSR、output を変換できる。PTCH v1 で表現できない接続（例：parameterCurve からフィルタ周波数への入力）や変換後 16 ノード超過はゲーム向けビルドでエラー。エディタの開発プレビューでは診断を出し、該当グラフを XPCH に保持する。

## 公開 C API 回帰テスト

既存のテスト用仮想デバイスで 48 kHz の出力波形を取得。プロジェクト JSON を実際の buildtool でバンクにし、公開 API でロード・再生・パラメータ設定・アンロードを行った。

| テスト | 合格条件 |
| --- | --- |
| VirtualReturnPreservesMaterialClock | 24 kHz 素材の 600 Hz が 48 kHz 出力でも維持される。実ボイス上限 1 で優先度の高い 937 Hz Patch に交代し、復帰後の位相と振幅を比較。1 倍と小数ピッチ 2 条件 |
| ParameterSetWhileVirtualAffectsFirstReturningGrain | 仮想中に位置を変更し、その間に発生した粒が復帰直後から変更先の 1500 Hz を出す |
| VirtualPatchHonorsGateAndReturnPolicy | 24,576 出力フレームのゲートを持つ Patch を一時的に仮想化。`continue` は元の時刻で終了し、`restart` は復帰からゲートを数え直す |
| PlaybackParameterChangesEachGrainControl | 4つの制御先を個別に変更。密度と長さは RMS、位置は 600→1500 Hz の切り替わり、ピッチ揺らぎは固定周波数成分の低下と出力エネルギーで判定 |
| UnloadPlayingVoicesAndReloadDifferentBytes | 再生中の両ボイスに終了通知が各 1 回。1 バンクスロットに同一 ID の別素材・別プログラムをロードし、新しい振幅と周波数を確認 |
| HeldRenderDelaysBankRelease | テスト用の公開エフェクトで描画ワーカーを保持。アンロード中に `UNLOADING` が維持され、ワーカーを開放した後に `UNLOADED` となり、再登録に成功 |
| NamedPatchAndLegacyXpchOnlyBank | 新しい名前付き Patch を再生。XPCH のみの旧形式では該当イベントの旧挙動と、同じバンクの別イベントの有音出力を確認 |
| CorruptProgramsRejectedAtBankLoad | スライスの範囲超過、不正命令、PTCH 欠落、空スライスの4条件。各々 CRC を更新し、ロード完了通知の `SND_ERR_CORRUPT` を確認 |

## 検証と再現

Linux x86-64、GCC 14.2、C++20、Debug。通常構成は Opus と RT 検出フック、Tela に依存しないエディタコアを有効にした。通常・RT 違反時 abort・ASan+UBSan・TSan の各構成で上記 8 ケースが成功。全体 CTest の登録 374 件中、369 件成功、失敗 0 件。実デバイス再生試験 1 件をスキップし、プロジェクト既定で無効の 4 件は未実行。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSND_BUILD_TESTS=ON -DSND_BUILD_TOOL_CORE=ON -DSND_RT_CHECKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
SND_RT_ABORT=1 ./build/api/test/api_granular_patch_test
```

サニタイザはそれぞれ別の Debug ビルドに `-DSND_SANITIZE=address,undefined` または `-DSND_SANITIZE=thread` を指定し、`api_granular_patch_test` をビルドして実行した。ASan は `ASAN_OPTIONS=detect_leaks=0`、UBSan と TSan は `halt_on_error=1`。この環境では LeakSanitizer の終了時検査が ptrace 制限で動作せず、リーク判定は未検証。Windows・macOS・iOS、物理デバイス、Tela GUI、実機ゲームへの組込み・実機での負荷測定は未実行。
