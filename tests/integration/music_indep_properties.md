# music 独立テスト: 検証すべき性質（実装を読む前に設計書・契約だけから作成）

入力: `spec/architecture/design.md` 2.5 / 8.1〜8.7、`contract/internal/include/snd/internal/music_if.hpp`・`common.hpp`・`commands.hpp`・`notify.hpp`、
`contract/public/include/snd/{sonus.h,types.h}`、`spec/interface/CONTRACTS.md`（D-05, D-18, D-20, D-21）、`spec/interface/{project,bank}-format.md`。

記号: SR = 内部サンプルレート、B = ブロック長、TPB = 960（tick / 拍）。期待値はすべて以下の式から解析的に求める。

## T. トランスポートと時間軸（8.1, D-20）

- T1 原点からの通し tick は 0 起点。`snd_musical_time` も bar/beat/tick すべて 0 起点、tick は 0..959。
- T2 一定テンポ bpm で、1 拍 = SR*60/bpm サンプル。ブロック境界 k*B の位置は `k*B*bpm*960/(60*SR)` tick（端数は tickFraction）。
- T3 拍子 n/d で 1 小節 = n 拍。小節番号は「拍子の変化点ごとに区切って」数える（変化点をまたいでも小節番号は連続し、各区間の小節長はその区間の拍子で決まる）。
- T4 テンポ変化点をまたぐ tick→clock 変換は区間ごとの積分（変化点より前は旧テンポ、後は新テンポ）。TicksToClock と ClockToTicks は互いに逆（±1 サンプル以内）。
- T5 Stopped / Paused のトランスポートは tick が進まない。共通クロックは止まらない。Paused→Playing で止めた位置から再開（拍がやり直されない、飛ばない）。
- T6 Stopped→Playing（`start_playing=0` で作ってから再生）では位置 0 から（または停止位置から — 仕様要確認）。
- T7 `SetTempo(bpm, when)` は when の位置から先のテンポを変える。変更点より前の tick↔clock 対応は変わらない（過去の拍時刻は保たれる）。
- T8 シーク: `snd_seek_transport(position)` 後、次ブロック先頭の位置が指定位置（bar/beat/tick を拍子から通し tick に換算した値）になる。
- T9 拍通知: Playing 中、各拍ちょうどのサンプル（`clock = TicksToClock(拍の tick)`）で 1 回だけ。ブロック境界ちょうどの拍も 1 回だけ（重複も欠落もしない）。a = bar, b = beat。小節頭は b = 0。
- T10 拍通知の clock は単調増加。N ブロック回すと通知数 = その区間に入る拍の数（解析値）。
- T11 拍通知の可聴推定時刻 `audible_delay_seconds` が `outputLatencySeconds` を反映する（MusicFrame の値を使う）。
- T12 単独トランスポートを `snd_destroy_transport` したら、そのトランスポート上の未発火の予約は `SND_END_DETAIL_TRANSPORT_GONE` で終わる。追随している Playback は止まらない。
- T13 Playback 所有のトランスポートは破棄できない（`snd_destroy_transport` が失敗するか無視される）。ハンドルは `0x80000000|playbackSlot` / Playback の世代で導ける（D-05）。
- T14 無効・破棄済みトランスポートを音楽系の When に渡すと即時扱い＋`SND_EVENT_WARNING`（detail = TRANSPORT_GONE）。
- T15 トランスポートのピッチを変えるとテンポと連動（ticksPerSample が pitch 倍）。

## R. 予約（クォンタイズ, 8.2）

- R1 NEXT_BEAT: 受理位置 p より厳密に後の最初の拍頭（p がちょうど拍頭のときの扱いは仕様の穴候補）。
- R2 NEXT_BAR: 次の小節頭。BARS_AFTER(N): 現在小節頭 + N 小節。N=1 は NEXT_BAR と一致。拍子変化をまたぐ場合は新しい拍子の小節長で数える。
- R3 GRID(k): k 分音符のグリッド（1 グリッド = TPB*4/k tick。k=3m の三連も可）。
- R4 offset_ticks: 量子化後に加算（負なら早める）。
- R5 予約は音楽時間で保持し、発火時に変換する: 受理後・発火前に SetTempo / シークがあれば、発火サンプルは新しい時間軸で計算した値と一致する。
- R6 発火サンプルの精度: FireEntry.fireClock = TicksToClock(目標 tick) をサンプル単位で一致（切り上げ/切り捨ては一貫）。先読み範囲内なら後のブロックのサンプルを持てる。
- R7 同じサンプルで発火する予約は sequence 順（2.5）。
- R8 最小先読み: 受理から発火まで「1 ブロック + プライム時間」が無い予約は「間に合わない時」規則に従う（既定 = 次のグリッドへ送る、deferrals 加算、ReservationLate 通知）。StartLate = いま発火して素材の途中から。Cancel = RESERVATION_MISSED で終了。
- R9 シークで目標を過ぎた予約: 時間軸に従う（目標が過去になれば遅れ規則、未来に戻れば待つ）。
- R10 Stopped / Paused 中のトランスポートの予約は発火しない（時間が進まない）。
- R11 予約表が満杯: RESERVATION_FULL で終わる（取りこぼしなく on_end が来る）。
- R12 CLOCK_SAMPLE / DELAY_US はトランスポート不要。指定サンプルで発火。

