# 段階3 カバレッジ対応表（設計書 7 章 × 現在のコード）

- 対象ブランチ: `int/s7-coverage`（出発点 ff5df53）
- 作成: 2026-11-20、セッション B の調査（コードを読んで判定。ビルド・実行はしていない）
- 判定の語: **実装済み** = 設定意図どおりに動く経路がコードにある（テストの有無は別の列）／**一部だけ** = 動く部分と動かない部分がある／**未実装** = 経路が無い／**B の対象外** = 他セッションまたは統合済みの持ち場
- 担当の語: **B** = このセッション（core / dsp）、**A** = physics、**D** = 拡張（Granular・Patch・コンボリューション・タイムストレッチ・ルーム）、**統合済み music** = テストを足すだけ、**持ち場の外** = source / bank / buildtool / livelink / api / platform（依頼 `requests/B-*.md` が要る）
- 「行 → テスト名」の列は、いま有るテストだけを書いた。B の作業で足したテストは、組ごとの受け入れの後にここへ追記する。
- 行番号は ff5df53 時点。

## 0. 先に分かった横断的な事実

- 公開 API（`api/compose/c_api.cpp`）には `SND_ERR_NOT_IMPLEMENTED` / `NOT_SUPPORTED` を返す関数は無い。足りないのは「関数そのものが無い」か「関数はあるが core が一部の値を読まない」のどちらか。
- core のテストは公開 C API ではなく `snd::core::System` を直接使う（`core/test/harness.hpp` の `Harness`）。バンクはテストの中で `TestBank::Build()`（`core/test/test_bank.hpp`、buildtool の `BankWriter` の薄い包み）で組み立てる。
- 列があるのに core が読んでいない（または一部しか使わない）バンクの列:
  - `EVNT.curveReverbSend`（どこからも読まれない）
  - `EVNT.virtualReturn`（`core/src/live_link.cpp:140` の編集表だけ。仮想ボイスの戻り方は `ASST.flags` の `kAssetVirtualRestart/Kill` から `core/src/bank_registry.cpp:125` で決まる）
  - `CURV.compose`（読まれない。合成は `CurveTarget` ごとの固定規則、`core/src/sys_dsp.cpp:118-124`）
  - `CURV.target` の `Send` / `EffectParam` / `ReverbSend`（`default: break`、`core/src/sys_dsp.cpp:123`）
  - `PARM.scopeMask`（`ParamDef` に入るだけ、`core/src/bank_registry.cpp:450`。強制していない）
  - `SNPV.targetKind` 1（エフェクトのパラメータ）・2（センドの量）と `SNPV.paramIndex`（`core/src/sys_dsp.cpp:258` で読み飛ばす）
  - `CLIP.fadeOutMs`（読まれない）、`CLIP.loopCount` は「1 以外 = 無限ループ」扱い（`core/src/sys_event.cpp:558`。N 回ループにならない）
  - `CATG.flags` の `kCategoryMuteOnFocusLoss`（読まれない。`api/compose/compose.cpp:156` に「core にまだ無い」と注記）
  - `BUSS.flags` の `kBusMeter`、`BUSS.lodPriority`、`EFFX.flags`（読まれない）
- `Config.listener_mode` は `core/src/create.cpp:116` で `Cfg` に写すだけで、どのシステムも読まない（常に最寄りのリスナー）。
- Emitter の遮蔽・閉塞（`snd.occlusion` / `snd.obstruction` の Emitter スコープ Parameter）は `Emitter.occlusion` 列まで来る（`core/src/sys_params.cpp:54-70`）が、EvaluateDspParams は読み出しを宣言しているのに使っていない。
- 予約 ID は 3 個だけ（`SND_ID_RESERVED_FIRST..LAST` = `0xFFFFFFFC..0xFFFFFFFE`、`contract/public/include/snd/id.h:31-32`）。`0xFFFFFFFC/FD` は再生単位の音量・ピッチ（D-104）、`0xFFFFFFFE` は `CURV.input` の距離。新しい予約 ID を足す余地は無いので、新しい設定を Parameter で運ぶなら `snd.occlusion` と同じ「名前のハッシュ」を使う慣習になる（予約 ID ではないので `ParamOk` の規則に掛からない）。
- コマンドの予約欄のうち、ライブリンクの形の規則（`livelink/src/command_check.cpp`）が値を検査していないもの: `SetParam.reserved[3]` / `reserved2`、`SetSwitch.reserved[3]` / `reserved2`、`SetBusVolume.reserved`、`ApplySnapshot.reserved`、`SetCategoryVolume.reserved`、`SetListener.reserved`、`Stop.reserved`、`SetScheduledStop.reserved`、`PlayUserBuffer.reserved`。`PauseCategory.paused` / `MuteCategory.muted` は 0/1 しか通らない（`command_check.cpp:169-173`）。予約欄を variant に使うときは、規則表の説明と検査（C の持ち場）にも足すよう依頼する（D-88 / D-89 と同じ扱い）。

## 1. 7 章の表の行ごとの判定

