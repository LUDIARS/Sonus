# 組① 独立テスト報告

## 性質の一覧

基準は 3675aad の設計書 2.1・2.5・4.3〜4.6・7・14 章、契約ヘッダ、組①依頼文の D-150〜157。以下を組①の実装と実装者テストを読む前に記録した。表の ID は検査結果と対応付けるためのもので、決定番号ではない。

| ID | 入力と定量的な期待値 |
| --- | --- |
| 01 ドップラー | 音速 343 m/s、基本周波数 600 Hz とする。放射方向に音源だけが音速の 1/4 で接近すると 800 Hz、離れると 480 Hz。リスナーだけが同じ速さで接近すると 750 Hz、離れると 450 Hz。径方向の速度が両者で等しければ 600 Hz。速度による位置更新が振幅・向きを変えないよう音量カーブを平坦にして試す。 |
| 02 ピッチの端点と変化 | 速度ゼロで係数 1 と 0 の波形がビット一致。平坦な音量カーブ上でパン方向を一定にして、係数 0 では径方向速度を変えてもビット一致。音速付近と超音速でも全サンプルが有限かつヘッダの比率範囲内。再生ピッチとの積は max_pitch_ratio 以下。速度切替のブロック内の瞬時周波数は旧値から新値へ移行する。 |
| 03 距離とコーン | 無曲線、コーンなし。距離 0・1・2・4 で定常振幅比 1・1・1/2・1/4、maxDistance より外は 0。音量カーブの 2 点を 0、−12 dB、CPNT.shape を 0 とすると中点は −6 dB（振幅比 0.501187）。同距離の正面と外角の外を比較し、coneOuterDb = −12 の外側は振幅比 0.251189、角度の遷移域は両端の間。 |
| 04 距離フィルタと spread | 小振幅の低音・高音を別々に入れる。距離 LP を狭めると高音／低音の定常 RMS 比が低下する。距離 HP を上げると低音／高音の比が低下する。距離カーブを逆向きにすると変化も逆。側方モノ音源で spread 0 は近側チャンネル優位、1 は左右レベルが等しい。 |
| 05 2D/3D ブレンド | 正面、音量・距離・コーンを固定。3D の定常ゲインが 1/4 のとき blend 0・1/2・1 に対応する相対ゲインは 1・5/8・1/4。ドップラーの比 r の中点は sqrt(r)、距離 LP の中点カットオフは 2D と 3D のカットオフの幾何平均。0 で距離・角度・径方向速度・距離 LP を変えても出力は 2D の対照に一致。側方での行列は 2D と 3D の中間。 |
| 06 複数リスナー | 有効人数 1 では全方式のサンプル列がビット一致。2 人が同位置・同姿勢では 1 人と同レベル。対称位置で重みを入れ替えると Weighted の左右 RMS が入れ替わる。Equal-weight Split は対称な結果となる。Nearest は近い 1 人の音に一致し、他方式の合成レベルは最大の単独リスナー以下。寄与を明確に離した設定でピッチ・距離 LP を単独時の主要リスナーと比較。寄与を逆転させたブロックでランプを測る。 |
| 07 遮蔽と閉塞 | ドライ・送りを独立に観測する。値 0 は未設定とビット一致。値 1 の遮蔽で両経路の低周波ゲインと高音／低音比が低下し、閉塞で直接音だけが同様に低下して送りは対照に一致。送り先を距離 SEND とゾーンそれぞれで確認。発音中の切替で追従・ランプの中間値を観測する。 |
| 08 範囲音源 | 半幅 2 の線・矩形・箱について中心と最近点を異ならせ、最近点から 1・2・4 m で既定のゲイン比 1・1/2・1/4。軸を 90° 回したときの最近点を座標から独立に求める。形の境界／箱内部で d = 0、左右の RMS が等しく、左右対称の外部点ではパンも鏡像。点は未指定とビット一致。発音中の位置・回転変更でパンがブロック境界で全量ジャンプしない。 |
| 09 距離による補助送り | Event のバスの最初の SEND を送り先に選び、通常のバス送りを対照測定で差し引く。一定の入力で曲線量 0・1/4・1 に応じて送り先の振幅が 0・1/4・1 倍、別の送り先には差分 0。該当 SEND が無ければ差分 0。再生の音量を 1/2 にすると補助送りも 1/2。ボイスエフェクトとパンを通した左右比・周波数応答が送りにも現れる。 |
| 10 ゾーン | ゾーンの量 0.8、フェード距離 4 m。リスナーが形の内側・境界、外側 2 m、4 m、6 m の相対送量は 0.8・0.8・0.4・0・0。音源だけゾーン内に置いた対照と区別する。複数の 3D 音源と 2D 音源を別々に鳴らし、3D だけに適用。複数バスの N および N+1 ゾーンを重ねて重み上位 N 個の送りを確認。同一バスの距離 0.75 ＋ゾーン 0.5 は 1。解除・再設定、ゾーン Emitter 自体の発音も確認。 |
| 11 寿命と原子性 | shortFadeMs を経てポーズ・停止・仮想化・バンク取り外しで該当ボイスの直接音と補助送りがともに 0。スロットを再使用した Emitter で以前の形・ゾーン・遮蔽が残らない。古い世代の設定は新しい再生に影響しない。同一ブロックに 2 回の形・ゾーン設定を送り、最後に成功した設定に一致。アリーナを満たして複数命令の書き込み途中が収まらない場合は前の設定を丸ごと保持する。 |
| 12 契約・境界 | 公開ヘッダの契約で決まった有限範囲の両端と直外、NaN、±∞、不正な形、上限のリスナー番号を検査。拒否された設定の後も有効値の音を維持。従来の variant 0 のパラメータ・スイッチは対照記録と一致。未割当の variant は同じ ID・値の従来パラメータや形・ゾーンを変えず、対照記録と波形がビット一致。 |
| 13 決定論・RT・負荷 | 記録が同じ台本で 0 ワーカーを 2 回、2 ワーカーを 1 回実行。LOD 無効、ストール・遅れチャンク 0、複数チャンクに異なる Emitter と非ゼロ送りを割り当て、全ブロック・全チャンネルがビット一致。初期化・バンク準備済みのレンダー中の確保増分 0、RT 違反 0。Release の 128 実ボイス、移動、ドップラー、補助送り、4 ゾーン、Split 2 人のブロック所要時間を測定し、ブロック実時間の 30% 未満と照合。 |

