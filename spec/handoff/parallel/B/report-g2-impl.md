# 組② 実装報告（標準 DSP とボイスエフェクト、D-130〜D-135）

worktree `/home/user/sonus-wt/g2`、ブランチ `wip/b-g2`（ローカルコミットのみ、push なし）。
コミット: a164742（A〜C 実装）→ dd79ecf（D の種別テスト・EQ/I3DL2・負荷測定）→ 5f6e8eb（MinGW 書式）→ a345035（決定論テストで LOD を切る）。

## 1. 作ったもの

### A. バスのエフェクトのパラメータを実行中に変える（D-130）
- `core/src/fx_control.hpp` `EffectParamControl { TimedRamp set; float contrib; float applied; }`。`EffectInstance`（core_impl.hpp）に `ctl` / `blockStart` / `blockEnd`（各 16 個、バンク記録メモリ、`bank_registry.cpp` の登録準備で確保・初期化）。`params` はバンクの値（基準値）のまま。
- 値（Render の MixBuses → `StepEffectParams`）: ブロック末尾値 = clamp(RampValue(set, ブロック末尾) + contrib, パラメータ表の範囲)。ParamBlock は start = 前ブロック末尾値、end = この値、changed_mask = 変化したパラメータのビット（番号 31 以上はビット 31）。標準もプラグインも同じ ParamBlock。LOD バイパス中も値は進む。何も変えていなければ従来とビット同一（dsp 側が同じ clamp をするため）。
- 組③向け内部関数（`core_impl.hpp` 末尾）:
  - `EffectInstance* BusEffectInstance(Impl&, SlotIndex busSlot, uint32_t effectIndex)` … バスの定義バンクのチェーンの effectIndex 番目（無い・初期化失敗は null）。
  - `void SetEffectParam(Impl&, EffectInstance&, uint32_t param, float value, float fadeSeconds)` … set の目標とフェード（ブロック先頭の set の現在値から線形、範囲で頭打ち）。ApplyCommands〜Render 前のどのシステムからでも可。
  - 合成の足し場 `e.ctl[k].contrib`: **EvaluateDspParams が毎ブロック書き切る値**（スナップショット SNPV.targetKind 1 の重み×(値−基準値)、CurveTarget::EffectParam の寄与などの合計。寄与が無くなったら 0 に戻すのも書き手）。Render は読むだけ。基準値は `e.params[k]`。`ClampEffectParam` も公開。
- 公開 API `snd_set_effect_param(sys, bus, effect_index, param_index, value, fade_seconds)`（C++ `snd::SetEffectParam`）。記録時: effect_index ≥ 16、param_index ≥ 16、値またはフェードが非有限 → `SND_ERR_INVALID_ARGUMENT`。適用時に無いバス・エフェクト・パラメータ → 警告 `SND_ERR_NOT_FOUND`（object = バス id、`SetBusVolume` と同じ流儀）。bus 0 = マスター。
- ライブリンクの EFPV 編集（`live_link.cpp`）も `SetEffectParam(…, 0)` を呼ぶ（基準値の書き換えだけでは鳴りに反映されなくなるため）。
- テスト（core/test/core_fx_test.cpp）: `CoreFx.BusEffectParamBlocksRampToTheTargetWithTheMask`（記録用プラグインで 4 ブロックのフェードの start/end/mask を厳密に）、`CoreFx.BusEffectParamChangesTheSoundAndIsClamped`、`CoreFx.BusEffectParamLimiterCeilingClampedToItsRange`、`CoreFx.BusEffectParamArgumentsAndUnknownTargets`。

### B. コンプレッサーのサイドチェーン（D-131）
- `fx_control.cpp` `FindSidechain` / `StoreSidechainSources`（`bus_graph.cpp` から）。対象バスの**定義バンク**の DUCK 行を行順に見て、kind 0 で対象がそのバスである最初の行の元バスを使う。`kEffectHasSidechain` の標準エフェクト（コンプレッサー）に渡す（`UseSidechain` = 0 なら dsp が使わない）。
- graphOrder で元バスが先 → 同じブロックのポストフェーダー出力。後（または自分自身）→ 前ブロックの出力（DUCK kind 0 の行ごとに 8ch×block のバッファ、バンク記録メモリ＝登録準備で確保、初期値 0）。グラフだけで決まる。
- チャンネル数の違い: 変換しない。dsp の検出器はサイドチェーンの全チャンネルのピークを取るので、元バスのチャンネルをそのまま渡す。
- 重なり: 同じ行のしきい値型ダッキング（`sys_dsp.cpp`）は変えずに掛かる（コンプレッサーの圧縮＋amountDb の一定量）。テストで -15 dB + -6 dB = -21 dB を確認。
- サイドチェーン行を持つ登録バンクが無ければバスごとの探索をしない。
- テスト: `CoreFx.SidechainCompressorFollowsTheSourceBusStaticCurve`（両方のグラフ順で静特性 -12.75 dB と一致）、`CoreFx.SidechainSameBlockOrPreviousBlockByGraphOrder`（立ち上がりがブロック 10／11、繰り返しで同じ）、`CoreFx.SidechainMonoSourceAndThresholdDuckingBothApply`。

