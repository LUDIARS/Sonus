# physics の独立テスト: 満たすべき性質（実装を読む前に書いた一覧）

出典: design.md 9 / 12.1 / 13 / 14、CONTRACTS.md（D-01, D-02, D-28, D-29, D-32, D-42, D-102〜D-104）、
bank-format.md の PHYS、project-format.md の materialTable、公開ヘッダ（`snd/physics.h(pp)`、
`snd/physics/physics.h(pp)`）、受け入れ側の決定 A〜E（依頼文）。
`physics.h` 冒頭の規則は「実装者の主張」として扱い、決定 A〜E に無いものは「未決（実装者の案）」と書く。
テストは `physics/test/indep_*.cpp`（ctest 名 `physics_indep_*`）。欠陥は `Defect_` で始まる名前。

## A. API・生成・資源

- P01 `snd_physics_create` / `snd_phys_create`: sys / cfg / desc / out が NULL なら `SND_ERR_INVALID_ARGUMENT`、失敗時の `*out` は NULL。
- P02 `struct_size` の小さい版（古いヘッダ）は、覆わない欄を既定にして受ける。`struct_size` < 8・4 の倍数でない・`version` 0 / 未来の版は拒否。大きい版（未来のヘッダ）の扱いは未決。
- P03 設定値の範囲: grid ≤ 0、NaN、負の予算・クールダウン・タイムアウト、max_dt ≤ 0、容量 0 / 上限超え は `INVALID_ARGUMENT`。予約 ID（`SND_ID_RESERVED_*`）の Parameter ID は拒否。`SND_ID_NONE` は「渡さない」。
- P04 確保は生成時だけ（数えるアロケータで呼び出し回数が増えない、かつ RT スコープ内の new / malloc 検出がゼロ）: submit、footstep、play_at、set_table、load_bank_table、set_material_map / entry / surface_param、set_curves、get / reset_stats、表の差し替え。破棄で確保したものを全部返す。確保の失敗は `SND_ERR_OUT_OF_BUDGET` でリークなし。
- P05 作ったスレッド以外からはすべて `SND_ERR_WRONG_THREAD`（`snd_physics_material_count` は 0）。何も変えない（統計も）。
- P06 `snd_phys_create`: `bank` 0 かつ `table` NONE なら空の表。desc の各欄（容量、閾値、格子、クールダウン、予算、タイムアウト）が設定に写る。`impulse_threshold == 0` は表の既定。
- P07 `snd_phys_create` / `snd_physics_load_bank_table`: 無効・古いハンドル、未ロード、PHYS なし、表 ID なし、アンロード後 は失敗で何も変えない（決定 10 は未決: 片方だけ指定は `INVALID_ARGUMENT`）。
- P08 PHYS の中身は信用しない: 範囲外のオフセット、個数の溢れ、整列違反、NaN・無限大のカーブ、減少する閾値 は `SND_ERR_CORRUPT`（ASan/UBSan で落ちない）。失敗時は表が変わらない。
- P09 PHYS の写し方（未決 4）: 衝撃量→音量 (t0,.25)(t1,.5)(t2,.75)(t3,1)、ベロシティ (t0,0)(t1,1/3)(t2,2/3)(t3,1)、質量→ピッチ・速度→音量の対数 8 点、表の閾値は t0 の最小。単体 `[N][4]`、組（全種別共通）、足音（モーション共通）、既定 `[4]`。
- P10 C++ ラッパー（`snd::phys`）は C と同じ結果を返す。`physics.h` は C11 としてコンパイルできる。

## B. 時刻（決定 D）

- P11 dt が負・NaN・±∞ は `SND_ERR_INVALID_ARGUMENT`、何もしない（時刻も予算の端数もタイムアウトも進まない、`calls_rejected` だけ増える）。
- P12 dt は max_dt で頭打ちし、µs の整数に丸めて積算する。クールダウン・タイムアウトも µs で比べる（「ちょうど」は通す / 止める）。
- P13 `count == 0`（contacts NULL 可）でも時刻は進み、タイムアウトを処理する。`count > 0` で contacts NULL は `INVALID_ARGUMENT`。

## C. 間引き（9.4、決定 D）

