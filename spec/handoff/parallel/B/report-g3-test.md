# 組③ 独立テスト報告

## 性質の一覧

実装を読む前に設計書 §2.5・§4.2〜4.3・§4.6・§14、受け入れ調査 §2③、組③依頼、D-130／D-131、公開 API、コマンドおよびバンクの契約から記録した。以下は検査の予定であり、通過結果は実行後に追記する。

| 項目 | 満たすべき性質 | 入力と観測量 |
| --- | --- | --- |
| スコープ | 許可された Playback、Emitter、Global、定義の既定値の順で選ぶ。禁止スコープへの設定で、許可された値を隠さない。 | mask 0〜7 で異なる音量値を順に設定し、同じ Emitter の再生 2 本と別の Emitter を比較する。旧バンクの列省略を明示の 7 と比較する。未定義の慣習名には定義済み Parameter の mask を流用しない。 |
| 追従速度 | 初回の既定値から目標に向かって指定単位／秒で進む。複数の声が同じ値を参照しても速度は変わらず、途中の再指定は現在値から進む。 | 線形の音量カーブを使い、音量から Parameter を逆算。上り・下り各 2 区間、反転の前後、停止点、rate=0 を確認する。 |
| 多点カーブ | 両端より外は端の値、節点で定義値、区間内はその区間の形状に従う。 | 非対称な 3 点以上を置き、両端の外、節点、線形区間と指数区間の内点を振幅で区別する。 |
| 音量・ピッチ | 複数の音量曲線の dB を加算し、ピッチ曲線の cents を加算する。 | 音量は基準音に対する振幅比、ピッチは既知の正弦波の周波数で比較する。曲線の行順を逆にして同じ値を得る。 |
| LP・HP・乗算 | LP は最も低い、HP は最も高いカットオフ。その他の乗算対象では 1 が中立、2 つの寄与を掛ける。 | 単独の制約の強いフィルタと二曲線の周波数応答を比較する。パンは左右の個々のゲインを測り、省略列・明示 compose・従来の単曲線を区別する。 |
| 空間・宛先 | Spread、ReverbSend、Blend3D は最終の音場・送りに反映される。Send／EffectParam の宛先情報がない場合に別行を変更しない。 | 左右で異なる位置に固定した音源の左右比、独立した補助バスの出力を比較する。宛先を持つカーブは B-2 と組①の合流後に確認する。 |
| スナップショット | 重み 0 は寄与なし。複数の重みを同時に反映し、片方を解除しても他方と基準値が残る。途中の再指定で開始点に飛ばない。 | 2 枚を異なる重みで同じ行に適用。適用前・遷移途中・終了・片方解除・全部解除の音量を記録し、別対象の対照音と比較する。 |
| エフェクト・送り | SNPV は指定した EFFX 行・paramIndex、SEND 行だけを変える。D-130 の設定値と寄与を合成して最後に範囲制限し、初期化専用値は実行中に変えない。 | 異なる 2 行の効果量と送りを用意する。API のベース値を寄与中に変更してから解除。プリ／ポストの出力を送り元フェーダーの変更前後で個別に測る。バンク再登録時は再登録した値を使う。 |
| 時間付きアクション | ApplySnapshot は親トラックの予定時刻に一度だけ適用し、遷移時間は重みが目標に着くまでの長さを示す。 | 発火とフェードの長さを異なる値にし、予定前・開始・途中・完了後のサンプルを照合。予定前の親の停止、ポーズ・再開、ブロック境界の両側も検査する。 |
| レベルでのダッキング | しきい値以下は変化なし。異なる超過量で異なる定常ゲインとなり、減衰量には上限がある。無音で元のゲインに戻る。 | 対象を一定音とし、キーを独立に無音→しきい値→超過小→超過大→上限超え→無音と動かす。対象のみの振幅を定常時と attack／release 中に測る。D-131 のコンプレッサーのキー動作も維持する。 |
| ルール型ダッキング | 指定カテゴリが発音している間は減衰し、最後の声の終了後に解放する。 | 同じカテゴリの重なる 2 本を別々に停止。対象振幅の攻撃中・1 本停止・最後の停止・解放中・定常復帰を比較する。 |
| カテゴリ | 音量・ミュート・ソロは可聴ゲイン、ポーズは再生進行に影響する。全体・カテゴリ・Playback のポーズ要因を別々に解除できる。 | 同じバスに異なるカテゴリのカウンタ音源を入れ、音量と再開最初のサンプルを照合する。kCategoryIgnorePause と kEventIgnorePause を別々に立てる。 |
| ソロ・フォーカス | 複数のソロ、親子、解除を反映し、SND_FOCUS_PER_CATEGORY の喪失で kCategoryMuteOnFocusLoss の対象だけが消音する。復帰後も明示ミュートの状態を保つ。 | 親・子・兄弟の音を区別し、ソロの追加・個別解除・全解除・再登録を比較する。フォーカス中も消音対象の再生位置を測る。 |
| 旧記録・負荷 | variant 0 は従来の意味を保ち、未知の variant が音や状態を変えない。資源制限で他の再生や設定に途中の変更を残さない。 | 同じ台本に未知の variant を挿入して音と通知を比較。短いアリーナ・ちょうどの設定数・1 超過を別々に調べる。 |
| 決定論・RT | 遅延チャンクと負荷 LOD が無い同じ台本では、直列 2 回と RT ワーカー 2 の出力がビット一致する。通常レンダー内の確保・RT 違反は 0。 | 複数声・複数対象を同時変更した出力バッファ、割り当てカウンタ、RT 検出数を検査する。128 声の Release 計測は構成と測定値を明記する。 |