| 行 | 判定 | 根拠（ファイル:行、関数） | 既にある機能テスト | 足りないもの | 担当 |
| --- | --- | --- | --- | --- | --- |
| 再生の単位（Event／Track／Clip） | 一部だけ | `core/src/sys_event.cpp:427-590` `StartEventTracks` / `StartClip` / `CreateVoice` | `CoreBasic.EndsWhenTheSampleEndsExactlyOnce`, `CoreBasic.ConstantMonoCenterIsMinus3dB`, `IndepCmd.FiniteSourceCompletesOnceWithAllFrames`, `CoreEndToEnd.ProjectToSpeakerThroughCore` | `CLIP.loopCount` が N 回ループにならない（1 以外は無限、`sys_event.cpp:558`）。`CLIP.fadeOutMs` 未使用。`TRAK.flags` 未使用 | B（N 回ループは source の `SourceStartRequest` にループ回数の欄が無いので、core で回数を数えて `Release` するか、source への依頼） |
| ランダム・順送り | 実装済み | `sys_event.cpp:457-521`（Polyphonic / Random 重み付き / RandomNoRepeat / Sequential / Shuffle ≤64 / Switch / ComboSequential / LayerCrossfade） | `CoreSequence.SequentialCycles`, `CoreSequence.ShuffleCoversEveryClipBeforeRepeating`, `CoreSequence.RandomNoRepeatNeverRepeats`, `CoreSequence.ComboAdvancesOnlyInsideTheWindow`, `IndepMix.DeterministicWithRandomClips` | 重み付き Random（`CLIP.weight`）の分布のテストが無い。Shuffle は 64 クリップまで（`sys_event.cpp:491`） | B |
| ランダム幅 | 実装済み | `sys_event.cpp:533-560`（音量・ピッチ・パン・開始位置・フィルタ。乱数は Playback のシード、`NextRandom`） | 範囲の機能テストは無し（`IndepMix.DeterministicWithRandomClips` と `CoreDeterminism.SameScriptSameBits` が決定論だけを見る） | 各幅が範囲内に収まり、幅 0 で揺れないことのテスト | B |
| 値による切替（スイッチ型 Event） | 実装済み | `sys_event.cpp:508-520`（`TRAK.layerParam` をスイッチグループ ID に流用、値 0 のクリップが既定）、`core/src/params.hpp:110` `SwitchValueOf`（Playback → Emitter → Global） | `CoreSequence.SwitchFollowsTheSwitchValue` | 3 スコープの優先と既定クリップのテスト | B |
| 値によるパラメータ制御（Parameter 3 スコープ＋多点カーブ） | 一部だけ | スコープ: `params.hpp:85` `ParamValueOf`。ランプ: `sys_params.cpp:23-43` `Step`。多点カーブ（shape 付き）: `core/src/core_util.cpp:84-104` `EvaluateCurve`。合成: `sys_dsp.cpp:107-125` | `CoreMix.ParameterScopesPlaybackOverEmitterOverGlobal`, `CoreMix.ParameterRampRate`, `CoreMix.OscillatorFollowsPitchCurveOnAParameter`, `CoreMix.PanCurveMovesTheMonoSourceRight`, `IndepMix.ParameterScopesComposePerPlayback`, `IndepMix.ParameterRampFollowsTheRate`, `IndepMix.VolumeCurvesAddInDb` | `CURV.compose` を読まない。4.3 の「その他は乗算」が無い（Volume/Pitch/Lowpass/Highpass/Pan 以外は捨てる）。`Send` / `EffectParam` 対象が効かない（どのセンド・どのエフェクトの何番かを指す列が `CURV` に無い）。カーブは Event にしか付かない（バスやエフェクトへの AISAC 相当が無い）。`PARM.scopeMask` を強制しない。`CPNT.shape` の多点・指数のテストが無い | B（Send / EffectParam の宛先の列はバンク形式の追加 → 依頼） |
| 他の再生の操作（アクショントラック、入れ子 Event） | 一部だけ | アクション: `sys_event.cpp:788-837` `RunActions` → 次ブロックの `core/src/sys_apply.cpp:948-990` `ApplyDeferred`。入れ子: `sys_event.cpp:549-551`（深さ ≤ 4、`core_impl.hpp:47`） | なし | アクションは次ブロックで適用（最大 1 ブロック遅れ、サンプル精度でない）。`SetParam` アクションは Playback ハンドルを積むのに Global スコープで書く（`sys_apply.cpp:978-980`）。入れ子 Event はその Event 自身のバス・カテゴリ・3D・リミットを使わず親のものを使う。機能テストが無い | B |
| 再生間クロスフェード | 一部だけ | 停止フェード: `sys_event.cpp:737-781` `BeginStop`。ポーズ／再開フェード: `sys_event.cpp:1250-1300`。クリップのフェードイン: `sys_dsp.cpp:185-190`。レイヤー: `sys_dsp.cpp:126-140` | `CoreMix.StopFadeRampsToSilenceAndEnds`, `IndepCmd.StopFadeShapeLengthAndStates`, `CoreMix.PauseAndResumeKeepPosition` | レイヤーのクロスフェードは「範囲の外は範囲幅の 10% で直線に落ちる」独自の形（`sys_dsp.cpp:134`）で、テストが無い。`Play` にフェードインの指定が無い（クリップの `fadeInMs` だけ） | B |
| ポーズ（Playback・カテゴリ・全体、ポーズ無視） | 実装済み | Playback: `sys_event.cpp:1250-1300`。カテゴリ・全体（`SND_ID_NONE` = D-30 の全体行）: `sys_event.cpp:50-62` `CategoryPaused`、`1270-1283`。無視: `sys_event.cpp:64-70` `CategoryIgnoresPause`（`kCategoryIgnorePause`、親をたどる）と `kEventIgnorePause` | `CoreMix.PauseAndResumeKeepPosition`, `IndepCmd.PauseKeepsPositionAndResumeContinues`, `IndepCmd.PauseThenResumeInOneFrameKeepsPlaying`, `IndepCmd.StopWhilePausedEndsSilently`, `IndepMix.CategoriesVolumeMutePauseAndAll`, `IndepMix.CategoryPauseHoldsThePosition` | `kEventIgnorePause`（Event 単位）のテスト | B |
| ミックス（Bus＋センド＋カテゴリタグ） | 一部だけ | バス木と依存順: `core/src/bus_graph.cpp:52-162` `MixBuses`、センド（プリ／ポスト）: `bus_graph.cpp:129-148`。カテゴリの音量・ミュート・親: `sys_dsp.cpp:52-68` | `CoreMix.ClipBusAndCategoryGainsMultiply`, `CoreMix.BusVolumeFadeIsLinearInGain`, `CoreMix.PostFaderSendAddsToTheTarget`, `IndepMix.BusVolumeAndHierarchyMultiply`, `IndepMix.PreAndPostFaderSends`, `IndepMix.SixChannelMasterIsDownmixedToStereo`, `CoreBanks.SharedBusIsOneRowAcrossBanks` | カテゴリの**ソロ**（4.6）が無い（API もコマンドも無い）。独立した枝の RT ワーカーでの並列化は無い（バスは直列） | B |
| 状態の切替（スナップショット、重み付き重ねがけ） | 一部だけ | `sys_apply.cpp:673-685`（重みを時間付きで遷移）、`sys_dsp.cpp:243-264`（重み × 値 を dB で加算、複数を足し合わせる） | `CoreMix.SnapshotWeightScalesBusVolume` | バス音量（`targetKind` 0）だけ。エフェクトのパラメータ（1）とセンドの量（2）は読み飛ばす（`sys_dsp.cpp:258`）。2 つ以上の重ねがけのテストが無い | B |
| ダッキング（サイドチェーン＋ルール型） | 一部だけ | `sys_dsp.cpp:213-234`: ルール型（カテゴリで鳴っているボイス数）とサイドチェーン型（入力バスの前ブロックのピークがしきい値を超えたら固定量下げる）、攻撃・解放の 1 次平滑。対象バスのゲインへ dB 加算（`sys_dsp.cpp:265-268`） | `CoreMix.RuleDuckingLowersTheTargetBusWhileTheCategoryPlays` | サイドチェーン型のテストが無い。サイドチェーン型が「しきい値超えで一定量」のオン／オフで、入力レベルに比例しない。dsp のコンプレッサーのサイドチェーン入力（`kEffectHasSidechain`）を core が配線していない（`bus_graph.cpp:120` で `sidechain = nullptr`） | B |
| 3D基本（距離減衰、ドップラー、コーン、spread） | 一部だけ | 距離・方向: `core/src/sys_3d.cpp:28-63` `Compute3D`。減衰: `sys_dsp.cpp:141-148`（`curveVolume` か 1/max(d,1)、`maxDistance` で 0）。コーン: `sys_dsp.cpp:155-166`。spread と 2D/3D ブレンド: `core/src/sys_render.cpp:470-495`（カーブがあるときだけ） | `Core3D.DistanceAndDirection`（1/d と方向のパンだけ） | **ドップラー無し**（`Spatial.dopplerRatio` を常に 1、`sys_3d.cpp:58`）。リスナーの合成方式 Weighted / Split が無い（常に最寄り、`sys_dsp.cpp:27-42`）。遮蔽・閉塞が音に効かない。spread はカーブ以外で設定できない。コーン・距離カーブのテストが無い | B（Event ごとのドップラー係数の列はバンク形式の追加 → 依頼。それまでは `snd_config` 末尾の全体値で） |
| 距離カーブ（音量・フィルタ・リバーブ送り等） | 一部だけ | 音量 `sys_dsp.cpp:143-145`、ローパス `sys_dsp.cpp:150-154`、spread / 3D ブレンド `sys_render.cpp:486-491` | なし | `EVNT.curveReverbSend` を読まない（送り先のリバーブバスを指す列も無い）。距離を入力にした任意の Parameter カーブ（`CURV.input = kInputDistance`）は音量・ピッチ・LP・HP・パンだけ | B（送り先の列は依頼） |
| 範囲・空間構造（範囲音源、リバーブゾーン。ルーム／ポータルは段階6） | 未実装 | Emitter は点だけ（`Emitter` 表に形の列が無い、`schema.hpp:170-177`） | なし | 範囲音源（線・面・箱）とリバーブゾーンの全部。ルーム／ポータルは D | B（範囲音源・ゾーン）、D（ルーム／ポータル） |
| エフェクト（標準 DSP 一式） | 一部だけ | dsp: 14 種（`dsp/include/snd/dsp/effects.hpp:42-102, 122-153`、表 `dsp/src/effects.cpp:133-149`）。リバーブは I3DL2 のパラメータ一式。core: バスのエフェクトチェーン `bus_graph.cpp:108-127`、インスタンスは DSP プール（`core/src/bank_registry.cpp:168-260`） | dsp: `Effects.*`（`GoldenDefaults` ほか 16 本）、`IndepEffects*`、golden `dsp/test/golden/fx_*.sndg` 14 本。core: `CoreMix.BusLowpassEffectAttenuatesHighTones`, `IndepBusEffects11.LowpassAndLimiterParametersFromTheBankAreAudible`, `EndToEnd.BuildLoadDecodeResampleEffectsGolden` | 実行中にエフェクトのパラメータを変える経路が無い（スナップショット・カーブ・API のどれも）。コンプレッサーのサイドチェーン未配線。core を通した各エフェクトの機能テストは LP・リミッターだけ。コンボリューションは D | B（コンボリューションは D） |
| 再生単位のフィルタ（ボイスエフェクト） | 一部だけ | LP / HP は全実ボイスに標準: `sys_render.cpp:118-120, 497-499` → `dsp::VoiceFilterProcess`（12 dB/oct ×2、`dsp/include/snd/dsp/filter.hpp:14-30`）。目標値は `sys_dsp.cpp`（クリップの乱数、カーブ、距離 LP） | `VoiceFilter.*`（dsp）、`CoreMix.OscillatorFollowsPitchCurveOnAParameter`（ピッチ）。core を通した LP / HP の機能テストは無し | 重いボイスエフェクト（リバーブ等を DSP プールの枠で）が無い。Event / Track にエフェクトチェーンを指す列が無い | B（列はバンク形式の追加 → 依頼） |
| 空間化（`IDspEffect` による空間化プラグイン） | 一部だけ | バスのチェーンにプラグインを置ける（`bus_graph.cpp:114-125`、`dsp::ProcessEffectChain`） | `Effects.ChainWithPluginAndBypass`（dsp）、`ApiCreate.PluginTablesAndArguments` | ボイス単位の差し込みが無い（上の行と同じ穴）。プラグインに 3D の情報（方向・距離）を渡す経路が無い（`snd_render_context` に欄が無い） | B（3D 情報の受け渡しは契約の追加が要る → 依頼か D-番号） |
| ボイス管理（リミットグループ、プライオリティ、仮想ボイス） | 一部だけ | 選択: `core/src/sys_voices.cpp:34-169`（優先度 → 可聴度、部分選択、ヒステリシス、予備枠での短いフェード）。グループ: `sys_event.cpp:256-285` `StealVictim`、`330-346`。Event 上限・再トリガー: `sys_event.cpp:680-690`。仮想の戻り方: source 側（`source/src/sample_kind.cpp:407-411`、`stream_kind.cpp:885-888`、`ASST.flags`） | `CoreVoice.KeepsTheLoudestRealVoicesAndSumsThem`, `CoreVoice.PriorityBeatsLoudness`, `CoreVoice.StealFadeIsShortAndMonotone`, `CoreVoice.FourTimesTheRealLimitStaysBounded`, `CoreVoice.LimitGroupNeverRejectsAndOldestSteals`, `CoreVoice.InstanceLimitAndRetrigger`, `IndepMix.VoiceSelectionPriorityThenLoudnessAndFourTimesTheLimit`, `IndepMix.StealHysteresisOfThreeDb`, `IndepMix.DemotedVoiceFadesOverTheShortFade`, `IndepMix.VirtualVoiceAdvancesAndContinuesInTime` | 奪い方「最も小さい」が可聴度でなく「優先度の低い順 → 古い順」の近似（`sys_event.cpp:274-276`）。「最も優先度が低い」のテストが無い。`EVNT.virtualReturn` を読まない（Event 単位の戻り方が効かない）。戻り方 3 種のテストが core に無い | B |
| 時間制御（共通クロック＋音楽時間の予約、開始・終了の時刻指定） | B の対象外（統合済み music） | `core/src/sys_transport.cpp`、`music/` | `CoreMix.SampleAccurateStartAndDelay`, `CoreMix.ScheduledStopCutsAtTheClock`, `IndepCmd.SampleAccurateClockAndDelayStarts`, `CoreMusic.MinimalTransportQuantisesToTheNextBar`, `MusicTransport.*`（14 本）, `MusicSystem.*`, `ApiMusic.*`, `ApiSynth.TransportBeats*` | 対応表に載せるテスト名の整理だけ | 統合済み music |
| ストリーミング | B の対象外 | `source/src/stream_kind.cpp` | `SourceStream*`（source/test）、`OpusPipeline.StreamSeekAndPreload`, `SourceEndToEnd.ResidentCompressedAndStreamThroughCore`, `MusicSystem.SoundPriorityStallHoldsTheTransport` | なし（判定のみ） | 持ち場の外（source） |
| 読み込み形式（6.2 の 4 形式） | B の対象外 | `source/src/sample_kind.cpp`, `pcm_cache.cpp` | `SourceSample.PcmResidentPlaysExactlyAndEnds`, `SourceSample.CompressedAdpcmAndOpusMatchTheCodec`, `SourceSample.DecompressCacheFillsOnceAndIsShared`, `SourceEndToEnd.ResidentCompressedAndStreamThroughCore` | なし | 持ち場の外（source） |
| 動的生成・入力（Callback、UserBuffer、Input） | 一部だけ（B の対象外） | Callback / UserBuffer: `source/src/callback_kind.cpp`, `user_buffer_kind.cpp`。**Input のソース種別は無い**（`source/src/runtime.cpp:47-52` で登録されるのは Sample / Stream / Callback / UserBuffer だけ）。バンクの合成ソース Oscillator / Noise も製品の種別が無い（core のテスト用 `TestOscKind` だけ） | `ApiSynth.*`, `IndepSynth.*`, `ApiPlaybackGain.SeveralReadersOfOneStaticBuffer`, `ApiLifetime.*` | Input、Oscillator、Noise の種別 | 持ち場の外（source / platform）。Granular・Patch は D |
| 再生中の通知（終了、マーカー、拍） | 一部だけ | 終了: `sys_event.cpp:175-231` `EndPlayback` → `core/src/sys_publish.cpp`。拍: `sys_transport.cpp:244-262`（単独）と music。マーカー: music の `MMRK` だけ（`music/src/music_logic.cpp`） | `CoreBasic.EndsWhenTheSampleEndsExactlyOnce`, `CoreCommands.OneShotsHaveNoEndCallbackAndOtherThreadsRecord`, `IndepRuntime.EndsFromConcurrentProducersAreExactlyOnce`, `MusicTransport.BeatsOfAStandaloneTransport`, `CoreMusic.MusicPlaybackDrivesVoicesThroughTheMusicSystem`（マーカー）, `music_*` の各テスト | 効果音（非音楽）のクリップにマーカーが無い（`ASST` / `CLIP` にマーカーの列が無い） | B（列はバンク形式の追加 → 依頼）、音楽は統合済み |
| 解析（バスごとのメータとスペクトル、ラウドネス） | 一部だけ | ピーク・RMS をバスごとに計測（`bus_graph.cpp:149-154`）、公開（`sys_publish.cpp:157-178`）、読み出し `core/src/update.cpp:338-363` `GetMeter`。ラウドネスはマスターだけ計測（`bus_graph.cpp:163-166`） | `Meter.*`（dsp）、`LivelinkProfiler.StreamsVoicesMetersTimingMemoryAndTheEventLog`、`snd_get_meter` を使うテスト（`core_basic_test.cpp` ほか） | **スペクトルの経路が無い**（dsp の `SpectrumPush/Compute` はあるが core も公開 API も無い）。**ラウドネスがバスごとでない**: マスター以外のバスにもマスターの値を入れている（`sys_publish.cpp:174-175`）。`kBusMeter` を見ない | B（公開関数の追加は c_api.cpp 末尾追記で可） |
| ピッチ・速度 | 実装済み（タイムストレッチは D） | `sys_dsp.cpp:191-202`、`sys_render.cpp:436-445`（`maxPitchRatio` で頭打ち） | `CoreMix.PitchAndSourceRateGiveTheExpectedFrequency`, `ApiPlaybackGain.PitchScalesTheFrequency`, `IndepPlaybackGain.PitchRatiosAndClamps`, `SourceSample.PitchAndSourceRate` | なし | B（テストの対応付けのみ）、D（タイムストレッチ） |
| 多言語ボイス | 実装済み（B の対象外） | `core/src/sys_apply.cpp`（SetLanguage）、bank | `CoreBanks.LanguageSwitchKeepsPlayingVoicesAndResolvesTheNewLanguage`, `IndepFix.LanguageBackAndForthRepeatedly`, `IndepFixed.core_language_switch_back_while_old_bank_unloading` | なし | 持ち場の外（判定のみ） |
| 出力先（出力バス） | 一部だけ（B の対象外） | `kBusOutput` のバスはマスターに足さない（`bus_graph.cpp:159`、`bank_registry.cpp:707`）が、どの出力にも出ない | なし | 出力バスを別の出力（デバイス・振動）へ渡す経路 | 持ち場の外（platform / api） |
| ライブ調整 | 実装済み（B の対象外） | `core/src/live_link.cpp`、`livelink/` | `LivelinkOverride.*`（12 本）、`IndepLivelinkOverride*`、`LivelinkRecord.*`、`LivelinkProfiler.*` | なし | 持ち場の外（C） |

