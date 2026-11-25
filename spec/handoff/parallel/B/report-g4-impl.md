# 組④ 実装報告（ボイス管理・シーケンス種別・通知・解析）

worktree /home/user/sonus-wt/g4、ブランチ wip/b-g4（push なし）。コミット: ace9aa2（A と下地）→ 8d9f6ba（B〜E）→ 26c1136（MinGW 書式）→ ee1575f（決定論テストを LOD 無効で）。

## 1. 作ったもの（機能 → ファイル・関数 → テスト）

### A. ボイス管理（4.4）
- 奪い方「最も小さい」（VLGR.stealMode 1）を前ブロックの Voice.audibility で選ぶ。同じなら古い方。同じブロックで作られたボイスは可聴度が無いので「最も大きい」扱い（奪わない）。`sys_event.cpp` StealVictim。schema に `R(RunEventLogic, Voice_audibility, Previous)`。
- D-140 仮想ボイスの戻り方（EVNT.virtualReturn）。素材の印（ASST.flags Restart/Kill、source が解釈）が立っていればそれが優先（core は何もしない）、無ければ Event の値（VoiceStatic.virtualReturn、CreateVoice で決める）。
  - 続きから: 従来どおり。
  - 先頭から: SelectVoices が昇格時（仮想中に位置が進んだボイス）に kVsRestartReturn を立て、Devirtualize の frame を startFrame にする。Render の InitRenderState が再生位置を startFrame にし、フェードは 1（新しい開始と同じ）。`sys_voices.cpp`, `sys_render.cpp`。
  - 戻らない: 仮想になった時点（奪われたフェードが終わって実スロットを離れたとき、または上位 N に入らず実でないとき。ポーズ中は除く）に SelectVoices が kVsKillVirtual を立て、次ブロックの RunEventLogic が破棄。Playback の全ボイスが消えたら **SND_END_STOLEN / SND_END_DETAIL_VOICE_LIMIT**（既存の理由から最も近いもの。列挙は足していない）。
- テスト: CoreVoice2.StealQuietestTakesTheLowestAudibility / StealQuietestTieTakesTheOldest / StealQuietestSparesAVoiceOfTheSameBlock / StealLowestPriority / LimitHierarchyEventThenGroupThenGlobal / VirtualReturnContinueKeepsTheTimeline / VirtualReturnRestartStartsAgainFromTheBeginning / VirtualReturnNeverEndsTheVoiceAsStolen / VirtualReturnTheAssetFlagWins / VirtualReturnNeverEndsAVoiceThatStartsVirtual（core_voice2_test.cpp）。「古い」「奪わない」は既存 CoreVoice.LimitGroupNeverRejectsAndOldestSteals、ヒステリシス・予備枠フェードは既存 IndepMix.* に任せた。