### 観測と判定について

定常 RMS と周波数は開始・移動・フィルタの過渡区間を除いて採り、ランプの検査では反対に時系列を保存する。位相と周波数は十分な窓の正弦波で測る。ゲイン・パンには小振幅の一定入力または同じ波形に対する比を使い、ピーク・RMS の検査にはゼロでない対照を置く。直接音が送り測定に混入する場合は、送り先バスの独立した計測か、送りを消した同一台本との差分を使う。

公開仕様が数値を保留しているものは、ドップラー比の頭打ち、遮蔽・閉塞の最大減衰とカットオフ、ゾーン数 N、形の使用軸、同順位のゾーン、全リスナーの重みが 0 の場合、およびフェード距離 0 の境界の扱い。ヘッダ・受け入れ決定で確定した値だけを厳密な期待値に採用する。

## 進捗

初回コミットは性質の事前登録だけ。組①の変更を含む公開ヘッダとテスト実装開始の連絡を待機中。テスト実行件数は 0。欠陥、実測値、構成別結果は未判定。

### Cloud 復元の事前条件

設計 §4.5、§7、公開ヘッダのリスナーと Emitter の契約を上記 01〜13 と再照合した。追加で、有限ループの終端とサンプル指定の予約停止について、音源を定数信号として直接とゾーン送りを個別に取り出す。各経路の LP が動作し、停止の直前に非ゼロであること、停止フレーム以後は左右の全サンプルが厳密にゼロであることを、それぞれ無加工の連続再生を対照として検査する。バスメーターの平滑化の影響を除くため、出力の区間電力またはサンプル値を使う。

### Cloud で復元した独立テスト

今回のテストを作った起点は 7aea75c。事前登録 1611484、初回テスト b9232dc、解除ランプ・容量境界 e0a725d、予約停止の時計 a9e777c、未知 variant 9f4d7e9、複数リスナーとカットオフ 246e64f。受け入れ実測と旧環境での試験結果は report-B に従う。

| 事前登録 | 主要な独立ケース（core/test/indep_g1_*_test.cpp） |
| --- | --- |
| 01, 02 | DopplerDistinguishesEmitterAndListenerVelocity、NearSonicDopplerIsFiniteAndBounded、ZeroVelocityAndDisabledDopplerKeepReferenceBits |
| 03, 04 | DistanceRolloffAndSilenceBeyondMaximum、DistanceVolumeCurveInterpolatesDecibels、OuterConeAttenuatesAccordingToDecibels、LowAndHighPassFollowDistanceInFrequencyDomain、SpreadMakesLateralEmitterCentered |
| 05, 06 | BlendInterpolatesAttenuationLinearly、BlendCutoffAtMidpointMatchesGeometricMean、ListenerModesWithOneListenerAreBitIdentical、CollocatedListenersPreserveSingleListenerGain、WeightedListenersSwapChannelsAndSplitIsSymmetric、DominantListenerSelectsDopplerRatio |
| 07, 08 | ObstructionFiltersOnlyDirectWhileOcclusionFiltersBoth、LineRectangleAndBoxUseNearestPoint、RotatingLineChangesItsNearestPoint |
| 09, 10 | DistanceSendUsesFirstBusSendAndSaturatesWithZone、ListenerPositionControlsContinuousZoneFade、TwoDimensionalEventNeverFeedsTheZone、StrongestFourZonesKeepIndependentDestinations |
| 11, 12 | RemovingZoneRampsWithinTheBlock、PartialZoneCommandNeverChangesItsDestination、PausedAndVirtualPlaybackLeaveNoZoneSignal、ClearingZoneAndReusingEmitterRemoveTheSend、InvalidZoneAndShapeDoNotOverwriteValidSettings、UnknownParameterVariantLeavesOrdinaryEmitterParameterUntouched |
| 13 | SpatialChangesRepeatBitForBitAcrossWorkers（10声・5チャンク、LOD無効、遅れ0、直列2回とワーカー2、確保件数・RT違反） |
| ①×④ | FiniteLoopCutsDirectFilterAtItsLastSample、FiniteLoopCutsZoneSendFilterAtItsLastSample、ScheduledStopCutsDirectFilterAtEndOfFade、ScheduledStopCutsZoneSendFilterAtEndOfFade |