## 2. B-coverage §2 の細目ごとの判定

### ① 3D と距離カーブ

| 細目 | 判定 | 根拠 | 既存テスト | 足りないもの | 担当 |
| --- | --- | --- | --- | --- | --- |
| 3D: 距離減衰（カーブ） | 一部だけ | `sys_dsp.cpp:141-148`（`EVNT.curveVolume` dB、無ければ 1/max(d,1) と `maxDistance`） | `Core3D.DistanceAndDirection`（1/d のみ） | 既定のロールオフの種類（逆数・線形・対数、最小距離）を選ぶ手段が無い（列も無い）。カーブのテストが無い | B |
| 3D: ドップラー | 未実装 | `sys_3d.cpp:58`（常に 1）。`Spatial.dopplerRatio` 列と EvaluateDspParams の読み出し宣言は既にある（`schema.hpp:190, 355`）。Listener / Emitter の速度もある | なし | 比の計算と Voice.pitch への掛け算。係数・音速の設定 | B（係数: `snd_config` 末尾に全体値、Event 単位の列は依頼） |
| 3D: コーン | 実装済み | `sys_dsp.cpp:155-166`（内角・外角・外側 dB を角度で補間）、`sys_3d.cpp:59` | なし | テスト。コーンの外側フィルタ（Unity / CRI にある LP）は無い | B |
| 3D: spread | 一部だけ | `sys_render.cpp:486-491`（距離カーブ `curveSpread`）→ `dsp::ComputePanMatrix(..., spread)` | なし | 一定値の spread（カーブ無し）を置く列が無い。テスト | B |
| 3D: 2D/3D ブレンド | 一部だけ | `sys_render.cpp:487-491`（`curveBlend3D`）、`PanFor` `sys_render.cpp:60-85` | なし | テスト。ブレンドは減衰には掛からない（パンだけ） | B |
| 3D: 複数リスナー（最寄り・重み付き・画面分割） | 一部だけ | 最寄りだけ（`sys_dsp.cpp:27-42`、`sys_render.cpp:471-480`）。`Config.listener_mode` 未使用 | なし | Weighted（`Listener.weight` は列にある）と Split | B（Split の出力の形は D-番号で決める） |
| 3D: 遮蔽・閉塞 | 一部だけ | Parameter → `Emitter.occlusion`（`sys_params.cpp:54-70`）まで。音には効かない | なし | 音量と LP への写像 | B |
| 3D: 位置の補間・外挿 | 実装済み | 外挿 `sys_3d.cpp:36-39`、ブロック内はパン行列とゲインのランプ（`sys_render.cpp:121-128`） | なし（間接的に `Core3D.DistanceAndDirection`） | テスト | B |
| 距離カーブ: 音量 | 実装済み | `sys_dsp.cpp:143-145` | なし | テスト | B |
| 距離カーブ: フィルタ（LP） | 実装済み | `sys_dsp.cpp:150-154` | なし | テスト。HP の距離カーブは `CURV.input = kInputDistance` + target Highpass で可（`sys_dsp.cpp:111-121`） | B |
| 距離カーブ: リバーブへの送り | 未実装 | `EVNT.curveReverbSend` を読む箇所が無い | なし | ボイスからの補助送り（ボイスは 1 バスにしか出ない、`sys_render.cpp:410-418`）と送り先 | B（送り先の列は依頼） |
| 範囲音源（線・面・箱） | 未実装 | Emitter に形の列が無い | なし | 形の設定、最近点までの距離・方向、内側での spread | B |
| リバーブゾーン | 未実装 | なし | なし | ゾーンの形・送り先・量、リスナー（または音源）がゾーン内のときの送り | B |
| ルーム／ポータル | B の対象外 | — | — | 段階6 | D |

