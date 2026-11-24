# 組② 独立テスト 報告（標準 DSP とボイスエフェクト）

- worktree: /home/user/sonus-wt/g2（ブランチ wip/b-g2、出発点 a345035）
- 状態: 手順 1〜6 完了。ブランチ wip/b-g2 の先頭 **97bd7c8**（ローカルコミットのみ、push なし）。実装・既存テストは変えていない。

## 1. 性質の一覧（実装を読む前。仕様・公開ヘッダ・決定だけから）

出典の略: H = 公開ヘッダのコメント、D = 依頼文の決定、S = 設計書。

### A. D-130 バスのエフェクトのパラメータ（snd_set_effect_param）
- A1 適用は「次のブロックから」。fade_seconds 後のブロック末尾で目標値。
- A2 fade の間、ブロックごとの値は時間に対して線形（等差）。到達後は動かない。
- A3 フェード中に別の目標を重ねると、いまの値から続く（跳ばない）。
- A4 fade 0 でもブロック内は dsp が補間（D-26）→ ブロック境界で段差なし。
- A5 値は範囲で頭打ち（範囲外の大きい値 = 最大値と同じ出力）。
- A6 非有限の value/fade、effect_index>=16、param_index>=16 → INVALID_ARGUMENT。fade<0 は 0。
- A7 未知のバス/エフェクト番号/パラメータ → 適用時に警告 NOT_FOUND（object=バス ID）、音は不変。
- A8 bus = SND_ID_NONE はマスター。
- A9 同じブロックに複数回 → 最後が勝つ。
- A10 effect_index はチェーン内の番号。同じ種別が 2 バスにあるとき片方だけ変わる。
- A11 バンクを外して再登録 → バンクの値に戻る（仕様に明記なし → 穴候補）。
- A12 SetBusVolume variant 1。variant 0 は従来とビット単位で同じ。知らない variant は捨てる。
- A13 決定論（2 回 / 直列と並列）。A14 確保ゼロ・RT 違反ゼロ。

### B. D-131 コンプレッサーのサイドチェーン
- B1 DUCK kind 0（S→T）＋ T に UseSidechain=1 のコンプ → T のゲインは S のレベルで決まる。
- B2 UseSidechain=0 → T 自身で検出。
- B3 S 無音 → T は下がらない。B4 S ミュート・一時停止 → 下がらない（音量の前後は穴候補）。
- B5 グラフ順で S が先なら同ブロック、後なら前ブロック。
- B6 DUCK 行が複数 → 穴候補。B7 S=T → 落ちない・暴走しない。B8 チャンネル数違い → ピークで同じ。
- B9 S のバンクが外れた → サイドチェーン消失、暴走しない。B10 定常ゲイン = しきい値と比どおり。

### C. D-132〜D-134 ボイスエフェクト
- C1 設定後に始まるボイスにだけ効く。C2 未設定のボイスはビット単位で従来どおり。
- C3 枠ちょうど=全部ウェット、1 つ超え=ドライ（終了しない）＋警告 LIMIT_REACHED/VOICE_EFFECT_SLOTS（object=playback）。
- C4 仮想化で枠を返す。C5 再利用で前の状態が漏れない。
- C6 テールなし（ボイス終了で切れる。停止フェードはエフェクトの後）。
- C7 順序 リサンプル→LP/HP→重い→パン。C8 複数段は slot 番号順。
- C9 playback の値: 次ブロックから、寿命中保持（後で実になるボイスにも）。古いハンドル INVALID_HANDLE、範囲外 INVALID_ARGUMENT。
- C10 chain=0 → API INVALID_ARGUMENT、4 可、5 は作成失敗。C11 bytes 小 → ドライ。C12 settings 1..65535、ちょうど/超え。
- C13 再生中に Event 設定を変える/外す → 鳴っているボイスは不変。C14 NONE で外す、未知種別 NOT_FOUND、count>16 INVALID_ARGUMENT。
- C15 プラグイン可。C16 ステレオのボイス。C17 ループ。C18 一時停止・再開。C19 スチールで枠が返る。
- C20 決定論・確保ゼロ・RT ゼロ・D-43（TSan）。C21 旧い大きさの snd_config → 既定値（api 側）。

### D. 標準 DSP 14 種（core 経由、物理量）
- D1 reverb RT60、Room/Reverb/Reflections -10000 でウェットなし、DryLevel -10000 でドライなし。
- D2 echo 遅延サンプル数と Decay 比。D3 delay Time / Feedback / WetDb / DryDb。
- D4 chorus Depth 0 で一定遅延。D5 flanger WetMix 0、くし形の谷。D6 distortion 頭打ち・OutputDb・Mix 0。
- D7 compressor 定常ゲイン。D8 limiter 天井と look-ahead 遅延。D9 parameq peaking/notch/allpass。
- D10 LP/HP カットオフ -3 dB。D11 pitchshift 周波数比。D12 normalize 持ち上げ。D13 surround Width 0 / 5.1 リア。
- D14 極端値で有限・有界（モノ/ステレオ/5.1）。D15 実行中変更が物理量で効く。