## S. 音楽の構造（8.3）

- S1 音楽 Event を Play すると Playback が 1 つトランスポートを所有し、初期 State の Playlist の先頭 Segment を鳴らす。
- S2 Segment の長さは bars 小節（Segment のテンポマップで）。Playlist の sequential は順に、loopCount 回（0=無限）で終わる。
- S3 random / shuffle の選択は Playback のシードから決まり、同じコマンド列なら同じ並び（2.5）。shuffle は 1 周で各エントリ 1 回（repeat 込み）。weight 0 の項目は選ばれない。
- S4 Stem の音量はパラメータ→カーブで決まり、SetParam の変化がランプで反映される（SetGain は目標値で止まる）。
- S5 遷移中、出側の余韻（exitFrame 以降）と入側の助走（entry 前のピックアップ）が重なる。助走はピックアップ分だけ頭拍より早く始まる。
- S6 PICKUP_ALIGN: 素材のピックアップマーカーが目標拍に揃う（開始サンプル = 目標 - ピックアップ長）。
- S7 マーカー通知: Segment のマーカーフレームを再生位置が通過したサンプルで 1 回（ブロック境界ちょうどでも 1 回）。
- S8 NEXT_MARKER(value=id): 現在の Segment のそのマーカー位置。0 = どのマーカーでも次のもの。
- S9 SEGMENT_END: 現在 Segment の終端（exit マーカー位置、無ければ bars 小節目の終わり）。

## G. 同期グループとストール（8.4）

- G1 1 つの音楽 Playback のボイス要求はすべて同じ syncGroup。
- G2 時間優先（既定）: ストール中もトランスポートは進み、拍通知はゲームとずれない（拍 clock がストール無しのときと同じ）。
- G3 音優先: データが来るまでトランスポートごと止まる。

## X. 遷移（8.5, 8.6）

- X1 ルールは MTRN の書いた順に照合し最初に一致した行。具体行が上、`*`（0）がワイルドカード。
- X2 Idle で同じ State を要求したら無視（ボイス要求ゼロ、通知なし）。
- X3 Pending で新しい要求 → 目標を上書き（最後の要求が勝つ）。ルールは新目標で引き直す（タイミングも新ルール）。
- X4 Pending で現在の State を要求した場合: 現在の State への遷移は「同じ State」なので遷移の取り消しになる（解釈・仕様の穴候補）。
- X5 Transitioning で割り込み可: いま聞こえている位置から新しい遷移を始める。
- X6 つなぎ完走後: 1 件だけ保留（最後の要求が勝つ）、完了後に処理する。保留の間に来た 2 件目以降は 1 件目を置き換える。
- X7 フェードの拍数: 出側は遷移元テンポ、入側は遷移先テンポで秒に換算（fadeSeconds = beats*60/bpm）。
- X8 開始位置: 先頭 / 同じ小節位置（小節番号 mod 遷移先 Segment 長の小節頭）/ 指定マーカー / 前回位置（そのStateを最後に離れた位置）。
- X9 つなぎ素材（bridge）があるとき: 遷移元→bridge→遷移先。bridge 完走で遷移先が始まる。
- X10 `snd_set_music_state` の When が即時ならルールのタイミング、それ以外なら When がタイミング列を上書き（D-21）。
- X11 非音楽 Playback に音楽 API → WRONG_EVENT_KIND（警告/エラー、Playback は続く）。
- X12 Stinger: グリッドに合わせて Play 予約。同じ Stinger の予約が重なったとき replace なら後の要求で置換（鳴るのは 1 回）、stack なら両方鳴る。

## D. 決定論・RT 安全（2.5, 3.3）

- D1 同じバンク・同じコマンド列（同じ適用ブロック）で、遷移の発生ブロック・選ばれた Segment・発火サンプルが一致（2 回実行して比較）。
- D2 負荷 LOD 無効・ストール無し・同一ビルドなら波形がビット一致。並列描画（ワーカー数を変える）でも一致。
- D3 オーディオスレッドの UpdateTransport / RunMusicLogic / CollectNotifications は確保ゼロ（rt_check）。
- D4 TSan で競合なし。
- D5 ボイス要求の容量が満杯なら次ブロックへ延期し、要求は失われない。
- D6 通知リングが溢れたら捨てて数える（音楽ロジックは影響を受けない）。

## 境界値

- B1 bpm の極値（非常に小さい / 大きい）、拍子 1/2・7/8・16 分。
- B2 予約の目標がブロック先頭ちょうど / ブロック末尾 -1 / ブロック末尾ちょうど（= 次ブロック先頭）。
- B3 tick 端数が累積しても拍の位置がずれない（長時間 = 数千ブロック）。