①×④の切断位置は独立に計算した。有限ループは179サンプル×2周。予約停止は310フレームで48サンプルの解放フェードに入り、同じ358フレームで無音になる。各経路の両チャンネルについて357の信号と以後の全サンプルを確認し、連続再生の同じ358フレームも対照に置いた。CPU性能は実装担当の128実声の測定と、受け入れ側の再測定を参照する。

4件の停止試験を含め、追加確認後の独立ケースは37件。距離LPとの最小値合成は DistanceAndOcclusionChooseTheLowerCutoff、席別のゾーン量と定位は EachListenerContributesOnlyWhenInsideTheZone と OnlyListenersInsideZoneDetermineSendPan。B-1の生記録の命令数・順序・各欄は ShapeZoneAndClearRecordThePublishedWireFormat、公開書式からの波形再生は ReplayedShapeUsesTheDocumentedHalfExtentAxes が検査する。

### Cloud 差し戻し（修正前 e6635a2、独立担当）

GCC 14 Debug の新規作成で空間16件すべて成功。経路と生記録17件のうち12成功、次の5件が失敗。全組横断の4件は別のビルド。以下を落ちる Defect_ として保存した。

| 検査 | 実測と判定 |
| --- | --- |
| OnlyListenersInsideZoneDetermineSendPan | 対称2席の片方だけゾーン内。Split、Weightedとも左0.025、右0.025。左0、右0.05を期待。左右の方向を持つ各席の送量と定位を掛け合わせる。 |
| DistanceAndOcclusionChooseTheLowerCutoff | 距離LP300 Hzで1500 Hz/187.5 Hzの周波数応答比を測り、対照で正規化。遮蔽時は直達・送りとも0.405174、閉塞時の直達は0.272836。正規化比1を期待。重ねた遮蔽/閉塞のLPは距離LPより高く、各経路で最小値一段を適用する。 |
| PartialZoneCommandNeverChangesItsDestination | 残り1命令で宛先と量の一括設定を拒否する部分は成功。B-1の解除も -6 で拒否された。解除はSetSwitch 1命令で成功し送りが0となるはずだが、0.0707107のまま。 |
| ShapeZoneAndClearRecordThePublishedWireFormat | 形、ゾーン登録、解除の記録が5件（期待4件）。半寸法X2.5、Y1.25を与えたSetParamのvalueは1.25、paramは0x40200000、期待はvalue 2.5、param 0x3fa00000。 |
| ReplayedShapeUsesTheDocumentedHalfExtentAxes | B-1記載のvalue X、param Y bitsで線音源を再生した波形が、同じ半寸法を指定した公開操作の波形と異なった。 |

### ①×④の停止境界の独立実測

試作 fc230171e7aa9a866e8047a3795a3d59c467f4eb を新規 GCC 14 Debug で configure / build し、indep_g1_stop_test は4件すべて成功した。179フレームの有限2周と、310フレームに始まる48フレームの解放フェードの両方で、直達・ゾーン送りを別々に測った。両チャンネルとも357フレームに音が残り、358〜1023は厳密な0。試作の結果は正式なマージ先で行う受け入れと別に記録する。

### 修正後の独立再確認

GCC 14 Debug、対象 a7687dc。独立担当が空間16件、経路・生記録17件を実行し33件すべて成功した。差し戻し前に失敗した5件も同じ入力・期待値で通過。経路別LPの最小値合成、席別送りの定位、B-1の生記録と音声再生、残り1命令でのゾーン解除を確認できたため、5件を通常の回帰テスト名に戻した。単独枝で予約停止の2件も成功。有限ループを含む4件は上記①×④試作での独立実測。

MinGW-w64 POSIXの構文検査は回帰名に戻した 4bc0e66 で5/5成功。対象は indep_g1_scene.hpp と、spatial、routing、wire、stop の4つの .cpp。GCC Debug の各ターゲットと同じ -I / -D を使用し、-std=c++20 -Werror -fsyntax-only で検査した。
