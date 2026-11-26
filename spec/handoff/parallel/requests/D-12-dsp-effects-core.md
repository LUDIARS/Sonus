# 依頼 D-12: dsp の既存の行の変更案（`effects.cpp` / `effects.hpp`、dsp を持つセッション B）

依頼元: セッション D（dsp 組）。決定: CONTRACTS.md「段階 7-D」の D-218、D-219。
D は既存の dsp の行を変えられないので、変更案だけを書く。どれも振る舞いの約束（`EffectReset` は新しい `EffectInit` と同じ状態）を変えない。

## 1. `EffectReset` と NaN からの回復で、コンボリューションは種別の Reset を使う

今: `effects.cpp` の `ReinitKeepingParams`（377 行目付近。`EffectReset`（463 行目）と `ProcessOne` の `if (bad) ReinitKeepingParams(state);`
（321 行目）から呼ばれる）は、どの種別でも `InitWith` で状態全体（`layout` の大きさ）を memset し、種別の `init` を呼ぶ。
コンボリューションでは状態が MB 単位（2 秒ステレオ・B=256 で 1,581,632 バイト）なので、memset と FFT の計画の作り直し（三角関数）で
平均 72.2〜228.7 µs かかる（独立テストの遅延 0 / B の測定、GCC 14.2 Release）。NaN を出し続けるソースでは毎ブロック走る。

案（`ReinitKeepingParams` の先頭に足す）:

```cpp
void ReinitKeepingParams(void* state) noexcept {
  FxHeader& hd = *Header(state);
  if (hd.kind == static_cast<std::uint32_t>(EffectKind::Convolution)) {
    // O(C B): the kind's reset leaves exactly the state of a fresh Init (the FFT plan and
    // the layout do not change; the input-spectrum delay line is invalidated, not cleared).
    kOps[hd.kind].reset(state);
    hd.silentFrames = hd.tailFrames;  // as InitWith: nothing sounding
    hd.cleared = 1;
    return;
  }
  const FxHeader h = *Header(state);
  ... (今のまま)
}
```

- `fx_convolution.cpp` の `ConvolutionReset` は D のブランチで「新しい `ConvolutionInit` と同じ状態（FFT の計画を除く、計画は不変）」に
  直してある: 入力の履歴と FDL の出力を 0、FDL の有効な分割数を 0（FDL 自体は消さない。有効数で無効にする）、周期の位置 0、
  使用中の IR なし（次の呼び出しですぐ使い始める）、クロスフェードなし。パラメータ（`h.cur`）と遅延・テールの長さはヘッダに残る。
- 効果の見込み: 状態全体の memset と FFT の計画を O(C·B) の 0 埋めに置き換える（ステレオ・B=256 で 8,256 バイト。導入時に実測する）。
- 他の種別も、状態が大きいもの（リバーブ、エコーの遅延線）は同じ形にできるが、今は今のままでよい。
- 確かめるテスト: `Effects.ResetEqualsFreshInit` と `RecoversFromNonFiniteInput`（既存）に `EffectKind::Convolution` が含まれれば足りる
  （`RecoversFromNonFiniteInput` は全種別を回す。`ResetEqualsFreshInit` は種別の一覧が固定なので足す）。

## 2. `effects.hpp` の `EffectLatencyFrames` のコメント

222 行目: `// Latency in frames: limiter look-ahead - 1, pitch shifter WindowMs / 2, 0 for the others.`
→ `// Latency in frames: limiter look-ahead - 1, pitch shifter WindowMs / 2, convolution B with`
   `// ZeroLatency 0 (else 0), 0 for the others.`

## 3. `ProcessCore` の順序を守る（変更ではなく注意）

`fx_convolution.cpp` は、`max_frames` より長い呼び出しでも IrSlot の中間の値（他の IR の番号でありうる）を引かないために、
サブブロックに渡される補間した値ではなく、ヘッダの `h.cur[IrSlot]`（`LoadParams` が書く、呼び出し全体の終わりの値）を読む（D-218）。
これは `ProcessCore` が「`LoadParams`（`h.cur` を呼び出しの終わりの値にする）→ サブブロックに分けて `ProcessKind`」の順であることに依る。
`ProcessCore` を変えるときはこの順序を保つか、整数・非ランプのパラメータ（`kParamInteger`）をサブブロックでも終わりの値に固定すること。
後者にすると、他の整数のパラメータ（ParamEq の Type、Compressor の UseSidechain）もサブブロックで中間の値を取らなくなり、より素直になる:

```cpp
// ProcessCore のサブブロックのループの中
for (std::uint32_t i = 0; i < info.paramCount; ++i) {
  const bool hold = (info.params[i].flags & kParamInteger) != 0u;
  q.s[i] = hold ? p.e[i] : p.At(i, t0);
  q.e[i] = hold ? p.e[i] : p.At(i, t1);
}
```

（ParamEq の Type などは今、長い呼び出しの途中で中間の型を取りうる。音は壊れないが、意図しない型のフィルタが一瞬掛かる。）
D のテスト `Convolution.LongCallUsesTheEndSlot` がこの性質を見ている。

## 4. MinGW で共有テストの整列確保を使えるようにする

`dsp/test/dsp_test_util.hpp:31` で使う `std::aligned_alloc` は MinGW-w64 の標準ライブラリにはない。
共有ヘルパーの所有者に、Windows では `_aligned_malloc` と `_aligned_free` を対にし、他の環境の整列・解放の契約を保つ実装を依頼する。