- P14 閾値: Impact は `impulse < threshold` を捨てる（ちょうどは通す）。NaN・負・±∞ の衝撃量は `dropped_invalid`。Break には閾値を掛けない。
- P15 Slide / Roll は `relative_speed < min_slide_speed` を捨てる（既定 0 なので捨てない）。NaN・負は invalid。
- P16 まとめ: キー =（各軸 `floor(p / g)`、順序なしの材質の組、種別）。ちょうど k×g は k、負の座標は負のセル。Impact と Break は混ざらない。代表は衝撃量最大（同値は配列で先）。`Count` = まとめた件数（≥ 1）、`merged` = 件数 − グループ数。巨大な座標・非有限の座標で未定義動作を起こさない。
- P17 クールダウン: キー =（順序なしの物体の組、順序なしの材質の組、種別）。最後に実際に鳴らしてから `cooldown` 以上で通す（ちょうど通す）。予算で落ちたものは記録しない。表は固定容量で、溢れたら最も古い記録を追い出す。
- P18 予算: Break が先、それぞれ衝撃量の降順（同値は配列で先）。枠 = `floor(carry + bps × dt)`、端数だけ持ち越す（使わなかった整数分は持ち越さない）。300/s・1/60 s は毎フレーム 5。dt = 0 は枠を増やさない。Slide / Roll・足音・play_at は予算に数えない。
- P19 作業領域は固定容量: 溢れた接触は `dropped_work_overflow`。既定の容量で 1 フレーム 1000 件は溢れない。
- P20 鳴らす Event が無いグループは何も鳴らさず、予算もクールダウンも消費しない（統計に数える）。
- P21 以上を素朴に書いた参照モデルと、乱数の接触列（固定の種）で、統計の各数と観測記録の列が一致する。

## D. 鳴らす音（9.1, 9.3、決定 C）

- P22 代替: 組 → 両方の単体（片方だけならある方、同じ材質は 1 回）→ 全体の既定 → 何も鳴らさない。段ごとの統計と FALLBACK 記録。
- P23 組の表は `pair[min*N+max]` で対称（下三角は無視）。`pair_by_kind` は種別ごとに置き換える。
- P24 対応表: 未登録は恒等。登録後、範囲外のゲーム材質・UNMAPPED は材質不明 → 全体の既定 + `fallback_unknown_material`。間の番号は恒等で埋める（未決 11）。
- P25 表面パラメータ: 単体の音はその材質の分、組・既定の音は両方（同じ ID は大きい値）を Playback の Parameter で渡す。
- P26 カーブ: 区分線形・範囲外は端の値・同じ x は段差。衝撃量→音量 / ベロシティ、質量→ピッチ、速度→擦れの音量とピッチ。単体の音は自分の材質のカーブと自分の質量。未設定は 材質 → 表の既定 → 組み込み。音量 [0,1000]、ピッチ [1/16,16]、ちょうど 1 は記録しない。
- P27 表の差し替えは次の呼び出しから効く。追跡中の再生は止まらない。
- P28 Parameter の ID は設定で差し替えられ、`SND_ID_NONE` は渡さない。既定は名前の `snd_hash_id`。

## E. 足音（9.2、決定 D）

- P29 重みの大きい 2 つまで（同値は配列で先）、0 以下・NaN・∞ は無視。音量 = curve(w / (w1 + w2))。
- P30 Event: `footstep[M][motion]` → `footstep_any[M]` → `footstep_default` → 何もしない。材質不明は直接 default。モーションと足の材質は Parameter。`foot` の Emitter（0 なら 2D）。未知のモーションは拒否。

## F. 継続する接触（9.5、決定 D）

- P31 キー =（順序なしの物体の組、順序なしの材質の組、種別）。最初の報告で START、以後 UPDATE（位置と速度）。同じ呼び出しの同じキーは相対速度最大の 1 件。
- P32 最後の報告からの経過がタイムアウト以上で（ちょうどで）フェード付きで止め、Emitter を返す。報告が続く間は止めない。
- P33 表が溢れたら最後の報告が最も古いもの（同じなら始まりが古いもの）を追い出す。同じ呼び出しで報告されたものは追い出さない（未決 8）。
- P34 再生が自分で終わっていたら次の報告で始め直す（RESTART）。
- P35 破棄で追跡中の再生を止め、Emitter を返す（max_emitters 個の Emitter を後で作れる）。

## G. 再生の経路（決定 D、未決 2）

- P36 Impact / Break は Emitter に位置を与えて鳴る（本物の出力で左右の比が位置どおり）。
- P37 Emitter が取れないときは `snd_play_at` に落として `play_at_fallbacks` に数える（Parameter なし）。保持中の Emitter は時間が経てば返る。破棄ですべて返る。
- P38 core: 同じフレームで create → set_transform → play → destroy した Emitter の再生が位置を保つか（A-004 の独立な再現）。

## H. 統計・観測（決定 E）

- P39 統計は累積で、`struct_size` の小さい版は前だけ写す（後ろは触らない）、大きい版は既知の分だけ。`struct_size` < 8 は拒否。リセットで 0。
- P40 観測コールバックは同じスレッドで 1 件ずつ、要求・開始・更新・停止・代替のたびに呼ばれる。中からの physics の呼び出しは `SND_ERR_UNSUPPORTED`、中での破棄は何もしない。
- P41 統計の恒等式（ヘッダの主張）: received = invalid + threshold + overflow + merged + groups + slide_reports、groups = no_event + cooldown + budget + group_requests。

