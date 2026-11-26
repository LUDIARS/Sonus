# dsp 組 独立テスト: 気づいた点の記録（随時追記）

## 机上（実装を読んで）
- [穴/文書] convolution.hpp の ConvolutionIrDesc のコメント「Kaiser-windowed sinc, 64 zero crossings」は D-213 と実装（fx_convolution.cpp:45,112 Blackman・片側 16 零点）と食い違う。ヘッダの誤記。
- [穴/文書] effects.hpp の EffectLatencyFrames のコメント「0 for the others」に convolution（ZeroLatency=0 で B）が無い。
- [異議] EffectReset / NaN 回復は effects.cpp ReinitKeepingParams → InitWith で状態全体（MB 単位）を memset し FftPlanInit（三角関数）も走る。fx_convolution の ConvolutionReset（GoDormant, O(C·B)）は EffectReset からは使われない（kOps.reset は Layout/Init 後に呼ばれない）。
- [観察] dormant 中に NaN を含む入力: peak 計算で NaN が無視され peak==0 → 無音を出す（その後、枠組みが NaN を検出して全体を作り直す）。
- [穴/品質] タイムストレッチの探索はチャンネルの和（ダウンミックス）の相関（time_stretch.cpp:155-164）。逆相のステレオ（L = -R）では和が 0 → 全候補の score 0 → 常に lo を選ぶ。
- [低] TimeStretchInputFrames / Process の need は uint32 へ切り詰め（time_stretch.cpp:276,284）。outFrames ~1e9 超で巻き戻り → 範囲外読み。現実的な大きさでは起きない。

## タイムストレッチ（テストで確認）
- [欠陥 1, 低] NaN の maxSpeed を 4 として扱う（D-210 は 1）。Defect_TimeStretchNanMaxSpeedIsOne。time_stretch.cpp:53（Shape の ClampMs の既定値）と :234（Init）。pad 4384 vs 2224、状態 46976 vs 29696 B、速度 2 が 1 に切り詰められない。
- [欠陥 2, 高] 逆相ステレオ（R = −L）で探索が働かない。Defect_TimeStretchAntiPhaseStereoLosesAlignment。time_stretch.cpp:155-164 が全チャンネルの信号の和で相関を取る → 和 0 → score 0 → 常に lo。220 Hz の残差: モノ 0.005 / 逆相 L 1.50（速度 1.5）、0.011 / 0.060（0.7）。
- [異議] 過渡: クリック列（100 ms 間隔、39 個）。速度 0.5: 39/39 が 2 重、0.8: 14/39 が 2 重、1.5: 4 個消失 + 13 個が 0.25〜0.7 に減衰、2: 13 消失 + 13 減衰、4: 27 消失。D-209 の「過渡を崩さず」は WSOLA（固定の窓・ホップ）では成り立たない。過渡検出（過渡のホップは自然な続きを優先、あるいは過渡位置にホップを合わせる）を足すか、決定の文言を直す。
- [異議] 50 Hz: 探索 ±8 ms（16 ms）が 1 周期（20 ms）より短く、窓 30 ms も 1.5 周期しかない。包絡は −0.9〜+0.35 dB だが、正弦との残差は 0.16〜0.27（約 −12 dB の歪み、速度 0.25/0.5/0.8/1.5）。100 Hz 以上は残差 < 0.03。低音を含む BGM では探索を ±10 ms 以上（50 Hz の半周期 + α）にするのが妥当。
- [文書] 「長く回したときの消費量は（速度の積分）+ 先読み ± 1 ホップ」の「1 ホップ」は入力の進み（speed·H、最大 4H = 2880）で、出力のホップ H ではない（実測は speed·H 以内）。
- [穴] ランプの端点に ±Inf を渡すと（s0 = +Inf, s1 = 1）、Inf − Inf で全フレーム NaN → 速度 1 になる。端点を先に切り詰めるか（4→1 のランプ）、仕様に書くか。
- 速度・振幅・位相・ブロック分割・ランプ・Reset・ガード・8 kHz〜384 kHz・8ch は仕様どおり（テスト通過）。

