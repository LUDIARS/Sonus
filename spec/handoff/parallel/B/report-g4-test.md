# 組④ 独立試験・再確認

## 性質の一覧

実装を読む前に、設計 4.2・4.4、公開終了理由と B-handoff §4 から次の観測条件を定めた。

- 音素材のない親は最後のアクションを適用するまで生存し、停止後には未適用のアクションを発火させない。
- 親のトラック時刻は出力フレームで進み、親の素材のピッチ、ループに左右されない。Playback の一時停止中は止まる。再開ブロックで期限を迎えるアクションも、繰り返し操作を含めて一度だけ発火する。
- レイヤーは 2 枚重なる範囲で等パワーを保証する。3 枚重なったときの各ゲインは規定しない。
- restart は素材の頭に戻る。開始時の線形フェードの時刻は Playback の経過フレームを使い、仮想ボイスの間は進み、Playback の一時停止中は止まる。
- グループが最後のボイスを奪った再生は STOLEN / LIMIT_GROUP。ボイスが残る間は再生を終了しない。残ったボイスの自然終了、明示停止、新規拒否はそれぞれ対応する終了理由を返す。終了通知は再生ハンドルごとに一度。

## 独立した観測

新規実行ファイルは `indep_g4_recheck_test`。すべて公開の再生操作と、独立フィクスチャの音声・終了通知で検査する。

| 条件 | 独立試験 |
| --- | --- |
| 3 枚のレイヤーを含む Event で実際に 2 枚だけ鳴る区間 | `IndepG4Recheck.EqualPowerWhereOnlyTwoOfThreeLayersOverlap`。各層をソロにして 4 地点を測り、非該当層の音量ゼロと残り 2 層のゲインの二乗和 1 を検査 |
| 素材 192 フレームをループし、ピッチ ½ 倍または 2 倍、途中 4 ブロック一時停止 | `PausedPitchedLoopFiresOnePulseAtItsTrackFrame`。再生時刻 720 に配置した 32 フレームのパルスについて、出力上の開始・終了時刻と総発音フレーム数を検査 |
| restart の実音源を仮想化し、仮想のまま一時停止・再開 | `RestartFadeCountsVirtualTimeAndExcludesPausedTime`。一時停止 0・8 ブロックの両方で、実ボイスに復帰した後の 1 ブロック全サンプルを 4800 フレームの直線と比較 |
| グループに最後の音を奪われた再生 | `IndepG4Steal.StolenPlaybackEndReason` で oldest、quietest、lowest priority の 3 モードの理由・詳細・通知回数。既存 `NeverRejectsTheNewRequest` で新規拒否 |
| 先に奪った後の残存ボイス | `SurvivingVoiceDeterminesNaturalOrStoppedEnd` は残ったボイスの発音、自然終了と明示停止の理由・詳細・回数を検査。`TwoGroupsStealingTheLastVoicesEndPlaybackOnce` は 2 グループが同一ブロック／別ブロックで全ボイスを奪った場合を検査 |
| 無音の親の最後の制御アクション | `ActionOnlyParentAppliesFinalSnapshotBeforeEnding`。480 フレームにある Snapshot の最初の適用ブロックは 512。前ブロック末尾、適用後の信号、親の終了理由・通知時期を検査 |

## 復元前に決めた検査条件

設計 §4.2・4.4、commands.hpp の Play.flags、B-6 に基づく。

- 同一グループに収容しきれず最後のボイスを失った再生には STOLEN/LIMIT_GROUP が届く。新規拒否は REJECTED、全体実ボイス上限による仮想化では継続する。
- 素材上の有限ループの反復回数は可変ピッチに依存しない。無限ループの出力から音で同定した次の反復開始点で有限再生は終了する。
- 通常・ワンショット・位置指定の Play と各フェード指定の録音で、下位予約ビット 2〜15 はゼロ。オプションの意味とフェード長は定義済みビットで識別する。
- 予約停止フレームより前のアクションは一回発火し、同時刻以降は取り消す。拍指定の再生は実トランスポートが示す位置に達した時点で発火する。

## 復元後の独立担当の GCC 14 Debug 実測

対象 `2147b0d`。新規 Ninja ビルドで警告 0。CTest は下記 8 実行ファイルを通過した。組②を含む全構成と最終性能は受け入れ側の記録を参照する。

| 実行ファイル | 成功したケース |
| --- | ---: |
| core_test | 44 |
| core_g4_test | 59 |
| indep_g4_seq_test | 64 |
| indep_g4_voice_test | 38 |
| indep_g4_analysis_test | 12 |
| indep_g4_system_test | 4 |
| indep_g4_recheck_test | 6 |
| indep_g4_boundaries_test | 5 |

- B-6 の 7 箇所は `Ends::Has` が対象ハンドルと reason、detail の三者を照合。7/7 で STOLEN / LIMIT_GROUP。階層試験ではグループの最古 2 再生が奪われ、残る 3 声は実 2・仮想 1。REJECTED と終了回数 0 の既存期待も成功した。
- `PitchChangeCutsFiniteLoopAtMeasuredWrap` の無限再生は 600、1200、1605 フレームで波形が再開。3 周設定では 1604 まで信号があり、1605 以降が完全無音。COMPLETED / NONE が 1 回届いた。
- `PlayModesLeaveReservedBitsClear` は通常・ワンショット・位置指定・フェード 3 値を録音。全 6 コマンドでビット 2〜15 は 0、上位 16 ビットのフェードは 0・123・65535 ms。
- `ScheduledStopCancelsAtItsFrame` は停止の前・同時・後を 1 フレーム刻みで検査した。`MusicalReservationAnchorsTrackTime` は実 MusicSystem の 120 BPM の次拍を使い、親の初回アクションは 1 ブロック以内、次は拍から 480 フレームで発音。各パルスの長さは 32 フレーム。
- `RefusesMeterCapacityBeyondPoolBudget` は通常設定で初期化に成功。4096 枠設定では OUT_OF_BUDGET で終了しシステムは NULL。
- アクションのみの親と交互の一時停止に関する旧 Defect_ 2 件は成功。再発防止試験名を `ActionOnlyEventLivesThroughItsLastAction`、`ActionsKeepTheirOrderAcrossAlternatingPauses` に変更。期待する出力と時刻は維持した。