### C. ボイスエフェクト（D-132〜D-134）
- 標準 LP/HP の core 経由テスト: `CoreFx.VoiceLowpassFromTheClipRandomFilterStaysInItsRange`、`CoreFx.VoiceLowpassAndHighpassFromParameterCurves`、`CoreFx.VoiceLowpassFromTheDistanceCurve`（周波数ごとの振幅）。
- 枠（`core/src/voice_fx.cpp`、`fx_control.hpp` の `VoiceFxPriv` / `VfxSlot`、確保は `create.cpp` `InitVoiceFx`）。`snd_config` 末尾 `SND_CONFIG_TOP_S3_VOICE_FX`:
  - `max_voice_effects` 既定 4（実＋フェード予備で頭打ち）、`voice_effect_chain` 既定 2（上限 4、0 で無効）、`voice_effect_bytes` 既定 160 KiB（1 ボイスの全段の状態、64 MiB 超は INVALID_ARGUMENT）、`voice_effect_settings` 既定 64（Event 設定表と Playback 値表の各行数、1..65535）。
  - 既定での増分: 状態 4×160 KiB = 640 KiB（MemTag::DspPool）＋表 約 60 KiB。48 kHz のリバーブ（約 132 KiB）＋フィルタが 1 ボイスに入る。
  - 枠は (実スロット, assignSeq, ボイス) に結び付く。Render 直列部分: `VfxCaptureVoice`（新しいボイスの Event の種別を取り込む）→ `VfxReleaseStale`（実割り当てを離れたボイスの枠を返す。保留中のボイスは返さない＝D-43）→ 項目ごとに `VfxPrepareItem`（新しい割り当てで 1 回だけ取得を試す、ブロックの ParamBlock 値を書く、`ItemInput::vfxSlot`）。チャンクは自分のボイスの枠だけを `VfxProcess`。
  - 順序: リサンプル → 標準 LP/HP → 重いボイスエフェクト → エンベロープ（リリース、D-15 のフェード、サンプル精度の停止）→ パン。エフェクトの無いボイスは従来の順・ビットのまま。
  - D-133: 空き無し／全段の状態が枠に入らない → その実割り当ての間ドライ。`System::GetVoiceFxStats()`（acquired / dry / dryTooLarge / slotsInUse / slots）と警告 `SND_ERR_LIMIT_REACHED` + 詳細 `SND_END_DETAIL_VOICE_EFFECT_SLOTS`（object = 再生）で見える（`snd_stats` は広げない）。仮想化で返し、実に戻ると初期化して取り直す（D-15 のフェードインで戻る）。
  - D-134: エンベロープをエフェクトの後に掛けるので、停止フェード・スチール・ボイスの終わりで一緒に切れる。テールなし。
  - 設定: 種別はボイス開始時に取り込み、値は枠の取得時に設定表から（取り込んだ種別と違えば既定値）。Playback の値が上書き（次のブロックから、ブロック内補間）。
  - プラグイン（`Config.plugins`、同じ id ならプラグイン優先 D-31）も差せる。`state_bytes` / `init` は取得時にオーディオスレッドで呼ぶ。ParamBlock の ctx.playback に再生ハンドル。
- 公開 API（`sonus.h` / `sonus.hpp` / `c_api.cpp` の末尾）:
  - `snd_set_event_voice_effect(sys, event, slot, effect_kind, params, param_count)`: event 0、slot ≥ chain、param_count > 16、params NULL で count > 0、非有限 → INVALID_ARGUMENT。未知の種別 → NOT_FOUND。effect_kind = SND_ID_NONE で段を外す。名前・引数は依頼どおり。
  - `snd_set_playback_effect_param(sys, playback, slot, param_index, value)`: 0／古いハンドル → INVALID_HANDLE、範囲外・非有限 → INVALID_ARGUMENT。