## コンボリューション（テストで確認）
- [欠陥 3, 中] IR の切り替え（IrSlot の変更・登録の解除）で、FDL の部分が 1 分割（最大 B フレーム）遅れて古い IR のまま出る。Defect_ConvolutionIrSwitchLagsOnePartition。fx_convolution.cpp Step（:327-369）が次の周期の OutBuf を周期の終わりに古い IR で計算し、ConvolutionProcess（:627）はそれを足すだけ。直接形の先頭は呼び出しの境目で新しい IR になるので、B フレームの間「新しい IR の先頭 + 古い IR の後ろ」の混ざった IR になる（ブロック 1024・B 64 で切替直後の誤差 1.6）。登録を外したあとも B フレームは古い IR の尾が鳴る（メモリは読まない＝安全だが、仕様の「呼び出しの境目で切り替わる」と違う）。直し方: 切り替えを検出したら（IR のポインタを状態に覚え、変わったら）その周期の OutBuf を新しい IR で作り直す、または仕様を「最大 B フレーム遅れる」に直す。
- [欠陥 4, 中] max_frames より長い呼び出しで IrSlot が補間される。Defect_ConvolutionIrSlotRampsInsideLongCalls。effects.cpp ProcessCore（:275-296）がサブブロックの境目で全パラメータを補間し（IrSlot も、整数のパラメータも）、ConvolutionProcess は p.e[kConvIrSlot] をサブブロックごとに引く → 1 → 3 の変更で間の枠 2（他のバンクの IR でありうる）が鳴る（ピーク 4.0）。直し方: IrSlot（整数・非ランプのパラメータ）はサブブロックでも呼び出しの終わりの値に固定する。
- [穴] 準備で IR のサンプルの NaN / Inf を拒否しない（ConvolutionPrepareIr / FromBlob、SCIR の検証も gain だけ）。NaN の IR を使うと、出力が非有限 → 枠組みが 0 にして状態全体を作り直す（MB の memset）を毎周期くり返し、DryMix の分も消える。推奨: Prepare（とブロブの検証）で非有限のサンプルを拒否（nullptr）。
- [穴] ZeroLatency 1 のとき実際に使う IR の長さは (cap+1)·B（直接形の B タップ + FDL の cap 分割）で、ConvolutionStateCapacityFrames（cap·B）より B 長い（MaxIrMs 10 ms・B 64: 容量 512、使用 576）。「容量を超える分は使わない」（D-214）と食い違う。害は無い（テールは cap·B + 2B で足りる）が、どちらかに揃える。

## 設計上の点（意見）
- 登録表がプロセスに 1 つ（1024 枠、ロックなし）: 机上の並行性は正しい（Register は CAS acq_rel で最小の空き枠、Unregister は exchange、RT は acquire load。IR のメモリは Register の release より前に書かれるので RT から見える。データ競合なし、TSan の RegistryConcurrency で検出ゼロ）。問題は寿命と所有: (1) 枠に所有者が無く、どのコードでも任意の枠を Unregister できる。(2) IrSlot はパラメータ（0..1024、ライブリンク・スナップショットで書ける）なので、別の snd_system / 別のバンクの IR を指せる。複数の snd_system が同じプロセスにあると、system A のバンクを外すときのエポックは system B のオーディオスレッドを待たない → B のインスタンスが A の IR の枠を指していると解放後の参照（Resolve は ir->magic を読む）。(3) テストの順序依存（実装者の Registry テストは「他に登録が無い」ことを前提にしている）。(4) 他のモジュールは状態を snd_system / 呼び出し側のメモリに持つ（D-44 のプール、3.3）方針で、プロセス大域の可変状態はこれが初。推奨: 表をシステムごと（core が IrTable を持ち、EffectProcess / ProcessEffectChain に表のポインタを渡すか、RenderContext 経由で渡す）。最低限、枠に世代番号（IrSlot = 枠 + 世代）を入れて古い番号を無効にし、IrSlot をライブリンクで読み取り専用にする。
- EffectReset / NaN 回復の memset: 2 秒ステレオ（1.55 MB）の EffectReset は Release で測定値（下の表）。ブロックの予算 5333 µs に対して小さいが、NaN の IR（穴）や NaN を出し続けるソースでは毎ブロック走る。ConvolutionReset（O(C·B) の GoDormant）が既にあるので、作り直しを「ヘッダとプランはそのまま、GoDormant + f/w/valid の初期化」にできる（プランは不変、FDL は valid で無効化済み）。推奨: Reset / NaN 回復では kind の reset フックを使い、全体の memset と FftPlanInit をやめる。
- IR の切り替えにクロスフェードが無い: 欠陥 3（1 分割の混ざり）と合わせて、切り替えでクリックが出る。ヘッダは「2 つのインスタンスでクロスフェード」と言うが、core 側に仕組みが無い。推奨: 欠陥 3 を直したうえで、IR 切替時に 1 分割（B フレーム）の線形クロスフェード（古い IR の出力と新しい IR の出力を両方計算、その周期だけ 2 倍の負荷）を入れる。少なくとも決定に「クリックが出る」と書く。
- WSOLA の定数: 窓 30 ms / 探索 ±8 ms は声と中高域には十分（220 Hz 以上で包絡 ±0.3 dB、残差 < 0.03）だが、50 Hz では探索幅 16 ms が 1 周期 20 ms に届かず残差 0.16〜0.27。先読み 1824 フレーム（38 ms）は S + 2H から決まり妥当。推奨: 探索を ±10〜12 ms（最低の周期の半分 + α）にするか、低域用に窓 40 ms を選べるようにする（API は既に windowMs / searchMs を取る。D-10 の core の配線で既定を変えられる）。過渡の扱い（上記）とステレオの和（欠陥 2）の方が影響は大きい。