### E. 標準 LP/HP（全ボイス）: E1 randFilterHz、E2 Parameter カーブ、E3 距離カーブ。
### F. 負荷 LOD: F1 バイパス可のバスはテールが終わってからバイパス。
### G. dsp 単体: G1 オールパスの振幅 1・中心で -180°。G2 非有限パラメータで有限出力。

## 2. 性質 → テスト名（すべて新しいファイル。既存のテストは変えていない）

ファイル: core/test/indep_g2_fixture.hpp（G2Kind = 出力レートの解析的な信号源、Probe = 呼び出しを記録するプラグイン、RawFx = LiveTap::replay で生のコマンドを差す System）、core/test/indep_g2_{fxparam,voicefx,kinds,impl,config}_test.cpp、dsp/test/indep_g2_dsp_test.cpp。CMake は core/test・dsp/test の末尾に追記。

| 性質 | テスト |
| --- | --- |
| A1 A2 次のブロックから・1 本の直線・保持 | IndepG2Param.FadeIsOneLinearRampFromTheNextBlock, SpecHole_FadeSecondsToFramesRounding |
| A3 フェード中の新しい目標 | IndepG2Param.NewTargetDuringAFadeContinuesFromTheCurrentValue |
| A4 fade 0 でも段差なし、負の fade = 0 | IndepG2Param.ZeroFadeMovesInsideOneBlockWithoutAStep |
| A5 範囲で頭打ち | IndepG2Param.ValuesAreClampedToTheRange |
| A6 記録時の引数検査 | IndepG2Param.RecordTimeArgumentChecks |
| A7 未知のバス/番号/パラメータ → NOT_FOUND（object = バス ID） | IndepG2Param.UnknownTargetsWarnNotFoundWithTheBusId |
| A8 SND_ID_NONE = マスター | IndepG2Param.NoneIsTheMasterChain |
| A9 同じブロックに複数 | IndepG2Param.SeveralCommandsInOneBlockTheLastWins |
| A10 チェーン内の番号・片方のバスだけ | IndepG2Param.IndexInTheChainAndOneBusOnly |
| A11 再登録 | IndepG2Param.SpecHole_ValueAfterTheBankIsLoadedAgain |
| A12 variant 0 はビット一致・知らない variant は捨てる（LiveTap::replay で生コマンド） | IndepG2Param.BusVolumeVariantZeroIsUnchangedAndUnknownVariantsAreDropped, EffectParamApiDoesNotTouchTheBusVolume |
| A13 決定論（2 回・RT ワーカー 0/2） | IndepG2Param.DeterministicSerialAndParallel |
| A14 確保ゼロ・RT 違反ゼロ | IndepG2Param.NoAllocationAndNoRtViolation |
| B1 B10 B5 キーで圧縮量（しきい値と比どおり -12.06 dB）、同ブロック | IndepG2Side.KeyBusDrivesTheGainReductionInTheSameBlockWhenItComesFirst |
| B5 グラフ順で後なら 1 ブロック遅れ | IndepG2Side.KeyBusAfterTheTargetActsOneBlockLater, IndepG2Impl.SidechainFromThePreviousBlockRecoversWhenTheKeyStops |
| B2 UseSidechain 0 | IndepG2Side.UseSidechainZeroDetectsTheBusItself |
| B3 B4 無音・一時停止・停止で戻る / ミュート | IndepG2Side.SilentOrPausedKeyDoesNotReduce, SpecHole_KeyBusMutedByItsVolume |
| B6 DUCK 行 2 本 | IndepG2Side.SpecHole_TwoDuckRowsOnOneTarget |
| B7 自分自身がキー | IndepG2Side.SelfKeyedStaysBounded |
| B8 チャンネル数違い | IndepG2Side.MonoKeyBusSameReduction |
| F1 LOD のバイパスはテールの後 | IndepG2Lod.BypassWaitsForTheTail |
| C1 C13 設定後に始まるボイスだけ、NONE で外す | IndepG2Vfx.OnlyVoicesStartedAfterTheSettingAreAffected |
| C2 未設定のボイスはビット一致（プール無し/既定/他 Event に設定あり） | IndepG2Vfx.VoicesWithoutEffectsAreBitIdentical |
| C3 枠ちょうど・1 つ超え（ドライ＋警告、終了しない） | IndepG2Vfx.OneVoiceOverThePoolPlaysDryWithAWarning |
| C4 仮想化で枠を返す | IndepG2Vfx.VirtualisedVoiceReturnsItsSlot |
| C5 再利用で状態が漏れない（標準・プラグイン） | IndepG2Vfx.ReusedSlotDoesNotLeakThePreviousVoice, PluginVoiceEffect |
| C6 テールなし・停止フェードはエフェクトの後 | IndepG2Vfx.NoTailPastTheVoicesEnd, StopFadeAppliesAfterTheEffect |
| C7 パンの前（ピンポンがモノのまま） | IndepG2Vfx.EffectRunsBeforeThePan |
| C8 段の順（dsp で作った参照と比較） | IndepG2Vfx.StagesRunInSlotOrder |
| C9 再生ごとの値・あとで実になるボイス・引数・古いハンドル | IndepG2Vfx.PlaybackValueAppliesFromTheNextBlockWithoutAStep, PlaybackValueReachesAVoiceThatBecomesRealLater |
| C10 C11 C12 chain 0/4/5、bytes・settings の範囲、clamp、小さい bytes、表あふれ | IndepG2Vfx.ChainLengthLimits, PoolConfigRanges, StateTooLargeForTheSlotPlaysDry, SpecHole_SettingsTableFull, IndepG2Impl.SpecHole_PlaybackValueTableFull |
| C14 snd_set_event_voice_effect の引数 | IndepG2Vfx.EventVoiceEffectArgumentChecks |
| C15 プラグイン | IndepG2Vfx.PluginVoiceEffect |
| C16 C17 C18 C19 ステレオ・ループ・一時停止・スチール | IndepG2Vfx.StereoVoice, LoopingVoiceKeepsTheEffectStateAcrossTheLoop, PauseAndResume, StolenVoiceReturnsItsSlot |
| C20 決定論・確保ゼロ・RT 違反ゼロ | IndepG2Vfx.DeterministicSerialAndParallel, NoAllocationAndNoRtViolation |
| C21 旧い大きさの snd_config | IndepG2Config.OutOfScope_ConfigOfThePreviousLayout（記録だけ） |
| D1〜D15 14 種を core 経由で物理量 | IndepG2Kinds.*（RT60 = DecayTime、エコーの位置 5760 と比 0.4、ディレイ 3600 と Feedback、コーラスの固定遅延 576、フランジャーのくし形の谷、ディストーションの頭打ち・OutputDb・Mix、コンプのしきい値/比/メイクアップ/アタックの時定数、リミッターの天井と遅延 239、EQ の型 1〜8 の周波数応答、LP/HP の -3 dB と傾き、ピッチ比 2 / 0.5 と遅延 720、ノーマライズ、サラウンドの幅と 5.1 のリア遅延 720、極端な値を 1/2/6 ch） |
| E1 E2 E3 ボイスの標準 LP/HP | IndepG2VoiceFilter.ClipRandomFilterVariesPerPlayAndIsDeterministic, ParameterCurveDrivesLowpassAndHighpass, DistanceCurveLowpass |
| G1 G2 dsp 単体 | IndepG2Dsp.ParamEqAllpassBand, NonFiniteAndOutOfRangeRampsStayFinite（dsp_indep_g2_test と _scalar） |

