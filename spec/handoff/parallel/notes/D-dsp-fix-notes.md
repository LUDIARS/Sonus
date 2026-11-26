# dsp 組 差し戻し（D-217〜D-219）作業メモ
- 起点 3758bfc。
- IrSlot の件: effects.cpp の ProcessCore は LoadParams で h.cur[i] = 呼び出し全体の終わりの値 を先に入れてから
  サブブロックに分ける。fx_convolution は p.e ではなく h.cur[kConvIrSlot] を読めば、サブブロックでも中間の値を取らない（確実）。
- 番号: n = 1..2^24-1 のカーソル割り当て、枠 = n & 1023（世代 = n >> 10）。連続登録は連続番号になる（Defect 4 の前提を満たす）。
- 切り替えのクロスフェードは [f, B)（周期の残り）。古い側は既に計算した OutBuf と状態に写した先頭タップだけ。
  Defect_ConvolutionIrSwitchLagsOnePartition は切替直後の 64 フレームも新しい IR と一致を求める → D-218 のクロスフェードと食い違い（報告）。
- D-217 実装 済（Defect 1, 2 通過）。50 Hz の残差 0.0000、55 Hz 0.0013、73 Hz 0.0018、220 Hz 0.0052（窓 960 の最悪）。
- D-218/219 実装 済（コミット済）。Defect 4 通過、Defect 3 はフェードの区間（切替から 64 フレーム）だけ不一致 → 決定との食い違いとして報告。
- 自分のテスト: IrSwitchCrossFades / UnregisterFadesOutWithoutReadingTheIr / NonFiniteIrIsRefused / LongCallUsesTheEndSlot を追加、Registry を順序非依存に。
- 残り: convolution.hpp の文書、CONTRACTS D-217〜219 と既存の直し、依頼 D-10/D-11/D-12、indep の落ちる一覧、構成、計測。
- indep の失敗（gcc Debug、両方の実行ファイル同じ）:
  - IndepTimeStretch.ShapeFormulas（search NaN の既定 8→12: S 12 / LA 44 を 8 / 40 と期待）
  - IndepTimeStretch.BackToSpeedOneIsAConstantOffset（|offset| ≤ 384+1 を前提、実測 461 ≤ 576）
  - IndepTimeStretch.RampFollowsOwnSimulation（自前の模擬が S=384 固定、差 192 = 576-384）
  - IndepTimeStretch.BitExactChecksumAcrossConfigurations（探索の変更でビットが変わる）
  - IndepConvolution.EffectInfoAndParameters（IrSlot の最大 1024 → 16777215）
  - IndepConvolution.PartitionRuleAndStateSize（容量 96000 → 96256 = +B）
  - IndepConvolution.TailExactAndWakeUp（容量/B を cap とみなす → 24 で 1 分割ずれ、以降の判定も連鎖）
  - IndepConvolution.Registry（番号 1..1024 の順を前提、外し損ねで表が満杯のまま残る → 同じ実行ファイルの後続に影響）
  - IndepConvolution.RegistryConcurrency（番号で配列 [1025] を添字 → 範囲外で SegFault）
  - コンパイルできないファイルは無い。