## 仕様の確認点と依存先

- `CURV.compose` は省略時の読み値も明示の AddDb も 0 になる。対象ごとの既定との優先関係、パンの中立点と旧バンクの扱いは確定した契約に合わせる。現状の観測値を根拠に新規則を決めない。
- `CURV` に EFFX／SEND の宛先行、`ACTN` に遷移時間がない。現行形式で表現できる動作と、実行時の追加 API、バンク／buildtool の変更後に検査可能な入力を分ける。
- ダッキングの超過量に対する曲線と attack／release の時定数、カテゴリ親子と複数ソロの合成、フォーカスのホスト経路は公開契約の確定を待つ。core の直接検査とホスト経由の検査はそれぞれ実行結果を記録する。
- 不許可スコープや無効行の警告コード・タイミングは公開契約に従って確認する。バンク検証段階で拒否される入力は、core に登録済みと仮定しない。

## 検証状況

性質の記録まで。新規の独立テスト、構成別実行、欠陥判定は未実施。

### Cloudで追加する観測条件（実装閲覧前）

同じバスIDの行を2バンクで共有する。対象とキーを別出力にし、片方のバンクだけを解放した後、残存する再生のゲイン復帰と同じキーの再駆動による再減衰を測る。開始直後のスナップショットは親が拒否された場合の対照音、受理時の経過時間、再指定・停止後の出力で区別する。全組横断では共通の台本を直列2回とRTワーカー2本で実行し、出力バッファ、通知、スペクトルをビット比較する。128実声では同じ機能を作動させ、確保、RT違反、1ブロックの予算比を別々に記録する。

## 復元した独立試験

バンク入力、公開コマンドとレンダー出力から以下を検査する。共有キーは両方のバンクに同じキーIDの出力バスを持たせ、対象音のバスと再生は残存バンクだけに置いた。