### B. シーケンス種別と Clip（4.2）
- 重み付きランダム: 重み ≤0（NaN 含む）は選ばない。全部 0 なら等確率。`sys_event.cpp` StartEventTracks。
- D-141 シャッフル: 上限なし・確保なし。巡ごとに Playback の乱数から 32 ビットの鍵を引き、4 段の Feistel 置換（偶数ビットの最小の定義域）を cycle-walking して [0,n) の順列にする（PermuteIndex）。巡の先頭が前の巡の最後と同じなら 1 つずらす。EventState 末尾に shuffleKey/Pos/Rot/Last。
- D-142 CLIP.loopCount: 0=無限、1=既定（ループ点の無い素材は 1 回、ループ点のある素材は従来どおり無限）、N≥2=ループ区間（素材のループ点、無ければ全体）を N 周して最後の周の終わりで切る。source に回数を渡さず、Render（ClipEnd）が再生位置＋リサンプラの PendingOffset とブロック内の比のランプから切る出力フレームを求め frameEnd に入れ、チャンクが ended を報告。仮想ボイスは AdvanceVirtual 後に判定。
- CLIP.fadeOutMs: クリップの終わり（素材の終わり、または数えたループの最後）へ向かう直線フェード（素材の時間で）。無限ループには無し。PostProcessItem で掛ける。
- D-143 レイヤー: LayerWeight（sys_dsp.cpp）。範囲外 0、重なり無し 1、重なりは等パワー（下から始まる側が cos で出る、上が sin で入る）、範囲が接するだけなら共有値は上の Clip。重みの跳びは 1 ブロックあたり blockFrames/shortFadeFrames に制限（DspPriv.layerGain）。
- D-144 入れ子: 子 Event 自身のバス・カテゴリ・リミットグループ・優先度・3D・virtualReturn・カーブ・シーケンス種別。親 Clip の音量・ピッチ（乱数込み）を dB/セントで加算、Clip の startMs で開始を遅らせる。深さ 4 超は鳴らさず PostWarning(SND_ERR_LIMIT_REACHED)。
- D-145 アクション: 1 ブロック先まで見て Deferred に積み、次ブロックで適用。Play は CLOCK_SAMPLE でトラック上の時刻のサンプルから。即時再生の開始ブロック内（時刻 < blockFrames）のアクションは ApplyCommands が CreatePlayback 直後に適用（Play の子・Playback スコープの Parameter・スナップショット）、StopEvent は RunEventLogic の前段（0b）で親を先に開始させた後に適用。親が開始できなければ子は取り消し（kPbCancelled）。ポーズ中は時刻が進まず、停止後は発火しない。Parameter はアクションを持つ Playback のスコープ（ACTN に列が無いため）。スナップショットは即時（時間の列が無い）。
- D-146 snd_play_fade: Play.flags の上位 16 ビットにミリ秒。Playback のフェード（logic.fadeGain）を 0 から直線で上げ、停止はいまの値から下げる。初回ブロックはフェードの開始値から（EvaluateDspParams、Render の gain0）。
- テスト（core_sequence2_test.cpp）: CoreSequence2.WeightedRandomFollowsTheWeights / AllZeroWeightsAreEquallyLikely / WeightedRandomIsDeterministic / RandomNoRepeatSkipsZeroWeights / SwitchScopesAndDefaultClip / ComboWindowBoundaryIsInclusive / SequentialStateIsPerEventAcrossPlaybacks / ShuffleWithoutLimitIsAPermutationPerRound / ShuffleOfTwoNeverRepeats / RandomVolume・Pitch・Pan・Start・FilterStaysInRange / LoopCountPlaysNPassesAndCutsAtTheEnd / LoopCountWithLoopPointsCountsTheLoopRegion / LoopCountZeroIsEndlessAndOneIsOnePass / ClipFadeOutRampsToTheEnd / LayerCrossfadeIsEqualPowerInTheOverlap / LayerCrossfadeFollowsTheRampWithoutClicks / LayerCrossfadeTouchingRangesSwitchWithTheShortFade / NestedEventUsesItsOwnRoutingAndTheParentClipGain / NestedEventsDeeperThanTheLimitDoNotPlay / ActionPlayStartsAtTheActionSample / ActionSetParamWritesThePlaybackScope / ActionSnapshotAndStopApplyInTheirBlock / ActionsWaitWhilePausedAndNeverFireAfterAStop / ActionsOfARefusedPlayAreCancelled / ActionsOfAScheduledPlay / PlayFadeRampsLinearlyFromZero / StopDuringTheFadeInFadesFromTheCurrentLevel。

### C. 通知（2.1）
- 終了理由: CoreNotify2.EveryEndReasonExactlyOnce（COMPLETED / STOPPED / REJECTED EVENT_LIMIT / NOT_LOADED / STOLEN / DISCARDED、各 1 回）、CommandOverflowEndsOnce、NestedAndActionPlaybacksEndOnceForTheParent（入れ子は親の 1 回、アクションの子はワンショットで通知なし）。
- マーカー: 受け口 `core/src/markers.hpp` ForEachMarkerCrossing（純粋関数、ループの周回ごと）と Render の PostMarkers（実ボイス、EventType::Marker、on_marker）。列ができるまでの入口 `CoreDesc::clipMarkers`（素材 id → 位置と id の配列）。テスト CoreNotify2.MarkerCrossingsOfAPlainAndALoopingClip / ClipMarkersReachOnMarker。
- 拍: 追加なし（music の既存テストで足りると判断）。