- テスト: `CoreFx.VoiceEffectRunsOnlyOnTheConfiguredEvent`、`VoiceEchoPutsDecayingCopiesAndEndsWithTheVoice`、`VoiceEffectPoolFullPlaysDryAndCounts`、`VoiceEffectTooLargeForTheSlotPlaysDry`、`VirtualisedVoiceReturnsItsSlotAndTakesOneBack`、`PlaybackEffectParamAndConfigurationArguments`、`VoiceEffectConfigurationIsTakenAtTheVoiceStart`、`PluginAsVoiceEffectGetsTheVoiceChannelsAndPlayback`、`EveryStandardKindRunsAsAVoiceEffect`（14 種が既定の枠で動く。エコー／ディレイは MaxDelay 100 ms のとき。既定 1000 ms では枠に入らずドライ）。
- 空間化: ボイス単位に `IDspEffect` を差せるので「ボイス単位の差し込み」は満たす。3D 情報の欄は足していない（依頼は §4）。

### D. 標準 DSP 一式
- 14 種とパラメータ表を CRI と Unity の和集合と照合して覆っていることを確認（Unity: Lowpass/Highpass(Simple)、Echo、Flange、Distortion、Chorus、SFX Reverb、Pitch Shifter、Compressor、Normalize、ParamEQ、Duck Volume は core のダッキング＋サイドチェーン。CRI: リバーブ／I3DL2、エコー、ディレイ、コーラス、フランジャー、ディストーション、コンプレッサー、リミッター、EQ、各種フィルタ、ピッチシフター、サラウンダー）。
- **D-135 の変更（dsp の登録表は触っていない）**: 各種フィルタ = LP/HP の種別＋EQ のバンドの型。足りなかった **オールパスを型の値の末尾 8 に追加**。そのため (1) `dsp/src/effects.cpp` のパラメータ表 `kParamEqParams` の Type1〜4 の上限を 7 → 8（4 行、エフェクト登録表の 5 か所ではなくパラメータ表）、(2) `dsp/src/fx_filter.cpp` `BandType` に `case 8 → BiquadType::Allpass` を 1 行（biquad の設計は既存）、(3) `effects.hpp` の説明。種別は足していない。
- I3DL2: 全パラメータが意図どおり効いていた（実装の追加なし）。
- テスト（dsp/test/effects_coverage_test.cpp）: `EffectsCoverage.ParamEqBandTypesFollowTheirMagnitudeResponse`（8 型×5 周波数で解析的な |H| と ±0.15 dB、目印値、オールパスの位相 180°）、`ReverbDecayTimeSetsTheRt60`（0.5/1/2/4 s → 0.501/0.999/2.000/3.997 s）、`ReverbDecayHfRatioShortensTheHighFrequencyDecay`、`ReverbLevelsDelaysAndFilters`（Room、Reverb、Reflections、ReflectionsDelay、ReverbDelay、Density、Diffusion、RoomHF/HFReference、RoomLF/LFReference、DryLevel）。
- core 経由（core/test/core_fx_kinds_test.cpp）: `CoreFxKinds.EchoPutsDecayingCopiesAtTheDelay`、`DelayTimeFeedbackAndLevels`、`FlangerAndChorusAreCombFiltersAroundTheirDelay`、`DistortionAddsOddHarmonicsWithDrive`、`CompressorFollowsItsStaticCurve`（-14.26 dB 一致）、`LimiterNeverExceedsTheCeiling`、`NormalizeLiftsAQuietInput`、`PitchShifterMovesTheFrequencyByTheRatio`、`SurroundFeedsTheRearChannelsOfASixChannelBus`、`ParamEqPeakAndFiltersOnABus`、`ReverbTailOutlastsTheInputAndDecays`。`test_bank.hpp` は変更不要だった。
- LOD バイパス: `CoreFx.LodBypassWaitsForTheTailThenSkipsTheChain`（テスト用に `System::SetLodLevelForTesting` を core.hpp に追加）。
- 足さずに一覧（「等」）: マルチタップディレイ、フェイザー、ビットクラッシャー、トレモロ／オートパン、リングモジュレーター、ゲート／エキスパンダー、マルチバンドコンプ、グラフィック EQ（多バンド）、Unity Chorus の 3 系統別 WetMix、Pitch Shifter の FFT サイズ・Overlap（窓長で近似）。