| 性質 | 実行ケース |
| --- | --- |
| パラメータ・スコープ・追従 | `IndepG3Param.AllowedScopesWinInPlaybackEmitterGlobalOrder`, `IndepG3Param.TwoCurvesAddDecibelsAndClampToEndpoints`, `IndepG3Param.LegacyPanAddsCurvesToClipInStoredOrder`, `IndepG3Param.ExplicitPanMultiplyCombinesContributions`, `IndepG3Param.RampRateFollowsElapsedTimeRegardlessOfVoiceCount` |
| ピッチ・フィルタ・カーブ | `IndepG3Curve.CentsAddToAnOctaveInEitherOrder`, `IndepG3Curve.LowpassUsesMinimumAndHighpassUsesMaximum`, `IndepG3Curve.ExponentBelongsOnlyToItsOutgoingInterval` |
| スナップショット・FX・送り | `IndepG3Snapshot.TwoWeightsReleaseIndependentlyToBusBase`, `IndepG3Snapshot.EffectContributionTracksChangingBaseAndReleases`, `IndepG3Snapshot.SendContributionsRespectPreAndPostFaderTaps`, `IndepG3Snapshot.RetargetedWeightContinuesFromCurrentLevel`, `IndepG3Snapshot.SharedBusEffectOffsetsResolveInEachBanksOwnRowSpace` |
| ターゲットへのダッキング | `IndepG3Ducking.LevelTracksDifferentOverThresholdInputsAndCaps`, `IndepG3Ducking.LastCategoryVoiceReleasesRule` |
| 共有キーバスの解放と再駆動 | `IndepG3Shared.SurvivingKeyRedrivesCompressorAfterOwnerUnload`, `IndepG3Shared.SurvivingKeyRedrivesLevelDuckAfterOwnerUnload` |
| カテゴリ・フォーカス・独立出力 | `IndepG3Category.ParentAndLeafSolosCombineAndClear`, `IndepG3Category.RegainingFocusPreservesExplicitMute`, `IndepG3Category.IndependentOutputIsAbsentFromMasterButPublishesItsOwnPeak` |
| ポーズの優先順位 | `IndepG3Pause.IgnoreFlagsSurviveGlobalPauseButIndividualPauseStillStops` |
| 親アクションの受理・拒否・再開 | `IndepG3Action.AcceptedInitialActionUsesTransitionDuration`, `IndepG3Action.RejectedInitialActionPreservesAlreadyRunningTransition`, `IndepG3Action.PausingBeforeFutureActionDelaysItsTransition`, `IndepG3Action.RetargetDuringScheduledParentsFirstFadeUsesItsCurrentWeight` |
| 全組横断の出力・RT・CPU | `IndepCrossStage3.SerialReplayAndTwoWorkersMatchAudioNotificationsAndSpectrum`, `IndepCrossStage3.Real128VoicesRunWithinBudgetWithoutAllocationOrRtViolation` |

単独ターゲットは `indep_g3_mix_test`、`indep_g3_control_test`。組①・④を含む先頭には `indep_b_cross_test` も登録される。性能試験はRTワーカー0と2のプロセスCPU時間をRenderBlockごとに測定し、ブロック時間に対する平均比を記録する。独立計画とソースを照合した時点では組③単独25ケース、全組横断5ケース。実行結果は別途記載する。

D-162 の3ケース（IndepCrossCurves）は、距離曲線のあり・なし、パラメータ0・0.5・1について、2本のカーブの積と等価な距離曲線だけの参照シーンを比較する。Spread は左右、Blend3D は距離4での減衰、ReverbSend はドライ信号がマスターに届かないバスグラフの左右を観測する。

### 独立担当の新規 GCC Debug

`245e7ab` の `indep_g3_mix_test` は18件成功、失敗0。キー音のある最初のバンクを解放し、同じIDを持つ残ったバンクで再駆動した。対象音の振幅はコンプレッサーで 0.007076 → 0.056569 → 0.007076、レベル型ダッキングで 0.028351 → 0.056569 → 0.028351。いずれもキー音を出す前の振幅は0.056569。残存する対象再生への終了通知は0件だった。