## 3. 欠陥の一覧（Defect_、落ちる形のまま。すべて core/test/indep_g2_impl_test.cpp）

1. **IndepG2Impl.Defect_VoiceBackFromVirtualTakesTheEventSettingChangedAfterItStarted**
   期待（sonus.h: 設定は "the voices of `event` that start after this is applied"）: 10 ms のディレイで始まったボイスが仮想 → Event を 30 ms に変更 → 実に戻ると、10 ms のまま（戻りから 480 フレーム後に聞こえる）。
   出力: 戻りの 700..1400 フレームが 0（30 ms で鳴っている）。
   見立て: voice_fx.cpp Acquire が枠を取るたびに Event の「今の」設定から値を読む（種別だけ開始時に保存）。開始時の値も保存する必要がある。
2. **IndepG2Impl.Defect_VoiceBackFromVirtualLosesItsSettingWhenTheEventStageWasRemoved**
   期待: 仮想中に Event の段を外しても、開始済みのボイスは開始時の設定（10 ms のディレイ）で戻る。
   出力: 戻り直後（300..480）に 0.198（ドライで鳴っている）と警告 1 件。エントリが消えたので種別の既定値（Echo の MaxDelay 1 s）で枠を取ろうとし、160 KiB に入らずドライになった。
   見立て: 1 と同じ（Acquire が Event の表から値を読む）。
3. **IndepG2Impl.Defect_PlaybackValueRampsFromTheEventValueWhenTheVoiceBecomesReal**
   期待（sonus.h: 再生ごとの値は "kept for the playback's life (voices that become real later use it too)"）: ずっと前に値 0.25 を入れた再生のボイスが実になった最初のブロックから 0.25。
   出力: 最初のブロックの ParamBlock が 1.0（Event の値）→ 0.25 のランプ。
   見立て: Acquire が sl.end = base（Event の値）にし、VfxPrepareItem がそこから再生の値へランプする。枠を取ったブロックは start も再生の値にする。（戻りの短いフェードインで大部分は隠れるが、仮想スタートのボイスはフェードインなしで鳴り始める場合に聞こえる。）
4. **IndepG2Impl.Defect_EventVoiceEffectRefusedForAFullArenaIsPartlyApplied**
   期待（sonus.h 冒頭: SND_ERR_COMMAND_BUFFER_FULL は「記録時に捨てた」）: BUFFER_FULL を返した呼び出しは何も効かない。
   出力: 1 + 5 個のうち前の 5 個（SetSwitch と値 4 個）が記録・適用され、Event に不完全な設定（MaxDelay が既定）が付いてボイスがウェットで鳴る。
   見立て: recorder.cpp SetEventVoiceEffect が 1 個ずつ RecordVariant し、途中の失敗で戻る。先に必要なバイト数が入るか確かめて、全部入るときだけ書く。

