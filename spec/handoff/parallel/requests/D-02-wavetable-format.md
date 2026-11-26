# D-02 ウェーブテーブルの資産の形（buildtool / バンク形式への依頼）

依頼元: セッション D（ソース組 1、D-192）。宛先: C（buildtool）、統合担当（バンク形式の列の追加）。

## 背景

Oscillator の種別はウェーブテーブルを、core が `OscillatorParams::wavetable` に渡す SYNP の資産（`SYNP.asset`）から読む。
バンク形式 2.0 にはテーブルの長さも枚数も無いので、source は次の規則で決めている（D-192）。

- 資産はメモリに常駐する PCM（`loadForm: "pcm"`、PCMF か PC16）に限る。それ以外は開始を失敗させる。
- 1 枚の長さ T: 資産にループがあれば `loopEnd − loopStart`、無ければ全フレームが 2048 の倍数なら 2048、それ以外は全フレーム。
- 読むのはチャンネル 0。帯域制限は無い。

## 依頼の中身

1. **buildtool の検査（C）**: `"kind": "wavetable"`（と `"oscillator"` に `asset` を付けたもの）で、資産の `loadForm` が `pcm` でない、
   またはコーデックが PCMF / PC16 でないときはビルドのエラーにする（今はランタイムで開始が失敗するだけで、気づきにくい）。
   `"kind": "oscillator"` に `asset` を付けるとウェーブテーブルとして鳴る（core が種別を区別せずに資産を渡すため）ことも、
   警告かエラーにしてほしい。
2. **テーブルの長さの列（統合担当 / C）**: SYNP に `tableFrames`（U32、0 = 上の規則）を足し、`"synth": {"tableFrames": 2048}` を
   書けるようにする。core は `OscillatorParams` にそのまま写せる欄が無いので、`OscillatorParams` の末尾に `tableFrames` を
   足す（内部の契約の追加。source は 0 なら今の規則を使う）。
3. **（任意）ミップのレベル**: 高い周波数で倍音が折り返さないよう、buildtool がテーブルごとに 1 オクターブずつ帯域を
   落としたレベル（例えば 10 段）を作って資産に並べる形を決める。source はレート比から段を選んで読む。メモリはおよそ 2 倍。
   形が決まったら source の組で読み側を作る。