### E. 決定論・RT・メモリ・遅れチャンク
- `CoreFxDeterminism.SameScriptSameBitsSerialAndParallel`（新経路を全部使う台本を直列 2 回＋並列で memcmp 一致）、`CoreFxMemory.NoAllocationAfterInitialisationOnTheNewPaths`、`CoreFxRt.NewPathsHaveNoRtViolations`、`CoreFxParallel.LateChunkKeepsTheVoiceEffectState`（ゲートで遅れチャンク、保留中も枠を保持、再取得なし、復帰後の音量）。
- TSan の repeat20 の失敗（13/25 回目程度）の原因: 遅れチャンクは 0 件で、直列同士でも一致しない回があった。原因は**負荷 LOD**: TSan だとブロックが予算を超え続けて LOD 2 になり、その後に始まるボイスが軽いリサンプラになる（時間依存）。データ競合ではない（TSan の報告なし）。a345035 でこのテストに `lod_down_blocks = 0` を設定（LOD テストにも、測った負荷で強制した段が動かないよう同設定）。修正後 TSan で 30 回連続通過。
  - 根拠: 設計書 2.5「条件付きで保証すること: 波形のビット一致は、同一ビルド・同一 SIMD 経路・**負荷LOD無効**・ストール無しのときだけ」。LOD が動く状態は「ロジックの決定」の一致だけが保証範囲で、波形のビット一致は保証外（リプレイ用の記録には LOD の段の変化を記録する、2.5 末尾）。既存の `CoreDeterminism.SameScriptSameBits` も `cfg.tuning.lod_down_blocks = 0`（core_system_test.cpp:177 のコメント「bit exactness needs the load LOD off (2.5)」）、`IndepParallel` も同じ。なお既存の CoreDeterminism の並列比較は 1e-6 の許容だが、本テストは memcmp で一致を確認している。

## 2. 決定の提案（番号は受け入れ側）
1. D-130 の合成: 値 = clamp(set のランプ + contrib)。contrib は EvaluateDspParams が毎ブロック書き切る加算値。乗算的な寄与は組③が換算する（別案: 寄与ごとの配列）。
2. バスの定義が別バンクへ移る（A1-16）と、実行中に設定した値は失われ基準値から（エフェクトの状態と同じ扱い）。
3. D-131 で見る DUCK 行は対象バスの定義バンクの行だけ。
4. サイドチェーンはプラグインには渡らない（`snd_dsp_effect.process` に引数が無い）。
5. ボイスエフェクト: 種別は開始時、値は枠の取得時。全段が入らなければボイス全体ドライ（段ごとに落とさない）。取得の試行は実割り当てごとに 1 回（途中から効き始めない＝ポップを避ける）。
6. プラグインをボイスエフェクトに使うなら `init` / `state_bytes` も原則 1 を守ること（取得時にオーディオスレッドで呼ぶ）。
7. Event 設定表は Event id で 1 行。Playback 値表は再生の寿命の間保持、満杯なら終わった再生の行を再利用、それも無ければ警告 LIMIT_REACHED。
8. EQ のオールパスは周波数と Q だけ使い Gain は無視。
9. 変種コマンドは合体フォールバックに入れない（variant を保持できない）。予約域も満杯なら拒否（COMMAND_BUFFER_FULL、reserve_overflows に計上）。

## 3. 契約の変更点
- `contract/public/include/snd/config.h`: `SND_CONFIG_TOP_S3_VOICE_FX`（`max_voice_effects` 4 / `voice_effect_chain` 2 / `voice_effect_bytes` 160 KiB / `voice_effect_settings` 64）を snd_config 末尾（1.2 host の後）と `snd_config_init` に追加。
- `sonus.hpp`: `Config` と `ToC` に同リスト、`snd::SetEffectParam` / `SetEventVoiceEffect` / `SetPlaybackEffectParam`（末尾）。
- `sonus.h` 末尾: `snd_set_effect_param`、`snd_set_event_voice_effect`、`snd_set_playback_effect_param`。
- `types.h`（`SND_END_DETAIL_RESERVATION_FULL` の直後に 4 行）:
  ```
  /* Stage 3 (coverage, D-133): warning detail only (SND_ERR_LIMIT_REACHED, object = playback):
   * a voice plays without its heavy voice effects (no free voice-effect slot, or the chain's
   * state does not fit snd_config voice_effect_bytes). */
  #define SND_END_DETAIL_VOICE_EFFECT_SLOTS 19u
  ```
  理由: D-133 の「数えて見えるようにする」を、`snd_stats` を広げずに既存の警告（on_error）で知らせるため。end の理由としては使わない（詳細値の列挙の末尾追加）。