### ② 標準 DSP とボイスエフェクト

| 細目 | 判定 | 根拠 | 既存テスト | 足りないもの | 担当 |
| --- | --- | --- | --- | --- | --- |
| DSP: リバーブ（I3DL2 を含む） | 実装済み（dsp）、バスでのみ使える | `dsp/src/fx_reverb.cpp`、パラメータ表 `effects.hpp:42-57` | `Effects.GoldenDefaults`（`fx_reverb.sndg`）、`Effects.BehaviourChecks`、`IndepEffectsBehaviour*` | core 経由の機能テスト。I3DL2 の RoomRolloffFactor は 3D 側（無し） | B |
| DSP: エコー / ディレイ | 実装済み（dsp） | `dsp/src/fx_delay.cpp` | `fx_echo.sndg`, `fx_delay.sndg`, `Effects.BehaviourChecks` | core 経由のテスト | B |
| DSP: コーラス / フランジャー | 実装済み（dsp） | `fx_delay.cpp`（変調ディレイ） | `fx_chorus.sndg`, `fx_flanger.sndg` | 同上 | B |
| DSP: ディストーション | 実装済み（dsp） | `dsp/src/fx_filter.cpp` | `fx_distortion.sndg` | 同上 | B |
| DSP: コンプレッサー（サイドチェーン付き） | 一部だけ | dsp は対応（`Effects.CompressorStaticCurveAndSidechain`）。core はサイドチェーンを渡さない（`bus_graph.cpp:120`） | `fx_compressor.sndg`, `Effects.CompressorStaticCurveAndSidechain` | サイドチェーン元のバスの指定と配線 | B |
| DSP: リミッター | 実装済み | `fx_dynamics.cpp` | `fx_limiter.sndg`, `Effects.LimiterHoldsTheCeiling`, `IndepBusEffects11.LowpassAndLimiterParametersFromTheBankAreAudible` | なし | B |
| DSP: ノーマライズ | 実装済み（dsp） | `fx_dynamics.cpp` | `fx_normalize.sndg` | core 経由のテスト | B |
| DSP: パラメトリック EQ | 実装済み（dsp） | `dsp/src/fx_filter.cpp`（4 バンド、8 型） | `fx_parameq.sndg` | 同上 | B |
| DSP: 各種フィルタ（LP / HP / BP / ノッチ / シェルフ） | 実装済み（dsp） | `fx_filter.cpp`（LP / HP 単体、BP・ノッチ・シェルフは ParamEq の型） | `fx_lowpass.sndg`, `fx_highpass.sndg`, `Biquad.*`, `CoreMix.BusLowpassEffectAttenuatesHighTones` | 独立した BP 種別は無い（ParamEq の型 6 で表す、で足りるかは判断） | B |
| DSP: ピッチシフター | 実装済み（dsp） | `fx_delay.cpp`（2 タップ） | `fx_pitchshift.sndg`, `Effects.PitchShifterUnityIsDelayedInput` | core 経由のテスト | B |
| DSP: サラウンダー | 実装済み（dsp） | `fx_filter.cpp`（`kEffectSurround`） | `fx_surround.sndg` | 同上 | B |
| DSP: 実行中のパラメータ変更 | 未実装 | `EffectInstance.params` は登録時の値のまま（`bank_registry.cpp:222-255`）、`ParamBlock` の start = end（`bus_graph.cpp:119`） | なし | スナップショット（`SNPV.targetKind` 1）、Parameter カーブ（`EffectParam`）、必要なら公開 API | B |
| DSP: LOD バイパス（テールの後） | 実装済み | `sys_dsp.cpp:273`、`bus_graph.cpp:113-126` | （LOD のテストは `CoreLoad.*` が間接） | 直接のテスト | B |
| DSP: コンボリューション | B の対象外 | — | — | 段階6 | D |
| ボイスエフェクト: LP / HP 標準 | 実装済み | `sys_render.cpp:118-120, 497-499`、`dsp::VoiceFilterProcess` | `VoiceFilter.*`（dsp） | core を通した LP / HP（クリップの乱数フィルタ、カーブ、距離 LP）の機能テスト | B |
| ボイスエフェクト: 重いもの（DSP プールの枠） | 未実装 | Event / Track からエフェクトを指す列が無い。ボイス別のエフェクト状態の置き場も無い（D-43 の「RealVoice のスロットで引くプール」に該当するものが無い） | なし | 列（依頼）、プールの枠（実ボイス数ぶんか、別の上限か）、チャンク内での処理 | B（列は依頼） |

### ③ Parameter・スナップショット・ダッキング・カテゴリ