### D. 解析
- D-147 `core/src/analysis.cpp/.hpp`: メーター 0 = 出力（マスター）、1..CoreLimits.maxMeteredBuses（既定 8）= kBusMeter のバス（Render が最初に見た順に割り当て、バスが消えたら解放）。ラウドネス（短期・統合、dsp の BS.1770）は計測するバスだけ、測らないバスは **NaN**。状態は Create で確保。
- D-148 `snd_get_spectrum`: Render は各メーターのチャンネルごとに直近 fftSize（CoreLimits.spectrumFftSize、既定 1024）フレームのリングを持つだけ。Publish は読み手が居る間（読むたびに 256 ブロック延長）だけリングを core 専用のトリプルバッファ（notify.hpp と同じ seqlock 手順、AtomicCopyIn/Out）に写し、Update が入れ替える。FFT は読み手のスレッドで、Create で確保した作業領域と読み手用ミューテックスで（オーディオスレッドは FFT もロックもしない）。関心が切れたら「データ無し」のフレームを 1 つ出す。
- テスト（core_analysis_test.cpp）: CoreAnalysis.PeakAndRmsPerBusAndChannel / LoudnessPerMeteredBus / SpectrumOfAKnownTone / SpectrumInterestExpires / MeteredBusesBeyondTheLimitHaveNoLoudness。

### E. 決定論・RT・確保・性能
- core_g4_system_test.cpp: CoreG4System.SameScriptSameBits（シャッフル・重み・アクション・入れ子・レイヤー・N 回ループ・フェードイン・解析を含む台本、2 回でビット一致、並列と差 0、スペクトルとメーターも一致）、NoAllocationAfterInitialisation、RenderPathHasNoRtViolations。
- core_g4_load_test.cpp: CoreG4Load.FourTimesTheLimitWithStealModes（512 要求を 4 種の奪い方のグループへ）、Real128VoicesWithLoudnessAndSpectrum（Release で 30% を強制）。

## 2. 決定の提案（番号は受け入れ側）
1. 「戻らない」の終了理由は SND_END_STOLEN / VOICE_LIMIT。Playback のどれか 1 ボイスでもそうなって消えたら STOLEN。
2. 「先頭から」は 0 ではなくそのボイスの startFrame（Clip のランダム開始位置）へ戻す（N 回ループの数え方と揃うため）。素材の印による source の「先頭から」は 0。
3. 重み: ≤0 は選ばない、全部 0 なら等確率（代案: 鳴らさない）。
4. loopCount 1 は「既定」でループ点のある素材は無限ループのまま（従来互換）。N≥2 はループ区間の N 周で、ループ点の後ろ（テール）は鳴らさない（source の detail.hpp の案「最後の周は終わりまで」とは違う）。
5. CLIP.fadeOutMs はクリップの終わりへ向かうフェード（停止のフェードは EVNT.fadeOutMs のまま）。終わりの無いクリップには効かない。
6. レイヤー: 等パワー、重なりが無い跳びは短いフェードで制限、接する値は上の Clip。
7. 入れ子: Event 単位の上限・再トリガーは子に掛けない（子は親のインスタンスの一部）。カテゴリのポーズは親 Playback のカテゴリで決まり、音量・ミュートは各ボイスのカテゴリ。
8. アクション: 開始ブロック内のアクションは即時再生だけ ApplyCommands で前倒し。予約付き開始では最初のブロックのアクションが最大 1 ブロック遅れる（Play は次ブロックの先頭から）。
9. snd_play_fade のミリ秒丸め（最大 65.535 s）。フェードの傾きはブロック単位の値を線形補間（最後の部分ブロックで 1 に届く）。
10. 解析: 測らないバスのラウドネスは NaN（代案: snd_meter.reserved に印）。スペクトルは読み手のスレッドで FFT、関心が無いときは写さない。上限は config でなく CoreLimits（maxMeteredBuses 8、spectrumFftSize 1024、maxActionChildren 64）。チャンネル容量は出力チャンネル数（最低 2）。