（下書きで欠陥としたフェードの 1 フレーム短縮は、既存のバス音量の FadeFrames と同じ切り捨て規則だったので仕様の穴に移した。）

## 4. 仕様の穴（SpecHole_、現状を記録して通るテスト）

| 穴 | 現状 | 選択肢 | 勧め |
| --- | --- | --- | --- |
| 秒 → フレームの丸め（SpecHole_FadeSecondsToFramesRounding） | 切り捨て。6 ブロックを 6*(256/48000.0f) 秒で渡すと 1535 フレームになり、ブロックごとの減り幅が 256/1535、最後のブロックだけ短い（差 5.4e-4） | 切り捨て / 最近接 / 切り上げ | 最近接（lround）を全フェード共通で（バス音量・カテゴリ・スナップショットも同じ関数） |
| A11 バスのバンクを外して登録し直したときの実行中の値 | バンクの値に戻る | 戻る / 同じ ID のバスの値を保つ | 戻る（バス音量と違いエフェクトはバンクのインスタンス）。決定として書く |
| B4 サイドチェーンはフェーダーの前か後か | 後（キーバスをミュートすると圧縮しない） | 前 / 後 | 後で可。決定に明記 |
| B6 同じ対象に DUCK kind 0 が複数 | 最初の行だけ（2 本目のキーは効かない） | 最初 / 全キーのピークの最大 / 和 | 最大（どのキーでも下がるのが REACT 的に自然）。少なくとも buildtool で警告 |
| C12 Event の設定表あふれ | 記録は OK、適用時に警告 LIMIT_REACHED（object = event id）、そのイベントはドライ | 記録時に拒否 / 適用時の警告 | 現状で可。ヘッダに書く |
| 再生ごとの値の表あふれ | 記録は OK、適用時に警告 LIMIT_REACHED（object = playback）、値は捨てる | 同上 | 同上 |
| 入れ子 Event のボイス | 入れ子側の Event の設定が効き、外側（再生した Event）の設定は効かない | 外側 / 入れ子側 / 両方 | 決定に明記（ルーティングと同じ扱いにそろえる） |
| [init] パラメータ（MaxDelay など）の実行中変更 | 警告なしで受け付け、効かない（遅延は元の MaxDelay で頭打ち） | NOT_FOUND / 黙って無視 | 警告（NOT_FOUND か UNSUPPORTED）を出す |

## 5. 契約・公開ヘッダのコメントと実装の食い違い

- 上の欠陥 1〜4（sonus.h のコメントに対して）。
- commands.hpp の SetBusVolume.reserved / SetSwitch.reserved / SetParam.reserved に、使った variant の意味（bits 8-15 エフェクト番号、16-23 パラメータ番号、SetSwitch の reserved[1] 段・[2] 値の数）が書かれていない（core/src/fx_control.hpp にだけある）。ライブリンクの規則表とリプレイのために契約側に書くべき。
- config.h の voice_effect_bytes "at most 64 MiB" は、超えたときに snd_create が INVALID_ARGUMENT になると書いていない（実装はそうする。テストは通る）。voice_effect_settings 0 / 65536 も同じ。
- **api（持ち場の外への依頼）**: config.h「a shorter struct gets the defaults」に対し、ひとつ前の大きさ（出力の種類まで、sizeof 384）の snd_config で snd_create が SND_ERR_VERSION_MISMATCH（api/compose/config_check.cpp NormalizeConfig が 1.0/1.1/1.2 の 3 つの大きさと現在以上しか受けない）。さらに新しい max_voice_effects（offset 380）は旧い構造体の末尾の詰め物の中にあるので、旧い大きさを受け付けるようにしても memcpy で呼び出し側の詰め物（不定値）が max_voice_effects に入る。kConfigSize に「1.2 host 版 = offsetof(max_voice_effects)」を足し、コピーは offsetof までにする（または新しい項目の前に 4 バイトの予約を置く）。IndepG2Config.OutOfScope_ConfigOfThePreviousLayout が現状を記録。

## 6. 確かめられなかったこと

- B9（キーバスのバンクだけ外す）: DUCK 行と対象バスが同じバンクに必要で、別バンクに分ける形が作れなかった（実装も対象バスを定義するバンクの DUCK 行しか見ない）。
- C7 の「標準 LP/HP → 重いエフェクト」の順は線形どうしで可換なので音で区別できない（パンの前であることだけ確認）。
- 負荷 LOD の「入力を止めてから」: 実装は入力を止めず、テールが終わった時点でバイパスする（連続入力ではバイパスしない）。テールが切れないことは確認、入力の扱いは確認していない。
- 遅れチャンク（2.3）があると並列と直列はビット一致しない（Debug で一度観測、rt_spin_microseconds を大きくして回避）。2.5 の条件の外なので欠陥にしていない。
- 性能（14 章）のベンチは回していない（実装者の担当）。

