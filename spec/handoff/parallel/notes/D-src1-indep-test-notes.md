# ソース組 1 独立テスト 報告（下書き）

## 性質（実装を読む前、/tmp/t-src-properties.md）
共通 C1〜C6（RT 安全・確保ゼロ・上限で VOICE_LIMIT、スロット再利用で状態が漏れない、D-43、統計、NaN を出さない）、
Oscillator O1〜O14（周波数、フーリエ係数と開始位相、帯域制限、直流、ブロック非依存、ピッチのランプのサンプル積分、
dt の丸め、デューティのサンプル補間と丸め、仮想・Seek・他の制御、ended なし、ウェーブテーブルの規則、全チャンネル同値）、
Noise N1〜N10（D-193 の白の列、傾き、RMS 0.577、シードでビット一致、ブロック・バッチ非依存、チャンネル無相関、
32.32 のピッチ、仮想・Seek、色 ≥3 拒否、ブラウンの直流）、Input I1〜I13（今から開始・ビット一致・遅延 0、アンダーラン、
オーバーラン、遅延の上限、1 ポート 1 ボイス、不明ポート、仮想で消費、制御、レート、snd_write_input の入口、SPSC、折り返し）、
core / C API E1〜E4。

## 足したテスト（source/test、CMake は末尾に追記）
- indep_src1_util.hpp（期待値の補助: 係数、FFT、Welch、D-193 の splitmix64 を文面から）
- indep_osc_test.cpp 13 件（うち SpecGap_ 2）
- indep_noise_test.cpp 9 件（うち SpecGap_ 1）
- indep_input_test.cpp 15 件（うち SpecGap_ 4）
- indep_src1_core_test.cpp 7 件（core 経由）
- indep_src1_load_test.cpp 3 件（上限・RT・確保ゼロ、ピンクの復帰費用、遅れたチャンク）
- indep_src1_api_test.cpp 3 件（C API、オーディオスレッドと書き手スレッド）
計 50 件。

## 欠陥
なし（Defect_ テスト無し、indep_defects_src1_test.cpp は作っていない）。

## 仕様の穴 / 決定への異議
1. D-194 Devirtualize（と Resume）の「今から」: 仮想中は既に消費済みなのに、直前に書かれた新しい 1 ブロックも捨てる → 復帰直後の 1 ブロックが無音 + starved + underrun に数える（書き手が歩調を合わせていても）。推奨: Devirtualize では飛ばさない（仮想の消費で十分）、または飛ばすとき最新の 1 ブロック分を残す、飛ばした直後のブロックはアンダーランに数えない。（SpecGap_DevirtualizeDropsTheFreshBlockAndCountsAnUnderrun）
2. 初めてフレームを渡すブロックが短いと、もう underrun（starved）に数える。「一度鳴らした後」の解釈。推奨: そのブロックより前に鳴っていた場合だけ数える。（SpecGap_FirstShortBlockCountsAsUnderrun）
3. InputPort::Configure の遅延の上限がブロックより小さいと、毎ブロック上限/2 しか残らず永久に starved。推奨: 下限をブロック長（または 2 ブロック）に丸める、または捨てたあと最低 1 ブロック分を残す。（SpecGap_LatencyBoundBelowABlockStarvesForever）
4. ボイスのチャンネル数 > ポートのチャンネル数の読み方が未規定（今は最後のチャンネルを繰り返す）。推奨: それを決定に書く。
5. 周波数 0（負・NaN・-Inf も 0 に写す）で位相が止まり、矩形は +1、パルスは 2−2d の全振幅の直流を出す。推奨: 無音（0）。（SpecGap_ZeroFrequencyGivesDc）
6. D-192 ループで表の長さを決めるが表の起点が未規定（loopStart > 0 でも表 0 は frame 0 から）。推奨: 起点 = loopStart、または「ループは 0 から」を buildtool で検査（D-02）。（SpecGap_WavetableLoopNotAtFrameZero）
7. D-193 色を白の RMS にそろえるので、ピンク・ブラウンのピークは約 2.8（白は 1 未満）。色でヘッドルームが変わる。決定として明記するか、ピーク基準の利得を選べるように。（SpecGap_ColouredPeaksExceedFullScale）
8. D-190 に書かれた帯域制限の数値は一般には成り立たない: 5.3 kHz で三角 −37.4 dB（決定は −46 dB 以下）、パルス −22.4 dB（決定は −23〜−33）。実装（2 点 PolyBLEP / PolyBLAMP）は正しく、数値の記述の問題。15 kHz ではノコギリ −16.3、パルス −14.9 dB。
9. D-193 の仮想からの復帰で 4096 標本から作り直す費用が、RT のスパイクになりうる: Release で 128 本のステレオのピンクが同じブロックで戻ると 2.36 ms（定常 0.255 ms の 9.3 倍、予算 5.33 ms の 44%）。256 本 × 8 ch なら単純計算で約 19 ms で予算超え。他の種別（Sample の seek は 1 ブロック 4 本まで）と違い上限が無い。推奨: 1 ブロックあたりの作り直しの本数に上限（残りは次のブロックまで無音で待つ）、または極ごとに必要な長さ（ブラウン ~1840、ピンク ~3900）に縮める。
10. Input の PCM に NaN / Inf が来るとそのままミックスに入る（D-194 は「そのまま出る」）。サニタイズするかは未規定（低）。
11. 契約上ありえない入力として: RenderItem::pitch / dutyOffset / tablePositionOffset が NaN だと Oscillator は NaN を出し続け、tablePositionOffset NaN は float→uint32 の未定義動作。core は pitch を丸めて渡し、他の 2 欄は今 0 なので到達しない。D-01 の配線の際に core 側で有限に丸めることを推奨。