## 3. 契約の変更点
- `contract/internal/include/snd/internal/commands.hpp`: 定数 2 つだけ追加（`kPlayFadeInMsShift = 16`、`kPlayFadeInMsMask = 0xFFFF0000u`）。**Play には予約欄が無い**ので、Play.flags の bit16..31 をフェードインのミリ秒に使った（bit2..15 は 0 のまま）。新しいコマンド種別なし、構造体の大きさ（kWireSize<Play> == 80 の static_assert のまま）と既存の欄の意味は不変。flags=0 の上位ビットが 0 なら従来とビット単位で同じ。他の予約欄（SetParam/SetSwitch/SetBusVolume 等）は使っていない。
- `schema.hpp` SND_SCHEMA_READS 末尾: `R(RunEventLogic, Voice_audibility, Previous)`、`R(Render, Voice_playbackSlot, Current)`。
- `sonus.h` 末尾: `snd_play_fade(sys, event, emitter, when, fade_in_seconds)`、`snd_get_spectrum(sys, bus, channel, out_magnitudes, bin_count)`。`sonus.hpp` 末尾: PlayFade / GetSpectrum。`api/compose/c_api.cpp` 末尾に配線。
- snd_config・列挙・bank_format・notify.hpp・source_if.hpp は不変。
- core 内部（契約外）: CoreLimits 末尾 3 項目、CoreDesc 末尾 clipMarkers、System::PlayFade / GetSpectrum。

## 4. 持ち場の外への依頼
- livelink（C）: `command_check.cpp` の Play / PlayOneShot の flags 検査に bit16..31（フェードインの ms）を許す。記録・再生で snd_play_fade が拒否されるため。
- bank-format / buildtool: (a) 効果音マーカーの列: ASST に `markerFirst` / `markerCount` と新しい表（例 AMRK: frame U64, id U32）、通知は既存のイベントリング（EventType::Marker）で足りる。それまでは CoreDesc::clipMarkers。(b) ACTN に `scope`（0 Playback / 1 Emitter / 2 Global）と `timeMs` 以外の `fadeMs`（スナップショット適用の時間）。(c) TRAK.flags に文書上の意味が無いので未使用（意味を決めてもらう）。(d) CLIP.loopCount の 1 とループ点の扱いを文書に。
- source: SourceStartRequest にループ回数の欄（detail.hpp の bit16..31 案）を契約にするなら、core のカウントと合わせる必要がある（現状 core は渡していない）。VoiceRenderStatus にエラーの印が無いので、再生中のソースのエラーで SND_END_ERROR を出す経路が無い。
- api: snd_play_fade / snd_get_spectrum の api テスト（c_api の末尾配線のみで、api/test は書いていない）。

## 5. 構成ごとの結果（gcc 13 / clang 18）
- 8d9f6ba: gcc Debug 214 件通過。Release 214 件通過、repeat20（core_g4_test, core_g4_system_test）通過。
- 26c1136: clang Debug 全件通過、ASan/UBSan 全件通過、TSan で core_g4_system_test が失敗（後述）。
- ee1575f: TSan 214 件通過、TSan repeat20（-j1）通過、Release 214 件通過、Release repeat20 通過。gcc Debug 全件・clang・asan の再実行は下の追記を参照。
- MinGW（x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror、win32 版で全部通過、posix 版は不要）: 変えた/足した core/src の .cpp 11 本、c_api.cpp、テスト 6 本。sonus.h は C11 で通過。
- 性能（Release、CPU 時間/ブロック、予算 5333 µs）: 既存 CoreLoad 128 ボイス 675 µs（12.7%, RT0）/ 854 µs（16.0%, RT2）。新 CoreG4Load 128 ボイス＋kBusMeter 8 本のラウドネス＋毎ブロックのスペクトル読み: 585 µs（11.0%, RT0）/ 616 µs（11.6%, RT2）、wall p99 787 / 426 µs。4 倍要求（4 種の奪い方）: real 128、virtual 128、playbacks 256 で有界。dsp は変えていない（SIMD 構成は不要）。

### TSan の失敗の切り分け（ee1575f で修正）
- 失敗は CoreG4System.SameScriptSameBits の直列どうし・直列と並列の不一致。TSan の報告（データ競合）は 0 件。TSan では 1 ブロックが予算を超え続け、負荷 LOD が台本の途中で下がり（実ボイス上限・リサンプラの軽量化）、そのブロックが実行時間で変わるため出力が変わっていた。
- 根拠は設計書 2.5「条件付きで保証すること: 波形のビット一致は、同一ビルド・同一SIMD経路・**負荷LOD無効**・ストール無しのときだけ」。既存の CoreDeterminism.SameScriptSameBits（core_system_test.cpp:173-177）も `tuning.lod_down_blocks = 0` と `rt_spin_microseconds = 200000` で回しており、同じ扱いに揃えた。テストは緩めず、終わりに `stats.lod_level == 0` と `late_chunks == 0` を確かめる行を足した（前提が崩れたら失敗する）。