| 細目 | 判定 | 根拠 | 既存テスト | 足りないもの | 担当 |
| --- | --- | --- | --- | --- | --- |
| Parameter: 3 つのスコープ | 実装済み | `params.hpp:18-108`、`sys_apply.cpp:649-659` | `CoreMix.ParameterScopesPlaybackOverEmitterOverGlobal`, `IndepMix.ParameterScopesComposePerPlayback` | `PARM.scopeMask` の強制（設定外のスコープの値をどう扱うかは D-番号） | B |
| Parameter: 多点カーブ | 実装済み | `core_util.cpp:84-104`（端で頭打ち、`shape` 指数） | `CoreMix.OscillatorFollowsPitchCurveOnAParameter`, `CoreMix.PanCurveMovesTheMonoSourceRight`, `IndepMix.VolumeCurvesAddInDb` | 3 点以上・shape のテスト | B |
| Parameter: 追従速度 | 実装済み | `sys_params.cpp:23-43`（単位/秒、初回は既定値から） | `CoreMix.ParameterRampRate`, `IndepMix.ParameterRampFollowsTheRate` | なし | B |
| Parameter: 合成の規則 | 一部だけ | `sys_dsp.cpp:118-124`（音量 dB 加算・ピッチ セント加算・LP 最小・HP 最大・パン加算） | `IndepMix.VolumeCurvesAddInDb` | `CURV.compose` を読まない。「その他は乗算」が無い。Send / EffectParam / ReverbSend / Spread / Blend3D の Parameter カーブが効かない | B |
| スナップショット: 重み付き重ねがけ | 一部だけ | `sys_dsp.cpp:243-264`、`sys_apply.cpp:673-685` | `CoreMix.SnapshotWeightScalesBusVolume` | エフェクト値・センド量。複数同時のテスト。アクションの `ApplySnapshot` は時間無しで即時（`sys_apply.cpp:981-986`） | B |
| ダッキング: サイドチェーン | 一部だけ | `sys_dsp.cpp:221-228`（前ブロックのバスのピック > しきい値でオン） | なし | テスト。比例的な（コンプレッサー型の）サイドチェーン | B |
| ダッキング: ルール型 | 実装済み | `sys_dsp.cpp:171-183, 218-220` | `CoreMix.RuleDuckingLowersTheTargetBusWhileTheCategoryPlays` | 攻撃・解放の時間のテスト | B |
| カテゴリ: 音量・ミュート | 実装済み | `sys_dsp.cpp:52-68`、`sys_apply.cpp:686-710` | `CoreMix.ClipBusAndCategoryGainsMultiply`, `IndepMix.CategoriesVolumeMutePauseAndAll` | なし | B |
| カテゴリ: ソロ | 未実装 | API・コマンド・状態のどれも無い | なし | 公開関数と表し方（下記 §3） | B |
| カテゴリ: ポーズ（Playback・カテゴリ・全体） | 実装済み | §1 の「ポーズ」行 | §1 | なし | B |
| カテゴリ: ポーズを無視するカテゴリ | 実装済み | `sys_event.cpp:64-70, 1272-1276` | `IndepMix.CategoriesVolumeMutePauseAndAll`（`kCategoryIgnorePause` を使う） | Event 単位の `kEventIgnorePause` のテスト | B |
| カテゴリ: フォーカス喪失で消音（`SND_FOCUS_PER_CATEGORY`） | 未実装 | `kCategoryMuteOnFocusLoss` を読まない。`api/compose/compose.cpp:155-157` | `IndepDevice.FocusContinueAndPerCategoryChangeNothing`（今は何もしないことを確認） | core 側のカテゴリ消音と、api からフォーカス状態を core に渡す経路 | B ＋ api（末尾追記で済まない可能性 → 依頼） |

### ④ ボイス管理・シーケンス種別・通知・解析

| 細目 | 判定 | 根拠 | 既存テスト | 足りないもの | 担当 |
| --- | --- | --- | --- | --- | --- |
| ボイスリミットのグループ | 実装済み | `sys_event.cpp:330-346` | `CoreVoice.LimitGroupNeverRejectsAndOldestSteals` | なし | B |
| プライオリティ | 実装済み | `sys_voices.cpp:27-31` | `CoreVoice.PriorityBeatsLoudness`, `IndepMix.VoiceSelectionPriorityThenLoudnessAndFourTimesTheLimit` | なし | B |
| 仮想ボイス | 一部だけ | `sys_voices.cpp`、`sys_render.cpp:579-611`（位置送り）、戻り方は source | `IndepMix.VirtualVoiceAdvancesAndContinuesInTime`, `IndepFix.VirtualUserBufferVoiceAndDestroy` | `EVNT.virtualReturn` を使わない。「先頭から」「戻らない」の core 経由のテスト | B |
| 奪い方の規則（古い・小さい・低優先・奪わない） | 一部だけ | `sys_event.cpp:256-285` | `CoreVoice.LimitGroupNeverRejectsAndOldestSteals`（古い・奪わない） | 「小さい」が可聴度でなく近似。「低優先」のテスト | B |
| シーケンス: ランダム | 実装済み | `sys_event.cpp:462-485` | `CoreSequence.RandomNoRepeatNeverRepeats` | 重み付きの分布のテスト | B |
| シーケンス: シャッフル | 実装済み | `sys_event.cpp:490-507` | `CoreSequence.ShuffleCoversEveryClipBeforeRepeating` | 65 個以上（64 で切る） | B |
| シーケンス: 順送り | 実装済み | `sys_event.cpp:444-446, 486-489` | `CoreSequence.SequentialCycles` | なし | B |
| シーケンス: コンボ | 実装済み | `sys_event.cpp:438-443` | `CoreSequence.ComboAdvancesOnlyInsideTheWindow` | なし | B |
| Clip 単位のランダム幅 | 実装済み | `sys_event.cpp:533-560` | （決定論のみ） | 範囲のテスト | B |
| スイッチ型 Event | 実装済み | `sys_event.cpp:508-520` | `CoreSequence.SwitchFollowsTheSwitchValue` | なし | B |
| アクショントラック | 一部だけ | `sys_event.cpp:788-837`、`sys_apply.cpp:948-990` | なし | 1 ブロック遅れ、`SetParam` が Global 固定、スナップショットが時間無し。テスト | B |
| 入れ子の Event | 一部だけ | `sys_event.cpp:549-551` | なし | 子の Event のルーティング・3D・リミットを使わない。テスト（循環はビルドで検出、深さ 4 で打ち切り） | B |
| レイヤーのクロスフェード | 一部だけ | `sys_event.cpp:458-461`、`sys_dsp.cpp:126-140` | なし | 重みの形の決定（等パワー等）とテスト | B |
| ワンショット | 実装済み | `cmd::kPlayOneShot` | `CoreCommands.OneShotsHaveNoEndCallbackAndOtherThreadsRecord` | なし | B |
| 通知: 終了 | 実装済み | `sys_event.cpp:175-231`、`sys_publish.cpp` | §1 の「再生中の通知」行 | なし | B |
| 通知: マーカー | 一部だけ | music の `MMRK` のみ | `CoreMusic.MusicPlaybackDrivesVoicesThroughTheMusicSystem`, `music_*` | 効果音のマーカー（列が無い） | B（列は依頼）、音楽は統合済み |
| 通知: 拍 | 実装済み | `sys_transport.cpp:244-262`、music | `MusicTransport.BeatsOfAStandaloneTransport` ほか | なし | 統合済み music |
| 解析: バスごとのメーター | 実装済み | `bus_graph.cpp:149-154`、`update.cpp:338-363` | `snd_get_meter` を使う各テスト、`LivelinkProfiler.*` | `kBusMeter` の扱い（全バス計測で良いかの決定） | B |
| 解析: スペクトル | 未実装（dsp にカーネルのみ） | `dsp/include/snd/dsp/meter.hpp:86-99` | `Meter.SpectrumFindsTheTone`（dsp） | core での計測、公開の読み出し関数 | B |
| 解析: ラウドネス | 一部だけ | マスターのみ（`bus_graph.cpp:163-166`）、全バスにマスター値（`sys_publish.cpp:174-175`） | `Meter.LoudnessEbuReference`（dsp） | バスごとの計測（状態 約 19 KB / バス）と正しい公開 | B |

## 3. 足りない機能の入口（列・API・コマンド・依頼）