### Defect_ の出力（gcc Debug、97bd7c8、core/test/indep_g2_impl_test）
```
../../home/user/sonus-wt/g2/core/test/indep_g2_impl_test.cpp:64: FAILED: mid ~= 0.5f * kCenter  (lhs: 0, rhs: 0.353553385, |diff| 0.354 > tol 0.001)
  A after its return, frames 700..1400: 0.0000 (10 ms setting: 0.3536, 30 ms: 0)
[  FAILED  ] IndepG2Impl.Defect_VoiceBackFromVirtualTakesTheEventSettingChangedAfterItStarted
../../home/user/sonus-wt/g2/core/test/indep_g2_impl_test.cpp:86: FAILED: early ~= 0.0f  (lhs: 0.198137224, rhs: 0, |diff| 0.198 > tol 0.0001)
  A after its return: early 0.1981, 700..1400 0.3536, warnings 1
[  FAILED  ] IndepG2Impl.Defect_VoiceBackFromVirtualLosesItsSettingWhenTheEventStageWasRemoved
../../home/user/sonus-wt/g2/core/test/indep_g2_impl_test.cpp:116: FAILED: first.start0 == 0.25f  (lhs: 1, rhs: 0.25)
  first block of the real voice: parameter 0 from 1.000 to 0.250
[  FAILED  ] IndepG2Impl.Defect_PlaybackValueRampsFromTheEventValueWhenTheVoiceBecomesReal
../../home/user/sonus-wt/g2/core/test/indep_g2_impl_test.cpp:158: FAILED: h.Mean(f0 + 8, f0 + kDelay) ~= 0.5f * kCenter  (lhs: 0, rhs: 0.353553385, |diff| 0.354 > tol 0.0001)
  warnings: 0
  E after the refused call: first frames 0.0000 (dry 0.3536, wet: 0)
[  FAILED  ] IndepG2Impl.Defect_EventVoiceEffectRefusedForAFullArenaIsPartlyApplied
  [spec hole] setting on the played (outer) Event: the nested voice is dry
  [spec hole] setting on the nested Event: the nested voice is wet
  [spec hole] MaxDelay set at run time: 0 warnings; the echo comes after 481 frames (480 = MaxDelay kept, 4800 = taken)
  [spec hole] third playback's value with a 2-entry table: level 0.3536 (0 = applied, 0.3536 = dropped), 1 warnings (code -14)
[==========] 8 test(s) run, 4 failed
```

## 7. 構成ごとの結果（先頭 97bd7c8。gcc Debug 全件だけ e933ff4 で回し、その後の 97bd7c8 は書式と確保関数の差しかないので足したテストを 97bd7c8 で回し直した）

| 構成 | 範囲 | 結果 |
| --- | --- | --- |
| gcc Debug | 全 221 件 | 220 通過（platform_device_play_test は skip）、落ちたのは indep_g2_impl_test だけ（中身は Defect_ 4 件のみ。RtViolation.AllocationInRtScopeFailsTheTest は枠組みの自己テストで想定どおり）。警告 0。97bd7c8 で足したテスト 7 本を再実行: impl 以外すべて通過 |
| ASan/UBSan | 全 221 件 | 同上（indep_g2_impl_test のみ失敗、サニタイザの報告なし） |
| TSan | 足したテスト 7 本 | 1 回目: impl のみ失敗。--repeat until-fail:20 -j1: 6 本 20 回とも通過。impl を 20 回: 毎回 Defect_ 4 件だけ失敗、TSan の報告なし |
| Release | 全 221 件 + 足したテスト repeat20 | 全件: impl のみ失敗。repeat20: 6 本通過。impl 20 回: Defect_ 4 件だけ |
| SIMD スカラー（-DSND_SIMD_SCALAR=ON） | 足したテスト + dsp_test / dsp_test_scalar | impl のみ失敗（8/9 通過） |
| AVX2（-mavx2 -mfma） | 同上 | impl のみ失敗（8/9 通過） |
| MinGW 構文検査 | 足した 6 ファイル | x86_64-w64-mingw32-g++-posix で警告・エラー 0（core 側はスレッドを使う indep_fixture.hpp を含むので posix 版。dsp のテストは win32 版でも 0） |

コミット: 17abdff, 08c5cb3, 968e400, e933ff4, 97bd7c8（先頭）。

---

## 再確認 1（実装 192bace、テストの先頭 dc9b368）

### 前提の確認
- `git diff 97bd7c8 192bace --stat` を見た。実装担当の差分は contract（commands.hpp / config.h / sonus.h）、core/src、core/test/core_fx_test.cpp だけで、私のファイル（indep_g2_*）は変わっていない。
- 読んだもの: 実装担当の報告（report-g2-impl.md の「差し戻し 1」）と sonus.h の新しいコメント。