## 6. できなかったこと・既知の制限
- 「続きから」の戻りは、ソースレートのボイスでリサンプラの先読み分（Sinc で約 16 フレーム）先に進む（仮想化時の位置が先読み込みのため。既存の挙動。テストは 0..17 フレームを許して明記）。
- マーカーの時刻も同じ理由で最大 16 フレーム早い。仮想ボイスではマーカーを出さない。
- N 回ループの切り位置は 32.32 固定小数のリサンプラ位置と倍精度計算の丸めで ±1 フレームあり得る（比 1 では一致）。テストの TestSampleKind は仮想中の loopCount を数えないので、仮想ボイスの N 回ループは実ソースでのみ正しい（テストなし）。
- 開始ブロックの StopEvent 以外（スナップショット）は親が上限で拒否されても前倒しで効いてしまう。Play の子は取り消す。
- 予約付き開始の最初のブロックのアクションは最大 1 ブロック遅れ。
- SND_END_ERROR を出す状況が core に無い（上記依頼）。
- ワンショット・PlayAt・拍のテストは足していない（既存で足りると判断）。

## 7. 既存のテストで新しい仕様と食い違うもの
- CoreVoice.LimitGroupNeverRejectsAndOldestSteals（core_voice_test.cpp:149）はグループに奪われた Playback を SND_END_COMPLETED としている。提案としては STOLEN / LIMIT_GROUP が「奪われた」に合うが、テストを守るため変えていない（受け入れ側の判断待ち）。

## 8. ブランチの先頭
ee1575f（本報告の後の再実行結果は下に追記）。

### 追記（ee1575f の再実行、flock 下）
gcc Debug 214 件通過、TSan 214 件通過・TSan repeat20（-j1, core_g4_system_test / core_g4_test）通過、clang Debug 214 件通過、ASan/UBSan 214 件通過、Release 214 件通過・repeat20 通過。Release の負荷: CoreG4Load 128 ボイス＋メーター 8＋スペクトル 585 µs (11.0%, RT0) / 616 µs (11.6%, RT2)。
ee1575f の根拠: 設計書 2.5「条件付きで保証すること: 波形のビット一致は、同一ビルド・同一SIMD経路・負荷LOD無効・ストール無しのときだけ」。既存 CoreDeterminism.SameScriptSameBits（core_system_test.cpp:173-177）も `lod_down_blocks = 0` / `rt_spin_microseconds = 200000` で回している。

---

## 差し戻し 1（受け入れ側の判断への対応）

先頭（作業中）: af89e2e（親 8f00b4c = テスト担当のコミット）。テスト担当のファイル（indep_g4_*）は変えていない。

### 直したこと
1. **奪われた再生の終了理由**: core は変えていない（下に依頼用の差分）。
2. **フェードイン（D-146）**: Playback のフェードをやめ、ボイスごとにサンプル単位。ゲイン = そのボイスの開始フレームからの「再生した」フレーム数 / フェードのフレーム数（`VoiceStatic.fadeInFrames`、`RenderPriv.voicePlayed`、`ItemInput.fadeIn*`、PostProcessItem）。予約付き開始のブロック途中からでも 0 から。一時停止（とその停止フェード中）・停止（停止フェード中）は進まない（RunEventLogic が `kVsFadeInHold` を立てる）。仮想の間は進む（Render (f)）。停止はいまの値から下がる（フェードインを止めたまま停止のフェード）。前の Render の gain0 の変更と EvaluateDspParams の開始値の細工は戻した。
3. **CLIP.fadeOutMs**: クリップの最後の F フレーム（end−F … end−1）で 1→0、最後のフレームで 0。比 ≠1 でも位置（pos0＋比のランプ）からサンプルごとに値を出し、ブロックの線形補間をやめた（`ClipTimeline`）。
4. **マーカーの時刻**: その素材フレームが出力に出るフレーム（pos(i) ≥ マーカー の最初の出力フレーム、先読みは PendingOffset で引く）。比 1 で正確、比 ≠1 で ±1。**仮想の間のマーカーも通知**（Render (f)、進んだ量からの時刻）。「続きから」の先読み分も直した: 実ボイスの最後の描画時の先読み量を `voiceLookahead` に残し、仮想で進める前・再割り当ての前に位置から引く（`TakeLookahead`。ループの折り返しをまたぐ場合も）。
5. **アクションの時刻（D-145 改）**: 実装をやり直した（sys_event.cpp `TakeActions` / `RunPending` / `ActionsAtStart` / `ParentState` / `DropPending`、sys_apply.cpp `ApplyFirstBlockActions` / `CommitTentativeSnapshots`）。
   - 時刻は親のトラック時間（`PlaybackPriv.trackTime`、一時停止中・停止後は進まない）。
   - Play: 1 ブロック先を見て子を作り、CLOCK_SAMPLE で時刻のサンプルから。
   - StopEvent: 時刻を含むブロックで、対象を時刻のフレームからの時刻付き停止（`BeginStop(at)`、予約付き停止と同じ）。
   - SetParam / ApplySnapshot: 時刻以降に始まる最初のブロックの先頭から（時刻より前には効かない）。
   - 先に取ったアクションは `EventPriv.pending` に置き、そのブロックの始め（コマンド適用の後）に親を確かめる。親が一時停止（要求・カテゴリ・状態）なら取り消してトラック時間を戻す（再開後にまた取る）、停止・消滅・停止予約なら取り消す。Play の子は kPbCancelled で鳴らずに終わる。
