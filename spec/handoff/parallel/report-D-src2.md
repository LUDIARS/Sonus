# セッション D・ソース組 2（中間報告）

受け入れ判定は保留。分岐元は `05ed0b2908c034763215afa18dfc740436952b8e`、受け入れ側が取得したコードは `47e81e05df3465ecde1df3af735f382b8a95e912`。

## 実装と決定

| 決定 | 内容 |
| --- | --- |
| D-200 | Noise の復帰差の記述を訂正。Input は最低 4 ブロックを確保し、不足に入った回数を数える。Oscillator は実効周波数 0 との行き来で位相を保持し、5 ms で利得を変える。最初の有効な描画・仮想進行の実効周波数で開始利得を決める |
| D-201〜203 | Granular は常駐 PCM16/float を元のレートで読み、ボイスあたり 8 粒の固定プールを持つ。密度・長さ・位置・ピッチ揺らぎを変調できる。整数シークで小数位置をクリアし、仮想時間のキャリーを呼び出しの分割に依存させない |
| D-204〜206 | PTCH v1 は 12 バイトのヘッダ、1 ノード 20 バイト、最大 16 ノード。呼出側のバッファで組立・検証できる。入力は前のノードに限り、最後が唯一の出力。発振、雑音、演算、フィルタ、ADSR、LFO などを評価する。変調の刻みは 32 標本 |
| D-207 | Runtime で両種別の有効化を個別指定。null は無効、非 null かつ空き枠なしは VOICE_LIMIT。空きがあれば検証し、不正な記述子を拒否したら枠を戻す |

契約ヘッダへの変更は `GrainWindow` と `GranularParams`、`PatchParams`、`RenderItem` の末尾追加。版とコマンド種別に変更はない。公開 C API は core の配線を要するため D-08 に関数案を記録した。D-06 は PTCH と buildtool、D-07 はバンク・core のパラメータ配線の依頼。ゲームからのバンク経由再生は統合後の検査対象。既存の core が null 記述子を渡す経路の検査を追加した。

## 独立テストと差し戻し

旧 Oscillator 3 件は即時無音を想定していた。位相と 5 ms の利得を標本ごとに照合する検査へ更新した。旧 Input 4 件は要求容量と実容量が等しい前提を見直した。多周回のビット一致、遅延上限、スレッド間の収支、非有限値のゼロ化と部分超過を個別に検査した。

新たに確認した欠陥は 3 件。半フレーム仮想進行後の整数シークで公開小数部が残る、長い仮想進行を分割すると整数フレームがずれる、最初の実効周波数 0 の発振器から音が出る、の各件を修正した。独立担当は回帰を通常検査に移し、極小ピッチの累積と初回のピッチランプの境界検査を追加した。

机上では各種別のプールと作業領域を初期化時に確保し、描画は担当スロットの状態とチャンクの出力・スクラッチを使用する。Granular は古い粒を固定枠で更新する。Patch の検証はノード数、全体サイズ、入力の依存順を確かめた後で読み進める。PCM 本体の寿命は core のバンク保持に依存する。

## 決定と検査の対応

| 決定 | 主な独立検査 |
| --- | --- |
| D-200 | `StartsAtFirstEffectiveFrequency`、`PartialFadesAreBitExactAcrossBlockSizes`、`BitExactAcrossManyWraps`、`ThreadedWriterExactAccounting`。実効周波数、位相、遅延、入力収支を検査 |
| D-201 | `AssetValidationPcm16AndFiniteDefaults`、`ExactSizePcmEndpointsLoopsAndLatchedModulation` |
| D-202 | `EveryBirthMatchesClosedCountIncludingRamps`、`ComplementaryWindowsAndOldestOfEight`、`WeightedEightGrainStereoInterpolationMatchesDirectSum` |
| D-203 | `PitchSpectrumAndSeededJitterExtent`、`LongVirtualDurationIsPartitionIndependent`、`AllBinary32FractionalPlacesCarryThroughAFullFrame`、`HannEndpointsAndVirtualReturnAtEveryAnchorOffset` |
| D-204 | `WireFormatUnalignedIsExactAndTransactional`、`InvalidByteSlicesAreRejectedByValidatorAndStart` |
| D-205 | `OscillatorGainAndFilterComplexResponse`、`ArithmeticSaturationAndBipolarClip`、`FixedKnotLfoRandomAndWhiteNoise` |
| D-206 | `AdsrKnotsGateAndLastRelease`、`ScratchShortageSeekAndFilterVirtualResidual` |
| D-207 | `RuntimeRegistrationIsIndependent`、`ConcurrentChunksRetainTheirWaveformsAndBankViews`、core 経由の無効パラメータの検査 |

### 負荷に関する差し戻し