| 機能 | バンクの列 | 公開 API / コマンド | 共通の決まりの範囲での表し方（案） | 持ち場の外への依頼 |
| --- | --- | --- | --- | --- |
| ドップラー | 無し（`EVNT` に係数の列なし） | 無し | `snd_config` 末尾に `doppler_factor`・`speed_of_sound`（全体値）。計算は Compute3D（`Spatial.dopplerRatio`、列は既存）、掛けるのは EvaluateDspParams（読み出し宣言は既存） | Event 単位の係数 `EVNT.dopplerFactor`（bank-format / buildtool / project-format） |
| リスナー合成方式 | 不要 | `Config.listener_mode` 既存 | core だけで実装。Split の出力の形（バスをリスナー数ぶん？）は D-番号 | Split を実出力に分けるなら platform / api |
| 遮蔽・閉塞の効き | 不要（既定の写像） | `snd.occlusion` / `snd.obstruction`（既存の慣習名） | EvaluateDspParams で音量と LP に写す（写像は D-番号） | 写像を設定可能にするなら列の依頼 |
| 範囲音源 | 無し | 無し | 新しい公開関数 `snd_set_emitter_shape(sys, emitter, kind, const snd_vec3* extents)`（スカラーと既存型だけ）。コマンドは既存の `SetParam`（Emitter スコープ）で慣習名（例 `snd.shape.kind` / `snd.shape.x|y|z`）を必達で送る。EvaluateLogicParams が新しい列 `Emitter.shape` に書き、Compute3D が読む（schema に列と読み出し宣言を追加） | 設計ツールで設定するなら Event / Emitter テンプレートの列（依頼）。replay 規則表の説明（C） |
| リバーブゾーン | 無し | 無し | ゾーン = 形を持つ Emitter ＋ `SetSwitch`（Emitter スコープ、値が Id そのまま運べる）で送り先バス、`SetParam` で量。ボイスに補助送り（下の行）を足す | バンクでゾーンを定義するなら新しいチャンク（依頼） |
| 距離カーブのリバーブ送り | `EVNT.curveReverbSend` は既存 | 無し | ボイスからの補助送り（チャンクのアキュムレータにもう 1 区分、`RenderSet.segs`）。送り先は (a) ゾーンのバス、(b) Event のバスの最初の `SEND` の先、のどちらかを D-番号で決める | Event の送り先バスの列 `EVNT.reverbBus`（依頼） |
| ボイスエフェクト（重いもの） | 無し | 無し | core 側の枠（実ボイスのスロットで引くプール、D-43 の規則どおり）を先に作り、テストではプラグイン（`Config.plugins`）か標準効果をコードで指定 | `EVNT` または `TRAK` に `effectFirst` / `effectCount`（依頼） |
| コンプレッサーのサイドチェーン | `DUCK` の kind 0（source = バス、target = バス）と `EFFX.flags`（未使用）が既存 | 無し | 「`DUCK` kind 0 の対象バスに `UseSidechain` のコンプレッサーがあれば、元バスの前ブロック（または同ブロック、グラフ順で先なら）の出力をサイドチェーンに渡す」、あるいは `EFFX.flags` のビットで指定、を D-番号で決める（列の追加は不要） | 専用の列にするなら依頼 |
| エフェクトのパラメータ変更 | `SNPV.targetKind` 1 / `paramIndex`、`CURV.target = EffectParam` は既存 | 無し | スナップショットは core だけで実装可（targetRow = EFFX 行、paramIndex）。公開関数 `snd_set_effect_param(sys, bus, effect_index, param, value, fade)` を足すならコマンドは `SetBusVolume.reserved` を variant にする案（または `SetParam` Global に慣習名）。`ParamBlock` の start/end でランプ | Parameter カーブの宛先（どの EFFX・何番か）は `CURV` に列が要る（依頼） |
| センド量のスナップショット | `SNPV.targetKind` 2（targetRow = SEND 行）既存 | — | core だけで実装可 | なし |
| カテゴリのソロ | 不要 | 無し | 公開関数 `snd_solo_category(sys, category, int32 solo)`。コマンドは `SetCategoryVolume.reserved` を variant（0 = 音量、1 = ソロ）にする案（`PauseCategory` / `MuteCategory` の値は 0/1 しか通らない） | replay 規則表に variant を足す（C） |
| フォーカス喪失でのカテゴリ消音 | `kCategoryMuteOnFocusLoss` 既存 | `SND_FOCUS_PER_CATEGORY` 既存 | core に「フォーカス喪失中」フラグを渡す内部関数を足し、EvaluateDspParams でカテゴリゲイン 0 | api の `compose.cpp`（末尾追記ではない）→ 依頼 |
| Event 単位の仮想ボイスの戻り方 | `EVNT.virtualReturn` 既存 | — | core が Devirtualize の前に Event の値で決める（または source に渡す。`SourceControlOp` に欄が要るなら依頼） | source（必要な場合） |
| 効果音のマーカー | 無し | `on_marker` 既存 | — | `ASST` にマーカー表（`MMRK` 相当の参照列、例 `ASST.markerFirst/Count`）か `CLIP` の列（依頼） |
| 「最も小さい」での奪い方 | `VLGR.stealMode` 1 既存 | — | `Voice.audibility`（前ブロック）で選ぶ。RunEventLogic に `Voice_audibility` の前ブロック読み出し宣言を足す（schema） | なし |
| N 回ループ | `CLIP.loopCount` 既存 | — | core が再生位置（`Voice.playhead.loopCount`）を見て N 周目の終わりで止める案。source が `kStartLoop` の回数を持てないため | source に回数を渡すなら依頼 |
| スペクトル | 不要 | 無し | 公開関数 `snd_get_spectrum(sys, bus, float* out, uint32_t bins)`（任意スレッド）。値は core が持つ独自のトリプルバッファで公開（`notify.hpp` の `BusSnapshot` は 80 バイト固定なので広げない） | なし（c_api.cpp 末尾追記） |
| バスごとのラウドネス | 不要（`kBusMeter` で対象を選ぶ案） | `snd_get_meter` の欄が既存 | `rd.loudness` をバスごと（`kBusMeter` のバスだけ）にする | なし |
| 動的生成 Input / Oscillator / Noise | — | `snd_play_input` 既存 | — | source（Input・Oscillator・Noise の種別）、platform（入力デバイス） |
| 出力バスの実出力 | `kBusOutput` 既存 | — | — | platform / api |

## 4. テスト基盤

- **バンクを組む**: `core/test/test_bank.hpp` の `TestBank`（`TEvent` / `TTrack` / `TClip` / `TAction` / `TBus` / `TEffect` / `TCurve` / `TParam` / `TCategory` / `TGroup` / `TSnapshot` / `TDuck`）→ `Build(bankId, languageId, deps)`。データは `Constant` / `Sine` / `Impulse`。**足りない欄**: `EVNT.curveReverbSend`・`virtualReturn`、`CLIP.fadeOutMs`、`CPNT.shape`、`PARM.scopeMask`、`BUSS.lodPriority`、`EFFX.flags`・blob、`SEND` 以外のバンク表（core/test にあるので B が足せる）。
- **ブロックを回す**: `core/test/harness.hpp` の `Harness`（`SmallConfig()`、RT ワーカー 0、決定論ジョブ `snd::mock::DeterministicJobSystem`、メモリ FS `snd::mock::MemoryFileSystem`、`Step()` / `Steps(n)` / `Load()` / `At(frame, ch)` / `ToneAmplitude()`）。ソースはテスト用の `TestSampleKind`（PCMF をそのまま）と `TestOscKind`（サイン）（`core/test/test_sources.hpp`）。独立テスト用は `core/test/indep_fixture.hpp` の `IndepKind`（DC・カウンタ・有限 DC、D-43 の検査、ゲートで遅れチャンクを作る）と `Fx` / `Bank`。モックデバイスは公開 API 側の `SND_OUTPUT_MANUAL` + `snd_advance_output`（`api/test/api_rig.hpp`、`ApiHostOutput.*`）。
- **ゴールデン**: `tests/framework/include/snd/test/golden.hpp`（`CheckGolden` / `CompareGolden` / `GoldenTolerance`、`.sndg`、`SND_UPDATE_GOLDEN=1`）。dsp は `dsp/test/golden/`、core には golden ディレクトリが無い（作るなら `core/test/golden/`）。dsp のテスト補助は `dsp/test/dsp_test_util.hpp`（`Audio`, `AlignedMem`, `ToneAmplitude`, `Noise`, `ToBuffer`）と `dsp/test/indep_fx_util.hpp`（`Fx`, `Run`, `Material`, `MaxAbs`）。
- **RT 安全性**: `SND_RT_SCOPE()` と `snd::test::rt::Reset()` / `UnexpectedTotal()`（`tests/framework/src/rt_hooks_*.c(pp)`）。例 `CoreRt.RenderPathHasNoRtViolations`（`core/test/core_system_test.cpp:246`）。
- **初期化後の確保ゼロ**: `CountingAllocator`（`harness.hpp:148`）、`CoreMemory.NoAllocationAfterInitialisation`、`IndepMix.NoAllocationAfterCreateUnderAVariedScript`。
- **性能ベンチ（128 ボイス）**: core に有る: `CoreLoad.Real128VoicesWithinBudget` / `CoreLoad.FourTimesTheRealLimit`（`core/test/core_load_test.cpp`、Release でだけ 30% を強制、プロセス CPU 時間）、`IndepLoad.Real128VoicesUnder30PercentAndFourTimesTheLimit`（`core/test/indep_core_load_test.cpp`）、`tests/integration/indep_load_cpu_test.cpp`。dsp: `dsp/test/pipeline_bench_test.cpp`（リサンプル → フィルタ → パン → ミックス、128 ボイス）。新しいシステム（3D、ボイスエフェクト、ラウドネス、スペクトル）を足したら `core_load_test.cpp` の負荷にも含める。
- **決定論**: `CoreDeterminism.SameScriptSameBits`（`core_system_test.cpp:207`、同じ台本を 2 回＋並列）、`IndepMix.DeterministicWithRandomClips`、`IndepParallel.SerialAndParallelAreBitIdentical`、`MusicSystem.SameCommandsGiveBitIdenticalOutput`。**記録の再生（2.5 の「コマンド記録を 2 回再生」）** は `tests/integration/livelink_record_test.cpp` の `LivelinkRecord.ReplayIsBitIdenticalTwiceAndMatchesTheRecordedRun`（`livelink::Replayer`）。`core/src/recorder.cpp` はリプレイではなくゲーム側のコマンド記録（アリーナへの積み込み）。core のテストから Replayer を使うなら `core/test/CMakeLists.txt` のリンクに `sonus_livelink` を足す（モジュールの依存は livelink → core なのでテスト実行ファイルなら可、のはず。未確認）。