### 決定に合わせて直したテスト（`Defect_` / `SpecHole_` を外した）
| 前の名前 | 新しい名前 | 検査すること |
| --- | --- | --- |
| Defect_VoiceBackFromVirtualTakesTheEventSettingChangedAfterItStarted | IndepG2Impl.VoiceBackFromVirtualReadsTheEventSettingAsItIsNow | 仮想の間に 10 ms から 30 ms に変えると、戻ったボイスは 30 ms で鳴る（戻りから 1440 フレームは無音） |
| （新規） | IndepG2Impl.KindChangedWhileVirtualComesBackWithTheNewKindAndItsValues | 仮想の間に種別を Echo から Probe に変えると、戻ったボイスは新しい種別と新しい値で鳴る（最初の呼び出しが 0.5→0.5、遅延なしで半分の大きさ）。古い種別に新しい値は入らない |
| （新規） | IndepG2Impl.EveryReturnReadsTheSettingAgain | 仮想と実を 4 往復し、その間に 20 ms → 外す → 5 ms → そのまま、と変える。戻るたびにその時点の設定で鳴る。警告 0 |
| Defect_VoiceBackFromVirtualLosesItsSettingWhenTheEventStageWasRemoved | IndepG2Impl.StageRemovedWhileVirtualComesBackDryWithoutAWarning | 外された段なしでドライ。警告 0、dry の数も増えない、枠も使わない |
| Defect_PlaybackValueRampsFromTheEventValueWhenTheVoiceBecomesReal | IndepG2Impl.PlaybackValueAppliesFromTheFirstBlockWhenTheVoiceBecomesReal | 最初のブロックの値が 0.25→0.25 |
| Defect_EventVoiceEffectRefusedForAFullArenaIsPartlyApplied | IndepG2Impl.EventVoiceEffectRefusedForAFullArenaIsNotApplied | BUFFER_FULL のとき何も効かない |
| SpecHole_InitOnlyParameterAtRunTime | IndepG2Impl.InitOnlyParameterAtRunTimeWarnsAndIsDropped, PluginParametersAreNeverInitOnly | [init] の実行中変更は 1 命令につき警告 1 件（INVALID_ARGUMENT、バス側は object = バス id、ボイス側は object = playback）で捨てる。同じ命令列のほかのパラメータは効く。Event の値としての MaxDelay は有効（40 ms → 1920 フレーム）。プラグインには [init] が無いので警告 0 で効く |
| SpecHole_PlaybackValueTableFull | IndepG2Impl.PlaybackValueTableFullWarnsAndDrops | 3 個目の再生の値で警告 LIMIT_REACHED 1 件（object = 3 個目の playback）、その値は捨てる |
| SpecHole_NestedEventVoicesTakeWhichSetting | IndepG2Impl.NestedEventVoicesTakeTheNestedEventsSetting | 子の Event の設定が効き、外側の設定は効かない |
| SpecHole_FadeSecondsToFramesRounding | IndepG2Param.FadeSecondsAreTruncatedToFramesLikeTheBusVolume | 各ブロックの先頭の値が 1 − 256k/1535 の線上にあり、6 ブロック目から 0。終わるブロックはバス音量の同じフェードと同じ |
| SpecHole_ValueAfterTheBankIsLoadedAgain | IndepG2Param.BankLoadedAgainStartsFromTheBankValues | 再登録でバンクの値（1.0）に戻る |
| SpecHole_KeyBusMutedByItsVolume | IndepG2Side.SidechainIsTakenAfterTheKeyBusFader | ミュートしたキーは 0 dB、音量 0.5 のキーは 6 dB 小さいキーと同じ量（しきい値と比の式どおり） |
| SpecHole_TwoDuckRowsOnOneTarget | IndepG2Side.OnlyTheFirstDuckRowFeedsTheSidechain | 2 行目のキーだけでは 0 dB、1 行目のキーで 1 本ぶん下がり、止めると戻る |
| SpecHole_SettingsTableFull | IndepG2Vfx.SettingsTableFullWarnsAndDrops | 記録は OK。適用時に警告 LIMIT_REACHED（object = event id）、そのイベントはドライ |

### 直しで変わったところを新しく突いたテスト（core/test/indep_g2_recheck_test.cpp）
- **IndepG2Recheck.AllOrNoneAtEveryArenaBoundary**: アリーナ（通常域 2048 B + 予約域 512 B = 32 B のコマンド 80 個ぶん）に k 個の詰め物を入れてから、6 個ぶんの呼び出しをする。k = 50..80 で通常域の境目と予約域を全部通る。
  - 入るとき（k ≤ 74）は OK で、全部の設定が効く。
  - 入らないときは BUFFER_FULL。何も効かず、残りの空きも減らない。
  - 通常域ちょうど、1 個足りない、予約域だけに入る、のどれも成り立つ。
- **IndepG2Recheck.AllOrNoneFromSeveralThreads**: 3 スレッドが同時に各自の Event へ 400 回ずつ記録する（ときどき Flush、所有スレッドは Update と描画を続ける）。どのスレッドも、最後に OK だった遅延がそのまま効いている（半端なし）。
- **IndepG2Recheck.FirstBlockHasThePlaybackValuesOnEveryStage**: 段 2 つに再生ごとの値が複数あっても、両段とも最初のブロックで start == end == 再生の値。
- **IndepG2Recheck.RawVariantOneEqualsThePublicCall**: commands.hpp の BusEffectParamReserved で作った生のコマンドは、公開関数とビット単位で同じ。エフェクト番号 200 は NOT_FOUND 1 件（object = バス id）。
- **IndepG2Recheck.ReacquireAndWarningsAreDeterministicAllocationFreeAndRtSafe**: 次を混ぜた台本で、2 回の直列実行と RT ワーカー 2 の実行が同じビット、初期化後の確保 0、RT 違反 0。
  - 仮想と実の往復
  - 戻る間の種別の変更と取り外し
  - [init] の警告
  - 再生ごとの値