- `schema.hpp`: 読み出し宣言 `R(Render, Voice_playbackSlot, Current)`（ボイスエフェクトの Playback 値と ctx.playback）。
- variant（定数は `core/src/fx_control.hpp`）:
  - `SetBusVolume.reserved` 下位 8 ビット = 1: バスのエフェクトのパラメータ。bits 8-15 effect index、16-23 param index、`volume` = 値（負もありうる）、`fadeSeconds` = フェード。
  - `SetSwitch.reserved[0]` = 1: Event のボイスエフェクトの種別。scope Global、`group` = event id、`value` = 種別（0 = 外す）、reserved[1] = 段、reserved[2] = 続く値の数。
  - `SetParam.reserved[0]` = 1: ボイスエフェクトのパラメータ。reserved[1] = 段、reserved[2] = パラメータ番号、`value`。scope Global: `param` = event id（設定）。scope Playback: `target` = 再生。
  - 3 コマンドとも知らない variant は捨てる。variant 0 は従来とビット同一。
- `api/compose/c_api.cpp` 末尾: 3 関数の配線（規則表に variant が無いので EntryOk は通さず core が検査）。
- core の内部（契約外）: `core.hpp` に `System::SetEffectParam` / `SetEventVoiceEffect` / `SetPlaybackEffectParam` / `GetVoiceFxStats` / `SetLodLevelForTesting`。

## 4. 持ち場の外への依頼
- バンク形式: `EVNT`（または `TRAK`）に `voiceEffectFirst` / `voiceEffectCount`（EFFX 行の範囲、値は EFPV）。buildtool・project-format も。代替: `snd_set_event_voice_effect`。
- `CURV` に EffectParam の宛先（バス id または EFFX 行＋パラメータ番号）の列（組③と共通）。
- livelink `command_check.cpp`: 上の 3 variant の規則と説明。特に SetBusVolume variant 1 は `volume` が負になりうるので、現行の Volume 規則のままだとリプレイ側が拒否する。
- api `config_check`: snd_config が伸びたので、1.2 host 版の大きさ（`offsetof(snd_config, max_voice_effects)`）を既知の配置に足す（今は旧ヘッダの呼び出し側が VERSION_MISMATCH）。api の検証に新 4 項目（core の Create は検証済み）。
- 空間化プラグインへの 3D 情報（欄は足していない）: `snd_render_context` の末尾に `const snd_vec3* direction`（リスナー基準の単位ベクトル）、`float distance`、`float spread`、`uint32_t listener_index` を足す案。ボイスエフェクトでは core が Spatial（最寄りリスナー）から写し、バスのエフェクトは NULL / -1。プラグインは `snd_contract_version()` で有無を確かめる。
- ext.h: ボイスエフェクトとして使うプラグインの `init` の RT 要件の注記。
- `dsp/test/dsp_test_util.hpp` の `std::aligned_alloc` が MinGW で通らない（既存。新しい dsp テストもこのヘッダを使うため、シムを `-include` して構文確認）。

## 5. 構成ごとの結果
- 5f6e8eb（A〜E 全部）: gcc Debug 214/214、clang 214/214、asan+ubsan 214/214、tsan 214/214、Release 214/214（repeat20 core_fx_test 通過）、scalar 214/214、avx2 214/214。警告 0（platform_device_play_test は既存のスキップ）。tsan の repeat20 core_fx_test は 1 回失敗 → 上記の LOD が原因、a345035 で修正。
- a345035: gcc Debug core_fx_* 通過、TSan で core_fx_test を 30 回連続通過。各構成の再実行結果は下の「再実行」に追記する。
- MinGW 構文検査（x86_64-w64-mingw32-g++、win32 版で通過。-posix 不要）: core/src の変更・新規 9 ファイル、dsp/src 2 ファイル、core/test の新規 3 ファイル、api/compose/c_api.cpp。dsp/test/effects_coverage_test.cpp は既存ヘッダの `std::aligned_alloc` のためシム付きで通過。
- 性能（Release、5f6e8eb、標準の状態の判定は既存テスト）:
  - `CoreLoad.Real128VoicesWithinBudget`: RT ワーカー 0: CPU 674.6 µs（予算 5333 µs の 12.6 %）、2: 737.0 µs（13.8 %）。FourTimesTheRealLimit avg 434 µs。`IndepLoad.Real128VoicesUnder30PercentAndFourTimesTheLimit` 通過。
  - `CoreFxLoad.Real128VoicesWithTheVoiceEffectPoolFull`（リバーブ＋LP を枠いっぱい、128 実ボイス、CPU / ブロック）: 枠 0: 662 µs（12.4 %）/ 738 µs（13.8 %）、枠 4（既定）: 804 µs（15.1 %）/ 844 µs（15.8 %）、枠 16: 1230 µs（23.1 %）/ 1002 µs（18.8 %）、枠 32: 1866 µs（35.0 %）/ 1919 µs（36.0 %）（各 RT ワーカー 0 / 2）。
  - dsp `Pipeline.Bench128Voices` 通過。