## 机上で検証した並行部分
- InputPort の SPSC: 書き手は readPos を acquire で読み（空きは保守的にしか見えない）、データを書いてから writePos を release。読み手は writePos を acquire、読んでから readPos を release。読み手が先に進むことは無く、書き手が読み中の領域を上書きすることもない。SkipToLive / 遅延の上限による読み飛ばしも読み手側だけが readPos を書く。正しい。
- 読み手の交代（Start / Control / AdvanceVirtual はオーディオスレッド、RenderBatch は RT ワーカー）: readPos を relaxed で読むが、交代は core のワーカープールの同期（Run の完了）を経るので happens-before がある。D-43 で保留中のボイスに Control / Release は来ず、ポートを持つボイスが生きている間は別ボイスの Start も来ないので、遅れたチャンクと SkipToLive が同じポートで重なることはない。
- Oscillator / Noise / Input の RenderBatch が触るのは自分のインスタンスとそのポートだけ。Start / Release が書く空きリスト・portOwner はオーディオスレッドだけ。state は atomic_ref（release/acquire）、統計は atomic。ScheduleBackground は 3 種別とも何もしない。D-43 の約束を守っている。
- TSan で: 書き手スレッドと読み手（正確な勘定、引き裂かれたフレーム無し）、読み手の受け渡し、Start / Release と書き込みの同時、2 ポート 2 書き手、遅れたチャンクとオーディオスレッドの操作、core（RT ワーカー 2）と書き手、C API のオーディオスレッドと snd_write_input。いずれも検出無し（-j1 で 20 回繰り返し）。

## 構成ごとの結果
- gcc Debug: 全体のビルド（警告ゼロ）と ctest -j2 で 223 件すべて通過（platform_device_play_test は環境で Skipped）、先頭 3dc4470
- -DSND_SANITIZE=address,undefined: 12 本 通過
- -DSND_SANITIZE=thread: 12 本 通過、並行の 4 本を --repeat until-fail:20（-j1）通過
- Release: 12 本 通過、並行の 4 本を --repeat until-fail:20 通過
- MinGW -fsyntax-only -Werror（プロジェクトの警告一式）: 新しい 6 ファイル 通過

## 処理時間（Release、256 フレーム = 5.33 ms）
- 種別ごとに上限 256 本（RenderBatch のみ）: Oscillator 8ch 568 us、Noise 8ch 4248 us（予算の 80%）、Input 2ch 42 us
- 128 本ステレオのピンク: 定常 255 us、仮想から一斉に戻るブロック 2360 us
- core 経由 150 本の合成ボイス: 260 us / ブロック
- 実装者の負荷テスト: 128 本ステレオで osc 248 us、noise 535 us、input 12 us
- core 経由の入力の遅延: 定常・一時停止後・仮想後とも 16 フレーム（リサンプラの遅延）

## 検査できなかった性質
- 入力のレートが内部レートと違う場合の core 側（依頼 D-03 未実装）、Parameter → デューティ / テーブル位置（D-01 未実装）、snd_config からのポート設定（D-05）、実デバイス（D-04）。
- 実際に遅れたチャンク（D-43 の保留）を core で起こしての検査: 決定的に起こせないため、種別の単位で別スレッドの RenderBatch とオーディオスレッドの操作の同時実行で代えた。
- RT ワーカーの有無でビット一致（2.5）: スレッドの時間でフェードが入りうるので入れていない（決定的な駆動での再現は確認）。
- 時間ベースの RT スパイク（仮想からの復帰）は Defect_ にせず数値の報告のみ（環境依存）。