- スチールで枠が移るときの枠の数は、既存の IndepG2Vfx.StolenVoiceReturnsItsSlot が引き続き通る。

### 新しく見つけた欠陥（`Defect_`、落ちる形のまま）
- **IndepG2Dsp.Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay**（dsp/test/indep_g2_dsp_test.cpp。dsp_indep_g2_test と dsp_indep_g2_test_scalar の両方で落ちる）
  - 期待: effects.hpp は Echo について "Delay is clamped to MaxDelay"、Delay について "Time is clamped to MaxDelay" と書いている。MaxDelay を超える値は MaxDelay ちょうどと同じ遅延になるはず（10 ms = 480 フレーム）。
  - 出力: Echo も Delay も、ちょうどなら 480、超えると 481（1 フレーム長い）。
    ```
    echo: at MaxDelay 480, above 481 | delay: at MaxDelay 480, above 481
    [  FAILED  ] IndepG2Dsp.Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay
    ```
  - 見立て: dsp の頭打ちが、遅延線の長さ（MaxDelay + 補間の余り 1 サンプル）で切っていて、MaxDelay の値そのもので切っていない。実装は 192bace 以前からあり、組② の差分ではない。core 経由では IndepG2Impl.InitOnlyParameterAtRunTimeWarnsAndIsDropped が 480 / 481 の両方を許している。
- 組② の core 側の欠陥: 残った `Defect_` は **0**。前回の 4 件は直っていることを確かめ、名前を変えた。

### 仕様の穴（新しく見つけたもの）
- 再生ごとの値の [init] の判定: 実装は「再生の Event の、その段の今の種別」で判定する。
  - ボイスが鳴っている間に Event の段の種別が変わると、ボイスの実際の種別ではなく新しい種別で判定する。
  - その結果、警告が要らないのに出たり、要るのに出なかったりしうる。実害は小さい（[init] の値はどのみち dsp が実行中に読まない）。
  - 勧め: 判定は値を使う側（枠の種別）で行うと明記する。
- 戻ったボイスの最初の音: 仮想から戻るのは Stop を適用した次のブロックで、そこから短いフェードインがかかる（テストはこの時刻に合わせた）。従来の振る舞い（組④の持ち場）なので記録だけ。

### 確かめられなかったこと
- 組④の範囲にある「戻りの時刻」と「停止フェードがブロック単位で伸びる」こと（10 ms のフェードが 512 フレームで終わる）は、組② の検査からは外した。

### 構成ごとの結果（テストの先頭 dc9b368、実装 192bace の上）
重いコマンドは 1 つずつ `flock /tmp/snd-heavy.lock`、`-j2`（TSan の繰り返しは `-j1`）で回した。

| 構成 | 範囲 | 結果 |
| --- | --- | --- |
| gcc Debug | 全 222 件 | 落ちたのは dsp_indep_g2_test と dsp_indep_g2_test_scalar の 2 本だけ。中身はどちらも Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay だけ。platform_device_play_test は従来どおり skip。警告 0 |
| ASan/UBSan | 全 222 件 | 同上。サニタイザの報告なし |
| TSan | 足したテスト 8 本 | 落ちたのは dsp の 2 本（Defect_ のみ）。core の indep_g2_* 6 本を `--repeat until-fail:20 -j1` で回し、全部通過。dsp_indep_g2_test を 20 回回して、毎回 Defect_ 1 件だけが落ち、ほかの失敗なし |
| Release | 全 222 件 + 足したテストの repeat20 | 全件は dsp の 2 本（Defect_ のみ）だけが落ちた。**jobs_rt_claim_steal_test は落ちなかった**（1 回の全件実行で通過）。repeat20 は 6 本通過。dsp を 20 回回して想定外の失敗 0 |
| SIMD スカラー | dsp_test・dsp_test_scalar・indep_g2_*（10 本） | 8/10 通過。落ちたのは dsp_indep_g2 の 2 本（Defect_ のみ） |
| AVX2（-mavx2 -mfma） | 同上 | 8/10 通過。同上 |
| MinGW 構文検査 | 変えた・足した 5 ファイル（fxparam, voicefx, impl, recheck, dsp） | x86_64-w64-mingw32-g++-posix でエラー・警告 0 |

**残った Defect_**: 1 件。dsp の IndepG2Dsp.Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay（組② の差分より前からある dsp の頭打ちの 1 フレームの差）。**core（組② の実装）の Defect_ は 0 件**。

先頭のハッシュ: **dc9b368**（wip/b-g2、ローカルコミットのみ、push なし）。

---

## 再確認 2（実装 b527335、テストの先頭 473e851）

### 前提の確認
- `git diff dc9b368 b527335 --stat` で見た変更は 4 ファイルだけ。私のファイル（indep_g2_*）は変わっていない。
  - dsp/src/fx_delay.cpp
  - dsp/test/effects_coverage_test.cpp
  - core/src/voice_fx.cpp（コメント）
  - sonus.h（コメント）