6. 一時停止・停止とアクション: 上のとおり。
7. **予約付き開始の最初のブロック**: 開始時刻が分かる予約（CLOCK_SAMPLE、DELAY_US は UpdateTransport と同じ式）は、`ApplyPriv.playbackStartClock` を使って開始の前のブロックで先に取る（開始がそのブロック内なら ApplyCommands で）。実際の開始時刻が違えば取り消して遅れの経路に戻す。音楽時刻の予約は従来どおり最大 1 ブロック遅れ（下記）。
8. **拒否された再生のアクション**: 種類を問わず効かない。最初のフレームのスナップショットは「仮の重み」（`ApplyPriv.tentative`）として EvaluateDspParams がこのブロックだけ使い、再生が始まらなければ RunEventLogic が取り消し、始まったものだけ次の ApplyCommands が確定する。Play の子・Parameter（自分のスコープ）・残りは開始後にだけ。
- 仕様の穴で実装を変えたもの: 「戻らない」は一時停止中（要求・状態のどちらも）に終わらせない（RunEventLogic が印を消し、再開後に SelectVoices がまた判定）。アクションの子が上限（maxActionChildren、キュー 256）を超えたら `SND_ERR_LIMIT_REACHED`（object = 親）を 1 ブロック 1 件。入れ子の子の乱数は `MixU32(Playback.seed ^ 子の EVNT.id ^ 深さ)`（最上位は従来どおり）。メーターの枠はバスの行の順（Bus 表の登録順）に、鳴っているかを見ずに割り当て（`AnalysisMaintain`）。sonus.h: snd_get_meter に NaN の注記、snd_get_spectrum の NOT_READY の説明を直した。