## 6. できなかったこと・既知の制限
- 枠の取得に失敗したボイスはその実割り当ての間ドライ（あとで空いても付かない）。
- Playback のボイスエフェクト値にフェード時間の指定は無い（1 ブロックのランプ）。
- 実行中に Event の設定を変えると、仮想中だったボイスは実に戻るとき新しい値を読む（種別が同じ場合）。
- ボイスエフェクトには LOD バイパスが無い。
- `snd_set_event_voice_effect` は複数コマンドで記録するので、途中で予約域が尽きると前半だけ適用される（戻り値は COMMAND_BUFFER_FULL）。
- サイドチェーンはプラグインに渡らない。

## 7. 既存のテストで新しい仕様と食い違うもの
- なし。EQ の Type 上限を 8 にしたため、パラメータを最大値にする既存テストはオールパスで走るようになった（全件通過）。

## 8. ブランチの先頭
- a345035（再実行の結果は下に追記）。

## 再実行（a345035、flock 付き）
下の「再実行の結果」を参照。

### 再実行の結果（a345035、flock 付き）
- TSan 全件 214/214、repeat20（core_fx_test, core_fx_kinds_test, -j1）通過。Release 全件 214/214、repeat20 通過。clang / asan / scalar / avx2 で core_fx_test・core_fx_kinds_test 通過、gcc Debug 全件 214/214。警告 0。
- Release の負荷（a345035）: CoreLoad 128 ボイス CPU 536.9 µs（10.1 %）/ RT2 600.6 µs（11.3 %）、4 倍要求 avg 317.5 µs。CoreFxLoad（リバーブ＋LP）: 枠 4: 666 µs（12.5 %）/ 697 µs（13.1 %）、枠 16: 985 µs（18.5 %）/ 1034 µs（19.4 %）、枠 32: 1440 µs（27.0 %）/ 1385 µs（26.0 %）。dsp Pipeline.Bench128Voices sinc mono 835.6 µs（15.7 %）、stereo 1189.6 µs（22.3 %）。

## 差し戻し 1（受け入れ側の判断への対応、コミット 192bace）