## 5. dsp の登録表と CMake の一覧（B は末尾追記だけ）

- エフェクトの登録表は 1 か所ではなく、次の 5 か所が組になっている（D も足すので衝突しやすい）:
  1. `dsp/include/snd/dsp/effects.hpp:122-138` `enum class EffectKind`（`Count` が最後。末尾追記 = `Count` の直前に足す）
  2. `dsp/include/snd/dsp/effects.hpp:140-153` 種別 ID の定数
  3. `dsp/src/effects.cpp:133-149` `kInfos[]`（`static_assert` で `Count` と数を照合、`IdsConsistent()` で順序と名前のハッシュを照合）
  4. `dsp/src/effects.cpp:229-245` 種別ごとの `switch`（処理の振り分け）
  5. `dsp/src/effects.cpp:425-438` `SND_FX_BATCH(...)` の並び
  パラメータ表（`kReverbParams` など）は `effects.cpp` の先頭、ヘッダの説明表は `effects.hpp:42-102`。
- CMake のソース一覧: `dsp/CMakeLists.txt:3-14` `SND_DSP_SOURCES`（スカラー版テスト `dsp_test_scalar` も同じ一覧から作る、`dsp/test/CMakeLists.txt:23`）。テスト一覧: `dsp/test/CMakeLists.txt:1-14` `SND_DSP_TEST_SOURCES`。core は `core/CMakeLists.txt:3-24`、テストは `core/test/CMakeLists.txt`。

## 6. 確かめきれなかったこと

- ビルド・実行をしていないので、表の「実装済み」は経路をコードで読んだ判定で、実際の数値は未確認。
- 公開ヘッダに「新しい構造体の型」を足してよいか（README §4 は「構造体は `snd_config` 末尾にだけ足す」）。上の案はスカラーと既存の型だけの関数にした。
- schema の「読み出し宣言」（`SND_SCHEMA_READS`）の追加が「新しい列の追加だけ」の範囲に入るか。D-88 では受け入れ側が足しているので可と推測。
- `core/test` から `sonus_livelink` をリンクしてよいか（ルートの依存規則は凍結の `CMakeLists.txt` 側）。
- 入れ子 Event が子のルーティングを使わないのは意図か欠陥か（設計書 4.2 は「Clip として置ける」だけ）。

## 7. 組②のマージ後に加わった機能テスト

上の §1・§2 は `ff5df53` での調査。次は `0ffb97a` にあるテストをソース上で照合した対応表。各構成の実行結果・差し戻しは `spec/handoff/parallel/report-B.md` に記録する。

| 設計 §7 の行 | 細目 | テスト名 |
| --- | --- | --- |
| エフェクト | リバーブ、I3DL2 | `IndepG2Kinds.ReverbDecayTimeIsTheRt60`、`EffectsCoverage.ReverbDecayHfRatioShortensTheHighFrequencyDecay`、`EffectsCoverage.ReverbLevelsDelaysAndFilters` |
| エフェクト | エコー、ディレイ | `IndepG2Kinds.EchoDelayInSamplesAndDecayRatio`、`IndepG2Kinds.DelayPingPongAlternatesSides`、`IndepG2Dsp.ClampToMaxDelayAtAnyRateAndChannelCount` |
| エフェクト | コーラス、フランジャー、ディストーション | `IndepG2Kinds.ChorusWithoutModulationIsAFixedDelay`、`IndepG2Kinds.FlangerCombNotchesAndMixes`、`IndepG2Kinds.DistortionClipsOutputGainAndMix` |
| エフェクト | コンプレッサー、リミッター、ノーマライズ | `IndepG2Kinds.CompressorThresholdRatioMakeupAndAttack`、`IndepG2Kinds.LimiterCeilingAndLookAheadLatency`、`IndepG2Kinds.NormalizeRaisesAQuietSignalWithinMaximumAmp` |
| エフェクト | EQ・フィルタ各型 | `IndepG2Kinds.ParamEqBandTypesFrequencyResponse`、`IndepG2Dsp.ParamEqAllpassBand`、`IndepG2Kinds.LowpassHighpassCutoffAndSlope` |
| エフェクト | ピッチシフト、サラウンド | `IndepG2Kinds.PitchShifterRatioAndLatency`、`IndepG2Kinds.SurroundFeedsTheRearsOfA51Bus` |
| エフェクト | 実行時の値と LOD | `IndepG2Param.FadeIsOneLinearRampFromTheNextBlock`、`IndepG2Impl.InitOnlyParameterAtRunTimeWarnsAndIsDropped`、`IndepG2Lod.BypassWaitsForTheTail` |
| ダッキング | コンプレッサーのキー経路（比例ダッキングの DUCK ルールは組③） | `IndepG2Side.KeyBusDrivesTheGainReductionInTheSameBlockWhenItComesFirst`、`IndepG2Side.KeyBusAfterTheTargetActsOneBlockLater`、`IndepG2Side.SidechainIsTakenAfterTheKeyBusFader` |
| 再生単位のフィルタ | 標準 LP・HP、距離 LP | `IndepG2VoiceFilter.ParameterCurveDrivesLowpassAndHighpass`、`IndepG2VoiceFilter.DistanceCurveLowpass` |
| 再生単位のフィルタ | 重い処理の枠と仮想復帰 | `CoreFx.EveryStandardKindRunsAsAVoiceEffect`、`CoreFx.VoiceEffectPoolFullPlaysDryAndCounts`、`CoreFxRecheck.VoiceBackFromVirtualReadsTheEventSettingAsItIsNow`、`IndepG2Recheck.FirstBlockHasThePlaybackValuesOnEveryStage` |
| 再生単位のフィルタ | プラグイン状態が枠を超えたときは全段ドライ。次の再生で枠を再利用 | `IndepG2PluginBudget.OneByteOverBudgetLeavesSlotForExactFit`、`IndepG2PluginBudget.SecondStageOverBudgetLeavesEntireChainDry` |
| 空間化 | プラグインをボイスに挿入。方位などをプラグインに渡す経路は B-5 依頼 | `CoreFx.PluginAsVoiceEffectGetsTheVoiceChannelsAndPlayback` |
| §2.5・§14 横断 | 決定論・確保・RT・128 実ボイス | `CoreFxDeterminism.SameScriptSameBitsSerialAndParallel`、`CoreFxMemory.NoAllocationAfterInitialisationOnTheNewPaths`、`CoreFxRt.NewPathsHaveNoRtViolations`、`CoreFxLoad.Real128VoicesWithTheVoiceEffectPoolFull` |