### 項目 1 の差分（依頼用。core は未変更）
```diff
--- a/core/src/core_impl.hpp
+++ b/core/src/core_impl.hpp
@@ enum PlaybackPrivFlags
   kPbActionStop = 1u << 12,
+  kPbVoiceStolen = 1u << 13,   // a voice was taken by a limit group (4.4)
 };
--- a/core/src/sys_event.cpp
+++ b/core/src/sys_event.cpp
@@ bool CreateVoice(...)   (the limit group's steal)
       StopVoice(c, victim, im.cfg.shortFadeSeconds);
       const std::uint32_t vd = base::rows::DenseOf(t.Voice.rows, victim);
       // The stolen voice leaves the group now (its fade still renders).
       if (vd != kNoSlot) {
+        const SlotIndex vpb = SND_W(Voice_playbackSlot)[vd];
+        if (vpb < im.cfg.maxPlaybacks) Priv(im, vpb).flags |= kPbVoiceStolen;
         SND_W(Voice_limitGroup)[vd] = kNoSlot;
@@ RunEventLogic, step 3 "All voices gone"
       const bool dropped = !stopping && !refused && (pp.flags & kPbVoiceDropped) != 0u;
+      const bool stolen = !stopping && !refused && (pp.flags & kPbVoiceStolen) != 0u;
       if (!EndPlayback(c, d,
-                       stopping ? pp.endReason : (refused ? SND_END_REJECTED : (dropped ? SND_END_STOLEN : SND_END_COMPLETED)),
-                       stopping || refused ? pp.endDetail : (dropped ? SND_END_DETAIL_VOICE_LIMIT : SND_END_DETAIL_NONE))) {
+                       stopping ? pp.endReason
+                                : (refused ? SND_END_REJECTED : (stolen || dropped ? SND_END_STOLEN : SND_END_COMPLETED)),
+                       stopping || refused ? pp.endDetail
+                                           : (stolen ? SND_END_DETAIL_LIMIT_GROUP
+                                                     : (dropped ? SND_END_DETAIL_VOICE_LIMIT : SND_END_DETAIL_NONE)))) {
--- a/core/test/core_voice_test.cpp   (既存テスト、書き換えの許可が要る)
-  SND_CHECK(h.ends.Has(q[0], SND_END_COMPLETED));  // its only voice was stolen (short fade)
+  SND_CHECK(h.ends.Has(q[0], SND_END_STOLEN, SND_END_DETAIL_LIMIT_GROUP));  // its only voice was stolen
```
（多ボイスの再生で 1 ボイスだけ奪われ、残りが自然に終わった場合も STOLEN になる。「最後に消えたボイスが奪われたときだけ」にするなら、kPbVoiceStolen を「残りのボイスが 0 になった時点で、最後のボイスが奪われたもの」だけで立てる。）

### 契約の変更点（差し戻し 1 の差分）
- `sonus.h`: コメントのみ（snd_get_meter の NaN、snd_get_spectrum の NOT_READY）。関数・型は不変。
- schema / commands / config / 列挙: 変更なし（Play.flags bit16..31 の使い方は前回のまま）。
- core 内部: PlaybackPriv（trackTime / actionStart / actionStopClock）、ApplyPriv（tentative、playbackStartClock、actionChildWarned）、EventPriv（pending、actionWarned）、VoiceStatic（fadeInFrames、kVsFadeInHold）、ItemInput（fadeIn*）、RenderPriv（voicePlayed、voiceLookahead）。

### 自分のテストの変更（決定に合わせた）
- CoreVoice2.VirtualReturnContinueKeepsTheTimeline / VirtualReturnTheAssetFlagWins: 許容 0..17 → 0（先読み分を直したため）。
- CoreNotify2.ClipMarkersReachOnMarker: 許容 −17..0 → 0。
- CoreSequence2.ActionSetParamWritesThePlaybackScope: 1000 フレームのアクションは 1024 のブロックから（1000・1023 では前の値、1300 で新しい値）に変更。
- 追加 core_g4_fix1_test.cpp: CoreG4Fix1.FadeInHoldsWhilePaused / FadeInRunsWhileVirtual / MarkersWhileVirtual / NeverIsNotEndedWhilePaused / ActionChildrenOverTheLimitWarn / NestedEventsHaveTheirOwnRandomSequence / MetersInTheOrderOfTheBusRows / ClipFadeOutReachesZeroOnTheLastFrame。

### テスト担当のテストで決定と食い違うもの（直していない）
- `IndepG4Action.SetParamActsOnItsOwnPlayback`（indep_g4_seq_test.cpp:1127）: 960 のアクションは決定どおり 1024 のブロックから効き、そのブロックの中でゲインが snd_set_param と同じくランプするので、1279 は 0.0518（許容 1e-3 を 0.0008 超える）。次のブロック（1280 以降）では 0.05。テスト担当の書き換えが要る。
- `IndepG4Steal.Defect_StolenPlaybackEndsAsCompleted`: 項目 1 のとおり失敗のまま。
- 「テスト担当が直す」SpecHole（MarkersWhileVirtual、NeverForAVoicePausedWhileVirtual、PauseDuringTheFadeIn など）は現状でも通過。

### 既知の制限（差し戻し 1 後）
- 音楽時刻（NEXT_BAR 等）の予約付き開始では、最初のブロックのアクションは最大 1 ブロック遅れる（開始時刻を前もって知る経路が core に無い）。Play の子は次のブロックの先頭から。
- 一時停止から再開したブロックのうちに来るアクションは、そのブロックの終わり（次のブロックの先頭）に回る（最大 1 ブロック）。
- 中身が「アクションのみで Clip なし」の Event は、開始時にボイスが 0 で COMPLETED になり、2 ブロック目以降のアクションが走らない（従来からの振る舞い。報告のみ）。