### テスト（dsp/test/indep_g2_dsp_test.cpp。dsp_indep_g2_test と _scalar の両方）
- **IndepG2Dsp.DelayAboveMaxDelayEqualsMaxDelay**: 旧 `Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay` を改名した。Echo も Delay も、MaxDelay ちょうどと超えた値がどちらも 480 フレームになり、通過。
- **IndepG2Dsp.ClampToMaxDelayAtAnyRateAndChannelCount**
  - 条件: 大きさを決めるレート → 動かすレートが 48000→48000 / 48000→24000 / 48000→44100 / 24000→48000 / 44100→96000。Echo と Delay、チャンネル数 1 / 2 / 6、遅延は MaxDelay ちょうど・直下（9 ms）・直上（37 ms）。
  - 結果: どの条件でも、直上はちょうどと同じ遅延。
  - 動かすレートが大きさを決めるレート以下のとき、ちょうど = MaxDelay のフレーム数（±1）で、直下はそれより短い。
- **IndepG2Dsp.DelayRampAcrossMaxDelayStaysInsideTheRing**
  - 条件: 2ch と 6ch、Echo はフィードバック 0.9、Delay はフィードバック 0.9 とピンポン。ブロックの中で遅延を 5→9.5→10.5→100→9.99→10.01→3→10→4000→1 ms とランプさせ、MaxDelay をまたぐ。
  - 結果: 出力は有限・有界。ASan でもリングの外の読みは出ない。100 ms で落ち着けた後の遅延は 480。
- **IndepG2Dsp.BatchEqualsSingleAcrossTheClamp**
  - 条件: 3 つのインスタンス（遅延 10 / 55 / 7.3 ms、ステレオ、ピンポン・フィードバックあり）。
  - 結果: EffectProcessBatch の出力は、1 つずつ EffectProcess した出力とビット単位で同じ。55 ms のインスタンスも 10 ms と同じ位置に繰り返しが出る。
- **MaxDelay 以下の遅延が従来どおりか、Chorus / Flanger / PitchShifter が変わらないか**
  - dsp_test・dsp_test_scalar のゴールデン（fx_*.sndg 14 本）がすべての構成で通った。
  - 以前からある私の遅延のテスト（core 経由のエコーとディレイの位置 5760・3600、ボイスエフェクトの 480 など）も全部通った。

### 残った Defect_
**0 件**（core も dsp も）。

### 新しい欠陥
なし。

### 仕様の穴・メモ
- **動かすレートが大きさを決めるレートより高いとき**（EffectInit の「入るだけに頭打ち」）
  - 遅延はリングで頭打ちになり、MaxDelay のフレーム数より短い（例: 24000 で大きさを決めて 48000 で動かすと 242〜253 フレーム。MaxDelay 10 ms なら 480）。
  - 直上 = ちょうど、という決定は守られている。
  - リングの長さはチャンネル数で 1〜11 フレーム変わる（1ch 253 / 2ch 245 / 6ch 242）。
  - core はレートを一致させて使う（D-48）ので、実行時には起きない。
  - 勧め: effects.hpp に「この場合の上限はリングの長さで、チャンネル数で数フレーム変わる」と書く。

### 構成ごとの結果（473e851）
重いコマンドは 1 つずつ `flock /tmp/snd-heavy.lock`、`-j2` で回した。

| 構成 | 範囲 | 結果 |
| --- | --- | --- |
| gcc Debug | 全 222 件 | 全件通過、警告 0 |
| ASan/UBSan | 全 222 件 | 全件通過（platform_device_play_test は従来どおり skip）。サニタイザの報告なし |
| Release | 全 222 件 | 全件通過（同じ skip）。jobs_rt_claim_steal_test も通過（失敗 0 回） |
| SIMD スカラー | dsp_test・dsp_test_scalar・indep_g2_*（10 本） | 10/10 通過 |
| AVX2（-mavx2 -mfma） | 同上 | 10/10 通過 |
| TSan | indep_g2_* 8 本を 1 回 | 8/8 通過 |
| MinGW 構文検査 | indep_g2_dsp_test.cpp | x86_64-w64-mingw32-g++（win32 版）でエラー・警告 0 |

先頭のコミット: **473e851**（wip/b-g2、ローカルコミットのみ、push なし）。

---

## clang の警告の修正（先頭 9e7b36e）
- clang Debug で `-Wunused-const-variable` が 2 件出ていた（core/test/indep_g2_voicefx_test.cpp の kNpos、core/test/indep_g2_impl_test.cpp の kB）。どちらも未使用なので消した。アサーションは変えていない。
- clang Debug（/tmp/snd-g2-clang）: 全体ビルドで警告 0。全 222 件のうち 221 件が通過し、1 件（platform_device_play_test）は従来どおり skip、失敗 0。
- gcc Debug: indep_g2_voicefx_test が 23/23、indep_g2_impl_test が 11/11 通過、警告 0。
- dsp/test/indep_g2_dsp_test.cpp とほかの indep_g2_* には、clang だけで出る警告は無かった。