コンボリューションは段階6（D）の担当。表中のテストは組②の経路を対象とし、組①・③・④との組み合わせは 4 組合流後に検証する。

## 8. Cloud 復元版の機能テスト対応

設計 §7 の各行について、G2 `ebaa5a6`、G4 `db0677b`、G1 `194ceea`、G3 `f4c16d9` の作業先頭にあるテスト宣言を照合した。表は機能ごとの代表例。各組の合否と構成別実行結果は report-B.md に記録する。組②修正版は `48f01ee` で正式マージ済み。組④は `2f9d2cc` で正式マージ済み。組①・③は受け入れ前。

| 設計 §7 の行 | 機能テスト | 範囲・依頼 |
| --- | --- | --- |
| 再生の単位（Event／Track／Clip） | `IndepG4Loop.PitchChangeCutsFiniteLoopAtMeasuredWrap`、`CoreBasic.EndsWhenTheSampleEndsExactlyOnce` | B |
| ランダム・順送り | `IndepG4Seq.WeightedRandomFollowsTheWeights`、`IndepG4Seq.ShuffleOfThreeHundredClips`、`CoreSequence.ComboAdvancesOnlyInsideTheWindow` | B |
| ランダム幅 | `IndepG4Rand.VolumeStaysInsideTheRange`、`IndepG4Rand.PitchStaysInsideTheRange`、`IndepG4Rand.PanStaysInsideTheRange`、`IndepG4Rand.StartPositionStaysInsideTheRange`、`IndepG2VoiceFilter.ClipRandomFilterVariesPerPlayAndIsDeterministic` | B |
| 値による切替（スイッチ型 Event） | `CoreSequence.SwitchFollowsTheSwitchValue` | B |
| 値によるパラメータ制御（Parameter 3 スコープ＋多点カーブ） | `IndepG3Param.AllowedScopesWinInPlaybackEmitterGlobalOrder`、`IndepG3Param.LegacyPanAddsCurvesToClipInStoredOrder`、`IndepG3Param.RampRateFollowsElapsedTimeRegardlessOfVoiceCount` | Send／EffectParam のカーブは宛先列を bank／buildtool に依頼（B-2）。 |
| 他の再生の操作（アクショントラック、入れ子 Event） | `IndepG4Action.PlayEventStartsOnTheExactFrame`、`IndepG4Action.ScheduledStopCancelsAtItsFrame`、`IndepG4Nest.ChildLimitGroupFullRefusesTheSecondChild` | B |
| 再生間クロスフェード | `IndepG4Layer.TwoLayersEqualPowerInTheOverlap`、`IndepG4Fade.FadeInRunsWhileVirtual` | 等パワーの保証は2枚の重なりまで。 |
| ポーズ（Playback・カテゴリ・全体、ポーズ無視） | `IndepG3Pause.IgnoreFlagsSurviveGlobalPauseButIndividualPauseStillStops`、`CoreMix.PauseAndResumeKeepPosition` | B |
| ミックス（Bus＋センド＋カテゴリタグ） | `IndepG3Snapshot.SendContributionsRespectPreAndPostFaderTaps`、`IndepG3Category.ParentAndLeafSolosCombineAndClear` | 独立したバス枝の RT 並列化は持ち越し。 |
| 状態の切替（スナップショット、重み付き重ねがけ） | `IndepG3Snapshot.TwoWeightsReleaseIndependentlyToBusBase`、`IndepG3Snapshot.EffectContributionTracksChangingBaseAndReleases`、`IndepG3Action.RejectedInitialActionPreservesAlreadyRunningTransition` | B |
| ダッキング（サイドチェーン＋ルール型） | `IndepG3Shared.SurvivingKeyRedrivesCompressorAfterOwnerUnload`、`IndepG3Ducking.LevelTracksDifferentOverThresholdInputsAndCaps`、`IndepG3Ducking.LastCategoryVoiceReleasesRule` | B |
| 3D基本（距離減衰、ドップラー、コーン、spread） | `IndepG1Spatial.DopplerDistinguishesEmitterAndListenerVelocity`、`IndepG1Spatial.OuterConeAttenuatesAccordingToDecibels`、`IndepG1Spatial.WeightedListenersSwapChannelsAndSplitIsSymmetric` | B |
| 距離カーブ（音量・フィルタ・リバーブ送り等） | `IndepG1Spatial.DistanceVolumeCurveInterpolatesDecibels`、`IndepG1Spatial.LowAndHighPassFollowDistanceInFrequencyDomain`、`IndepG1Routing.DistanceSendUsesFirstBusSendAndSaturatesWithZone` | B |
| 範囲・空間構造（範囲音源、リバーブゾーン。ルーム／ポータルは段階6） | `IndepG1Spatial.LineRectangleAndBoxUseNearestPoint`、`IndepG1Routing.StrongestFourZonesKeepIndependentDestinations` | ルーム／ポータルは段階6。 |
| エフェクト（標準 DSP 一式） | `IndepG2Kinds.ReverbDecayTimeIsTheRt60`、`IndepG2Kinds.ParamEqBandTypesFrequencyResponse`、`IndepG2Kinds.PitchShifterRatioAndLatency` | コンボリューションは段階6。 |
| 再生単位のフィルタ（ボイスエフェクト） | `IndepG2PluginBudget.SecondStageOverBudgetLeavesEntireChainDry`、`VoiceFilterIndep.OpenIsBitExactPassThrough`、`IndepG2Vfx.NoAllocationAndNoRtViolation` | B |
| 空間化（`IDspEffect` による空間化プラグイン） | `IndepG2Vfx.PluginVoiceEffect` | プラグインへの方向・距離の伝達は拡張契約に依頼（B-5）。 |
| ボイス管理（リミットグループ、プライオリティ、仮想ボイス） | `IndepG4Steal.LowestPriorityNewcomerIsRejected`、`IndepG4Steal.StolenPlaybackEndReason`、`IndepG4Virtual.ContinueRestartAndNeverFromTheEvent` | B |
| 時間制御（共通クロック＋音楽時間の予約、開始・終了の時刻指定） | `MusicTransport.QuantisedPlaysStartOnTheirGrid`、`IndepG4Action.MusicalReservationAnchorsTrackTime` | 音楽時間の実装は統合済み music。B は予約とアクションの接続を検査。 |
| ストリーミング | `SourceEndToEnd.ResidentCompressedAndStreamThroughCore` | ストリーミング経路は source 担当。 |
| 読み込み形式（6.2 の 4 形式） | `SourceSample.CompressedAdpcmAndOpusMatchTheCodec` | デコードと常駐素材の読み込みは source／codec 担当。 |
| 動的生成・入力（Callback、UserBuffer、Input） | `ApiSynth.UserBuffersPlayTheGameMemory`、`ApiSynth.CppWrapperCallbackGetsTheContext` | Callback と UserBuffer の担当は source／api。Input は source／platform、Granular・Patch は D。 |
| 再生中の通知（終了、マーカー、拍） | `IndepG4Marker.MarkerClockIsTheFrame`、`IndepG4Recheck.TwoGroupsStealingTheLastVoicesEndPlaybackOnce`、`MusicTransport.BeatsOfAStandaloneTransport` | 素材マーカーの永続化は bank／buildtool に依頼（B-2）。 |
| 解析（バスごとのメータとスペクトル、ラウドネス） | `IndepG4Meter.LoudnessPerMeteredBus`、`IndepG4Spectrum.SineOnABinReadsItsAmplitude` | B |
| ピッチ・速度 | `IndepPlaybackGain.PitchRatiosAndClamps`、`IndepG4Loop.NLoopsWithPitch` | タイムストレッチは D。 |
| 多言語ボイス | `CoreBanks.LanguageSwitchKeepsPlayingVoicesAndResolvesTheNewLanguage` | 言語バンクの解決は統合済み bank／core の既存検査。 |
| 出力先（出力バス） | `IndepG3Category.IndependentOutputIsAbsentFromMasterButPublishesItsOwnPeak` | バスの分離とメーターを検査。物理デバイス・振動への出力は platform／api（B-3）。 |
| ライブ調整 | `LivelinkOverride.BusVolumeChangesTheLevelAndKeepsItAfterDisconnect` | ライブ調整は livelink／C。新しい variant の規則表は B-1。 |
