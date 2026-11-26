# D-06 パッチプログラムのバンク化（C・統合担当）

対象は buildtool の合成クリップ書き出し、bank-format.md の PTCH 節、project-format.md の synth 定義。既存の SYNP.programOffset / programBytes は PTCH チャンク先頭からのバイト単位。以下を bank-format.md の形式定義とする。版は個々のプログラムの版で、バンクの版は維持する。

| オフセット | 型 | 内容 |
| --- | --- | --- |
| 0 | u8[4] | ASCII PTCH |
| 4 | u16 LE | 1 |
| 6 | u16 LE | N、2〜16（Output に入力が必要） |
| 8 | u16 LE | 出力番号 N−1 |
| 10 | u16 LE | 予約 0 |
| 12 + 20i | u8 × 4 | 種類、入力 a、入力 b、波形。入力無しは 255 |
| 16 + 20i | binary32 LE × 4 | 引数 x0〜x3 |

各スライスは厳密に 12+20N バイト。行・スライスにアラインメントの前提はない。出力は最後に 1 つ、すべてのノードが出力につながる。入力番号は自身より前だけ。使わない入力は 255、引数は 0、Oscillator 以外の波形は 0。非有限の値は全引数で不許可。

| 種類番号 | JSON 名 | 入力 | 許される引数を順に記す |
| --- | --- | --- | --- |
| 0 | constant | なし | 値 [−16,16] |
| 1 | oscillator | なし | 周波数 [0,192000] Hz、duty [0,1]。波形 0 sine、1 saw、2 square、3 triangle、4 pulse |
| 2 | whiteNoise | なし | なし |
| 3 | add | a,b | なし |
| 4 | multiply | a,b | なし（gain には定数を b に結ぶ） |
| 5 | lowpass | a | カットオフ [0,192000] Hz |
| 6 | adsr | なし | attack 秒 [0,60]、decay 秒 [0,60]、sustain [0,1]、release 秒 [0,60] |
| 7 | lfo | なし | 周波数 [0,200] Hz |
| 8 | randomHold | なし | 更新周波数 [0,200] Hz |
| 9 | clip | a | 対称クリップ閾値 [0,16] |
| 10 | output | a | なし |

project-format.md に追加する案。nodes は ID つきで記述し、配列順には依存しない。入力は ID、output は出力ノードの ID。gateFrames は出力サンプル単位のノート長で 0 は無期限、既定 0。

```json
{"synth":{"kind":"patch","channels":2,"gateFrames":0,"output":"out","nodes":[
  {"id":"vco","op":"oscillator","waveform":"sine","args":[440,0.5]},
  {"id":"g","op":"constant","args":[0.25]},
  {"id":"amp","op":"multiply","inputs":["vco","g"]},
  {"id":"out","op":"output","inputs":["amp"]}
]}}
```

buildtool/src/build.cpp の kind==3 診断を置換する。ID の重複、未知参照、循環、孤立、出力の多重、型・範囲を行・列つきで診断する。依存順を安定に決める（同順位は宣言順）、入力を番号へ置換して PTCH に連結し、SYNP に範囲を記録する。出力側でも programOffset ≤ blobBytes、programBytes ≤ blobBytes − programOffset と検証する。参照実装は source の BuildPatchProgram / ValidatePatchProgram。buildtool の依存先に source を追加しない場合はこの表で符号化し、共通 fixture で参照実装とバイト一致を検査する。丸め・ノードの信号処理は D-205〜D-206。