### 直したこと
1. Event の設定を読む時（Defect 1・2、決定の変更）: ボイスは枠を取るたびに（開始時・仮想から実に戻ったとき）Event のその時点の設定を種別と値の両方まとめて読む。鳴っているボイスは変わらない。仮想の間に段が外されていれば戻ったボイスはその段なし（警告なし、dry に数えない）。開始時の種別の取り込み（`VfxCaptureVoice`、`voiceEntry` / `voiceKinds` の列 = 仮想ボイス数×4×4 B + 4 B）は削除。`voice_fx.cpp` `Acquire` が `EventOfVoice` → 設定表から読む。入れ子 Event のボイスは子の Event の設定。Defect 1・2 のテストは決定と逆の期待なので落ちたまま（テスト担当が書き換える）。代わりのテスト: `CoreFxRecheck.VoiceBackFromVirtualReadsTheEventSettingAsItIsNow`（LP → HP に変えて戻ると HP、種別と値が食い違わない）、`CoreFxRecheck.StageRemovedWhileVirtualComesBackWithoutItAndWithoutAWarning`。既存の `CoreFx.VoiceEffectConfigurationIsTakenAtTheVoiceStart`（鳴っているボイスは変わらない）はそのまま通る。
2. Defect 3: 枠を取った最初のブロックは、再生ごとの値があれば start も end もその値（`VfxSlot::fresh`）。テスト `CoreFxRecheck.PlaybackValueAppliesFromTheFirstBlockOfASlot`。
3. Defect 4: `snd_set_event_voice_effect` は全部か無し。記録の前に 1 + param_count 個ぶんのバイト数が現在のアリーナの通常域だけ、または予約域だけに入るかを確かめ（`recorder.cpp` `ArenaFits`、確保・ロックなし）、入らなければ何も書かず `SND_ERR_COMMAND_BUFFER_FULL`（reserve_overflows に計上）。アリーナはスレッドごと（その生産者のスレッドだけが追記する）なので、他スレッドの同時記録で半端にならない。テスト `CoreFxRecheck.EventVoiceEffectIsRecordedAllOrNothing`。
4. [init] パラメータ: `snd_set_effect_param` は適用時に警告 `SND_ERR_INVALID_ARGUMENT`（object = バス id）で捨てる。`snd_set_playback_effect_param` は再生の Event の設定のその段の種別で判定して警告（object = playback）で捨てる（使う側でも [init] は上書きしない）。`snd_set_event_voice_effect` の値としては有効（枠の取得時の init で読む）。ライブリンクの EFPV 編集経由の `SetEffectParam` も [init] は動かさない（バンクの値は書き換わる）。テスト `CoreFxRecheck.InitOnlyParametersAreRefusedAtRunTime`。
5. 決定をコメントに: フェードの秒→フレームは切り捨て（バス音量と同じ）、バスを持つバンクの再登録でバンクの値に戻る、サイドチェーンはキーバスのフェーダーの後、DUCK kind 0 が複数なら行の順で最初の 1 本、表あふれは適用時に警告 LIMIT_REACHED で捨てる、入れ子 Event は子の設定（sonus.h・config.h・fx_control.cpp・voice_fx.cpp）。

### 契約の変更点の差分
- `contract/internal/include/snd/internal/commands.hpp` 末尾（namespace snd::cmd の閉じの直前）: `kBusVolumeVariantMask`、`kBusVolumeVariantEffectParam`、`kBusVolumeEffectIndexShift`(8)、`kBusVolumeParamIndexShift`(16)、`kBusVolumeIndexMask`(0xFF)、`kSwitchVariantVoiceEffect`、`kParamVariantVoiceEffect`、`BusEffectParamReserved()` とビット割り当てのコメント。コマンド種別・構造体は不変。`core/src/fx_control.hpp` は `using cmd::…` で参照。
- `sonus.h`: 3 関数のコメント（設定を読む時、全部か無し、[init] の警告、表あふれの警告、切り捨て、再登録で戻る）。関数の形は不変。
- `config.h`: `voice_effect_bytes` は 0..64 MiB（超えると INVALID_ARGUMENT、0 = 枠なし）、`voice_effect_settings` は 1..65535（範囲外 INVALID_ARGUMENT）、あふれは適用時に警告。既定値は不変。
- `types.h`・`schema.hpp` は前回から変更なし。

### 構成ごとの結果（192bace）
重い実行はすべて `flock /tmp/snd-heavy.lock`、-j2（TSan の ctest は -j1）。警告 0。全 221 件（テスト担当の indep_g2_* を含む）。
- gcc Debug 全件: 219/221 相当（失敗は indep_g2_impl_test だけ = 決定どおり落ちる Defect 1・2 の 2 件。Defect 3・4 は通過）。platform_device_play_test は既存のスキップ。
- ASan/UBSan 全件: 同上（indep_g2_impl_test の Defect 1・2 だけ）。
- TSan 全件: 同上。repeat20（-j1、core_fx_test / core_fx_kinds_test / indep_g2_* から impl を除く 8 本）通過。indep_g2_impl_test は 20 回実行して Defect 1・2 以外の失敗 0。
- Release 全件: 同上に加えて `jobs_rt_claim_steal_test` が 1 回失敗（jobs モジュール、遅れチャンクの時間依存。もう 1 体のビルドと同時刻。再ビルドして repeat20 は通過。組②は jobs を触っていない）。repeat20（同じ 8 本）通過、impl ×20 で想定外の失敗 0。
- SIMD スカラー / AVX2: dsp・core_fx_*・indep_g2_*（15 本）で indep_g2_impl_test の Defect 1・2 以外すべて通過。
- MinGW 構文検査: core/src の変更 7 ファイル、core/test/core_fx_test.cpp、api/compose/c_api.cpp が通過。
- Release の負荷（192bace、もう 1 体と同時刻のため a345035 より高め）: CoreLoad 128 ボイス CPU 649.2 µs（12.2 %）/ RT2 817.0 µs（15.3 %）、CoreFxLoad 枠 4: 806 µs（15.1 %）/ 795 µs（14.9 %）、枠 16: 1275 µs（23.9 %）/ 1297 µs（24.3 %）、枠 32: 1775 µs（33.3 %）/ 1556 µs（29.2 %）。Pipeline.Bench128Voices sinc mono 1083.6 µs（20.3 %）。判定（標準の状態 < 30 %）は通過。