128 ステレオ声 × 256 フレーム、48 kHz、40 ms Hann、200 粒/秒では、独立担当の初回測定で定常と一斉復帰がともに 5 回とも予算（5,333 µs）を超えた。8 粒の出生間をまとめて描き、ステレオ間で補間位置を共有し、Hann 窓を 32 標本ごとに絶対位置で補正するよう差し戻した。独立担当は窓長 1・2・3・31・32・33・63・65・1,920・384,000 で全標本を照合し、復帰時の 32 種類のオフセット、PCM16 と float の左右異なる信号で 8 粒の直接和を追加検査した。Release 対象 8 本と追加 2 ケースが通過し、初期化後の確保と RT 違反はいずれも 0 件だった。

| 測定 | 200 粒/秒の定常・復帰中央値（µs） | 予算超過（定常・復帰） |
| --- | ---: | --- |
| 実装担当、CPU 2 固定、旧版と交互に 5 回 | 2,728.4 / 2,654.8（旧版 6,487.7 / 7,184.5） | 0/5・0/5 |
| 独立担当、共有環境、5 回 | 3,702.6 / 4,323.9 | 1/5・2/5（最大 8,626.7 / 8,127.9 µs） |

独立担当の同じ測定で、20 粒/秒は 543.8 / 250.4 µs、Patch は 1,687.3 / 2,053.2 µs だった。機能回帰は検出されなかった。共有環境の実測から、毎ブロックの締切を常に満たすとは判断できない。

## 受け入れ側のクリーンビルド

各構成で新規ビルドディレクトリを使用。件数は CTest に登録された実行単位で、成功には SKIP を含めない。

| 構成 | ビルド | 警告 | CTest 成功 / 失敗 / SKIP |
| --- | --- | ---: | --- |
| gcc-debug | ビルド中 | — | — |
| clang-debug | 実行待ち | — | — |
| asan | 実行待ち | — | — |
| tsan | 実行待ち | — | — |
| release | 実行待ち | — | — |
| notests | 実行待ち | — | — |
| noopus | 実行待ち | — | — |
| clang-asan | 実行待ち | — | — |
| clang-tsan | 実行待ち | — | — |


音声実機、バンク・core から Granular／Patch を鳴らす統合、共有環境外でのリアルタイム上限は未検証。§6.1 の既知 DSP 3 実行単位とデバイス SKIP は分けて判定する。

この環境では、空の GCC/Clang ASan プログラムでも LeakSanitizer が ptrace の制限で終了時に失敗した。リーク検査を無効にする環境変数は使用していない。


## 統合時に照合する契約の追記事項

下記は担当者が用意した基点比の追加文のみ（18 行追加、0 行削除）。リモートの `spec/interface/CONTRACTS.md` への同期は未完了。更新手段が要求した既存文書全文の転送を自動承認レビューが拒否したため、今回の追加文をここに保存する。

| D-200 | **以下の更新事項は先行決定の該当箇所に優先する。** (1) Noise の仮想復帰・Seek では D-193・D-199 (2) のとおり直前 1024 標本でフィルタを再構成する。連続描画との復帰直後の最大差について、D-199 (2) の実測値をピンク約 8e-3、ブラウン約 1.6e-3 に訂正する。値は復帰時のフィルタ状態に依存する。(2) D-194・D-197 (3) の Input の underruns は不足の開始回数。直前が満量だったときの不足だけを数え、開始・Seek・Resume 直後、連続する不足は数えない。(3) D-190・D-198 (1)(3) の Oscillator は、非有限なピッチを 0 として扱う場合を含め、実効周波数が 0 になれば 5 ms の線形フェードで無音に移る。この間は位相と最後の正の dt を保持する。各サンプルの描画前に整数利得位置を 1 減らし、正に戻れば 1 増やす。範囲は 0〜max(1, round(rate × 0.005))、正規化して波形に掛ける。途中反転可。最初の実描画・仮想進行の実効周波数（基準周波数 × 無害化したピッチ）が 0 なら初期利得 0、正なら 1。0 フレームの呼び出しでは決めない。仮想でも同じ増減。(4) D-194 の Input ポート容量に下限を設ける。既存のリング容量と 4 × blockFrames の大きい方を 2 の累乗へ切り上げ、D-197 (2)(4) の規則を適用する。 | 周波数切替時の段差を抑え、最新 1 ブロックを残す規則・遅延の下限 2 ブロックを実効にする。 |
### ソース組 2（Granular、Patch）での決定（D-201〜D-207）