## 再確認（51c7144、受け入れ側の決定の反映後）で変わった性質

- P02 改: `struct_size` が `sizeof` より大きい版は受け、既知の部分だけを読む（追加の決定 1。いまは `IndepApi.Defect_LargerStructSizeIsAccepted` が落ちる）。
- P03 改: `emitter_hold_seconds` は無くなった。API のカーブの y は 音量 [0,1000]・ピッチ [1/16,16]・ベロシティ |y| ≤ 1e6、範囲外は `INVALID_ARGUMENT`（追加の決定 2）。
- P08 改: PHYS の |cents| ≤ 4800、|dB| ≤ 200。超えたら `SND_ERR_CORRUPT`（決定 5）。
- P17 改: クールダウン表の追い出しは、記録の時刻が同じなら先に挿入したものから（決定 2）。
- P20 追: 予算の枠を得ても Emitter が取れない組は鳴らさない（`dropped_no_emitter`、DROP 記録、枠は消費、クールダウンは記録しない、要求・段の統計に数えない）。
- P24 改: 片方だけ材質不明なら、分かっている側の単体（その材質のカーブ・その物体の質量・その表面パラメータ）、無ければ全体の既定。両方不明は全体の既定。組は引かない（決定 1）。
- P29 改: 有限で正の重みはすべて有効（和は溢れない）。
- P32/P33 改: 1 回の呼び出しは (1) 追跡中のキーの報告 → (2) タイムアウト → (3) 新しいキー（空きが無いときだけ追い出し）。
- P19 改: 追跡中のキーへの報告は作業領域を使わない。新しいキーは追跡表の容量ぶんの別の領域、溢れは `dropped_work_overflow`。
- P37 改: 単発の Emitter は、音声側が再生を適用したと観測できてから（Submit の中で）返す。保持の環が満杯で、すべてがこの呼び出しのものなら新しい組を捨て、前の呼び出しのものがあれば、いちばん古いものを早めに返す。`snd_play_at` への代替は無い。
- P38 改: core の A-004 は「記録」として、正しい結果（a）か既知の欠陥（b）のどちらかを確かめる。
- P42 新: 別のスレッドからの破棄は何もしない（停止も解放もしない）。作ったスレッドで破棄し直せる。

## 最終確認（e57abd7、受け入れ側の決定 1〜5）で変わった性質

- P02 改: `version` は互換でない変更のときだけ上がる番号で、今の値と違えば（0 も新しい値も）`INVALID_ARGUMENT`。大きい `struct_size` は受けて既知の部分だけ読む（設定も表の記述も）。
- P37 改: 保持の環が満杯なら、前の呼び出しのものでも早めに破棄せず、新しい組を捨てる（DROP、`LIMIT_REACHED`、`dropped_no_emitter`、予算は消費、クールダウンは記録しない）。保持中の音は位置を保つ。音声側が進めば環は必ず空く。`held_emitters_released_early` は無くなった。
- P38 改: 入れ子の Event のアクションが Emitter の解除の後に鳴らす子の再生は core の制限（A-004 5 節）。記録のテストで (a) 右（core が直った）か (b) リスナーの位置（既知の制限）かを確かめる。
- P43 新: 単発の再生の音量とピッチは physics のもの（解除までは physics が元の値を記録し直しうる）。テストは置かない（決定 4、任意）。

## report-A.md 6 節の「確かめていない」箇所と、通っていない行（indep_final_test.cpp）

- P43 確認: 保持中にゲームが観測のハンドルで変えた音量は、解除の確認で physics の値に戻りうる（今は戻る）。Emitter を返した後に変えた音量はそのまま効く。保持中でも `snd_stop` と状態の問い合わせは働き、止めた再生の Emitter は返る。
- P44 新: 解除の確認（同じ値の音量の記録）は音を変えない。確認が何度も走る場合と走らない場合の出力が、頭を揃えてサンプル単位で一致する（音量 0.5、ピッチ 1.5、2 音の重ね）。
- P45 新: 出力が長く進まない間（数千フレーム）、保持の環は満杯のまま新しい組を捨て続け、位置なしでは鳴らさない。出力が進めば保持は全部解け、新しい衝突は正しい位置で鳴る。
- P46 新（欠陥）: `snd_suspend` 中もゲームが毎フレーム Submit を呼ぶと、解除の確認の記録でコマンドの予備が溢れ、`snd_resume` が受け付けられなくなる（core の欠陥と重なる）。
- P47 新（記録）: 解除の後に始まる子の再生: 音楽の 2 つ目のセグメント（core は音楽の Event を 3D にしない。保持した Emitter と同じ結果）、ゲームが鳴らすスティンガー（(b) リスナーの位置）。
- P48 新: 資源の枯渇（再生のスロット、Emitter、コマンド領域）で API の失敗は `api_failures` と記録の result に出て、落ちず、資源が戻れば回復する。