### 先頭のハッシュ
- 192bace（wip/b-g2、ローカルのみ、push なし）。テスト担当のファイル（indep_g2_*）は変えていない。


## 差し戻し 2（コミット b527335、親 dc9b368）

### 直したこと
- `IndepG2Dsp.Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay`: `dsp/src/fx_delay.cpp`。Echo / Delay の Delay・Time を、リングの長さ（MaxDelay のフレーム + 1）ではなく MaxDelay の値（ms × 実行レート / 1000、`RingState::maxDelay`、EchoInit / DelayInit で設定）で頭打ちにした。リングは 2 つ目の上限で、実行レートがサイズ決めのレートより高い場合にだけ効く。構造体の大きさは変わらない（未使用の `pad` を置き換えた）。経路は `DelayFrames` の 1 か所で、バッチ版・SIMD / スカラーも同じ関数を通る。MaxDelay 以下の遅延はビット単位で従来と同じ。Chorus / Flanger / PitchShifter は変えていない。481（リングの長さでの頭打ち）を期待する既存のアサーションは無かった（既存の dsp テスト・ゴールデンは全部通過）。
- 回帰テスト `EffectsCoverage.DelayAboveMaxDelayIsExactlyMaxDelay`（dsp/test/effects_coverage_test.cpp）: サイズ決め / 実行レート 48k/48k、44.1k/44.1k、96k/48k、192k/44.1k で、Echo・Delay の 10 / 10.5 / 100 / 5000 ms が MaxDelay 10 ms ちょうど（480 / 441 フレーム）、5 ms はそのまま。
- コメントだけ: [init] の判定は「その再生の Event に今設定されている種別」で行うこと、鳴っている枠に値を入れるときは枠の種別で [init] を除くことを `voice_fx.cpp` 冒頭と `sonus.h`（`snd_set_playback_effect_param`）に明記。

### 契約の変更点の差分
- `sonus.h` のコメントだけ（関数・型・定数の変更なし）。

### 構成ごとの結果（b527335、重い実行は flock、-j2、TSan は -j1）
- gcc Debug 全件: 222 件すべて通過（platform_device_play_test は前からのスキップ）。
- ASan/UBSan 全件: 222 件すべて通過。
- Release 全件: 222 件すべて通過。
- TSan（dsp と core の fx 関係、indep_g2_* を含む 16 本）: 全部通過。
- `-DSND_SIMD_SCALAR=ON` / `-mavx2 -mfma`（同じ 16 本）: 全部通過。
- MinGW 構文検査: dsp/src/fx_delay.cpp、core/src/voice_fx.cpp、api/compose/c_api.cpp は通過。dsp/test/effects_coverage_test.cpp は既存ヘッダの aligned_alloc のシム付きで通過。
- **落ちるテストは 0 件**（Defect_ を含め全構成で失敗なし）。
- 性能（Release、flock の中で測定。標準の状態の判定は通過）:
  - CoreLoad 128 ボイス: CPU 520.6 µs（予算 5333 µs の 9.8 %）/ RT ワーカー 2: 646.5 µs（12.1 %）。
  - CoreFxLoad（リバーブ + LP、枠を使い切り）: 枠 0: 633.6 µs（11.9 %）/ 655.7 µs（12.3 %）、枠 4: 787.1 µs（14.8 %）/ 827.2 µs（15.5 %）、枠 16: 1190.5 µs（22.3 %）/ 1289.0 µs（24.2 %）、枠 32: 1766.1 µs（33.1 %）/ 1492.8 µs（28.0 %）（RT ワーカー 0 / 2）。
  - dsp Pipeline.Bench128Voices: sinc mono 992.0 µs（18.6 %）、sinc stereo 1393.8 µs（26.1 %）。

### 先頭のハッシュ
- b527335（wip/b-g2、ローカルのみ、push なし）。