| # | 決定 |
| --- | --- |
| D-201 | Granular は元レートで合成し、core のリサンプラでボイスピッチとレート差を処理する。常駐 PCMF/PC16、1〜8ch、1〜384000 Hz、1〜2³²−1 フレームのみ。Start で記述子を写し、PCM は Sample の直接再生と同じくボイスが保持するバンクから Release まで借りる。チャンネルが足りなければ最後を複製する。非有限の標本は 0、PCM は ±1 に制限。位置は毎回固定の正規化中心に一様乱数 × positionJitter を足して [0,1] に丸め、素材区間の先頭〜最終標本に写す。グレインは pitchCents ＋ 一様乱数 × pitchJitterCents（揺らぎ幅 0〜4800、合計 ±4800 セント）で前進、線形補間する。ループ有効なら有効な素材ループまたは素材全体で折り返し、無効なら最終フレームの端数部は最終値、その先は 0。 |
| D-202 | 1 ボイス 8 枠、全体は 8 × maxVoices。出生番号 g は 0 からで枠 g mod 8 に入る。過負荷では 8 回前の出生を置換する。シードと出生番号から位置・ピッチ用の独立したカウンタ乱数を作る。Q=2³²、位相初期値 Q−1、各素材フレームで I=round(Q × clamp(密度,0,素材レート)/素材レート) を加算し、桁上がりでそのフレームに発生。N フレームの出生総数は floor((Q−1+ΣI)/Q)。長さ L=max(1,round(素材レート × clamp(lengthMs,0,1000)/1000))。年齢 a=0..L−1 に対して Hann=(1−cos(2πa/L))/2、三角=1−abs(2a/L−1)、矩形=1。偶数 L なら Hann・三角は間隔 L/2 で和 1、矩形は間隔 L で和 1。重なりの加算を max(1,窓の和) で割る。 |
| D-203 | 密度、長さ、中心位置、ピッチ揺らぎの加算ランプを RenderItem に追加。素材フレーム f/n で補間し、密度は毎フレーム、残りは出生時に固定。有限でない端点を含むランプは全体を 0、非有限の基準パラメータは 0。AdvanceVirtual はパラメータのオフセット 0 で桁上がり総数と最後の最大 8 件の出生時刻を閉じた式で復元し、元レート換算済みの有限な正の binary32 pitch（上限32）× outputFrames は整数部・端数を正確に累積し、呼び出しの分割によらない。公開する fraction は端数の上位32ビットであり、それ以下の繰り上がりも保持する。Seek は端数を 0 にし、指定の素材フレームへ基準値で再構成。次の実描画・仮想進行で frame と fraction の両方を公開する。SetLoop は有効な [frame,frame2) またはループ解除に切替え、旧グレインを破棄。Pause/Resume/Virtualize/Devirtualize は状態維持、停止中は core が描画を休む。終了なし、常駐のため待機・先読みなし。 |
| D-204 | PTCH v1 は LE 12 バイトのヘッダと 20 バイト固定長ノード列（最大 16）。詳細は D-06。最終ノードが唯一の Output、全ノードが出力に寄与し、入力は手前のノードだけ。引数の有限性、範囲、未使用欄、バイト数を検証する。BuildPatchProgram は呼出側領域に書き、失敗なら 0 で領域を保つ。Start は O(N) で検証したノードを専用プールへ写す。元のプログラムは以後不要。 |
| D-205 | 内部レートで、定数・帯域制限 Oscillator（D-190 と同じ波形）・白色雑音・加算・乗算・1極ローパス・ADSR・正弦 LFO・ランダムホールド・対称クリップ・出力を順次評価。算術結果は各段で ±16、出力は ±1、全 ch 同値。オシレーターは周波数 × pitch、dt を [0,0.4999] として Q32 に丸め、0 なら無音・位相維持。雑音のキーは開始 seed とノード番号。LP は y += (1−exp(−2π × min(fc/rate,0.5))) × (x−y)。変調器はボイス開始起点 32 フレームの格子点で評価し、間を線形補間。LFO・ランダムは Q32 に四捨五入した位相増分（乱数は完了した周期数の下位 32 ビットで引く）。 |
| D-206 | gateFrames=0 は保持、正ならその絶対出力フレームで全 ADSR をノートオフ。秒を最寄りフレームに丸め、A は 0→1、D は 1→S、R はノートオフ時のレベル→0 で線形。長さ 0 の段は即座に次へ。最後のリリース終了を格子点に切り上げた所で ended、ADSR またはノートオフの無い声は無期限。Release は直ちに解放し、core の停止フェードとは独立。Seek は指定フレームの無変調位相へ、AdvanceVirtual は指定ピッチで位相・時間を O(N) で進める。両方ともフィルタ履歴を 0 にするため復帰時のフィルタ出力差は初期状態差 × 極の n 乗で減衰する（多段は各段に適用）。他の制御は状態維持、先読み不要。中間値はスレッドの scratch に N 個の float、各ボイスの後に返す。不足時は無音・starved とし、ランプ終値で仮想進行する。 |
| D-207 | Runtime は両種別を既定で登録、enableGranular/enablePatch で個別に外せる。プールは種別ごとに maxVoices、nullptr は満杯時も invalid slot / NONE。非 null は容量を先に検査し、空きなしなら中身を読まず VOICE_LIMIT。空き枠を確保してから検証し、不正なら枠を返して invalid slot / NONE。API のバンク無し再生は core に記述子の所有とコマンド適用が必要であり、D-08 に案を渡す。 |

| 版 | 日付 | 変更履歴（統合時に §7 へ） |
| --- | --- | --- |
| 1.2（段階 7-D ソース組 2） | 2026-11-24 | D-200〜D-207。Granular と Patch、PTCH v1、Runtime の登録。GranularParams・PatchParams・RenderItem の末尾に追加。公開 C ABI とコマンド種別は既存どおり。 |