### 構成ごとの結果（af89e2e、各コマンドを flock /tmp/snd-heavy.lock で包んで 1 つずつ）
- gcc Debug / clang Debug / ASan+UBSan / TSan / Release: 218 件中 216 件通過。失敗は indep_g4_seq_test と indep_g4_voice_test の 2 実行ファイルだけ（サニタイザの報告 0 件）。
- 失敗の照合（各 20 回、gcc Debug・Release・TSan）: indep_g4_voice_test は毎回 `Defect_StolenPlaybackEndsAsCompleted` だけ（項目 1）、indep_g4_seq_test は毎回 `IndepG4Action.SetParamActsOnItsOwnPlayback` だけ（決定 5 と食い違う期待、上記）。それ以外の失敗 0。clang / ASan は全件 1 回で、失敗はこの 2 実行ファイルのみ。
- repeat20（TSan は -j1、Release は -j2）: core_g4_test（core_voice2 / sequence2 / notify2 / analysis / g4_fix1）、core_g4_system_test、core_g4_load_test、indep_g4_analysis_test、indep_g4_system_test すべて 20 回通過。
- MinGW（x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror）: 変えた core/src 11 本、c_api.cpp、テスト 4 本すべて通過。
- 決定論（CoreG4System.SameScriptSameBits、indep_g4_system_test）、確保ゼロ、RT 検出ゼロは上の全構成で通過。
- 性能（Release、CPU 時間/ブロック、予算 5333 µs）: CoreG4Load 128 ボイス＋kBusMeter 8 本＋毎ブロックのスペクトル 381 µs（7.1%, RT0）/ 613 µs（11.5%, RT2）、wall p99 527 / 382 µs。既存 CoreLoad 128 ボイス 533 µs（10.0%）/ 717 µs（13.4%）。4 倍要求 real 128。
- 先頭: af89e2e（push なし）。

## Cloudでの組④の復元と実装担当の実測

統合先 `c86901f` と組④の公開先頭 `50555f3` を作業ブランチで合流した。組②のボイスFXを終了・停止のフェード前に実行し、バス解析とサイドチェーンの保存にはフェーダー後の信号を渡す。スキーマの Render による Voice_playbackSlot 読み出しは1宣言にまとめた。`099392f` は最後のボイスが奪われた理由の実装と、独立担当が作成した承認済みの既存期待を一緒に収める。修正の適用前に公開済みの終了理由変更を打ち消すコミットを置いている。

`2779373` は固定帯域のSinc係数の隣接位相差を初期化時に一度だけ求める。元のfloat値同士で差を取り、描画時の係数の積と和、畳み込みの加算順を保つ。比率4超の可変帯域処理は従来どおり。実装担当が GCC 14.2 Release 相当 (-O3 -ffp-contract=off) で旧・新を独立にコンパイルし、16比率（1/256〜4、全11帯域）、モノ／ステレオ、257フレーム×6ブロックで全74,016サンプルを比較した。SSE2・scalar・AVX2/FMAの各経路で旧新のバイト列は一致した。

`df814a7` の GCC 14.2 Release で core_g4_load_test を新規ビルドし、実装担当が単独で5回測定した。旧版は同じビルドの resampler.cpp.o だけを `fc4b20f` のソースから同じコンパイルオプションで再生成してリンクした。128実声・8メーターバス・毎ブロックスペクトル取得、予算5333.3 µs。旧新とも全5回で2ケース通過、毎回128実声・スペクトル298回。

| Sinc・RTワーカー数 | CPU µs/block（測定順） | 中央値・予算比 |
| --- | --- | --- |
| 旧・0 | 482.3、495.1、439.0、451.9、538.0 | 482.3 µs・9.0% |
| 新・0 | 432.0、474.6、434.8、438.7、441.8 | 438.7 µs・8.2% |
| 旧・2 | 670.6、510.9、674.5、507.6、632.2 | 632.2 µs・11.9% |
| 新・2 | 659.3、578.0、610.8、596.2、678.6 | 610.8 µs・11.5% |

受け入れ担当の9構成・反復・最終統合・横断台本は別計測。
