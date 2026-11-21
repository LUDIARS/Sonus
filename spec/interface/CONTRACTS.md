# 契約（段階0「契約の凍結」）

設計書 `spec/architecture/design.md`（版 0.2）の 12.2 で凍結すると決めた 7 項目を、このリポジトリのどのファイルが表すか、
どう変えるか、段階0で何を決めたかをまとめる。段階1以降の並列実装はこの文書と下記のファイルだけを入力にする。

- 契約の版: **1.2**（`SND_CONTRACT_VERSION_MAJOR = 1`, `SND_CONTRACT_VERSION_MINOR = 2`、`contract/public/include/snd/version.h`）。1.1 の変更は 3 節の D-43〜D-55、1.2 の変更は D-88〜D-99、D-102〜D-104 と 7 節
- 1.2 への版の更新は受け入れ側の決定（ユーザー承認）。凍結ファイルの変更は `version.h` の MINOR と、`contract/test/contract11_test.cpp` の版の検査を「major が 1、minor が 1 以上」に緩めたこと。1.2 は新しいコマンド種別を足さず、既存の予約欄を `variant` にして表した（1.1 の記録は 0 を書いていたので互換）
- バンク形式: **2.0**（`snd::bank_format::kFormatMajor/Minor`）
- ライブリンク: **1.0**（`snd::livelink::kProtocolMajor/Minor`）

## 1. 凍結対象のファイル

| 12.2 | 項目 | ファイル |
| --- | --- | --- |
| 1 | 公開 API（C） | `contract/public/include/snd/version.h`, `id.h`, `types.h`, `ext.h`, `config.h`, `sonus.h`, `physics.h` |
| 1 | 公開 API（C++） | `contract/public/include/snd/id.hpp`, `sonus.hpp`, `physics.hpp` |
| 2 | 内部 IF（12.3） | `contract/public/include/snd/ext/types.hpp`, `allocator.hpp`, `job_system.hpp`, `file_system.hpp`, `codec.hpp`, `dsp_effect.hpp`, `output_device.hpp`（ユーザーも実装できるので公開層に置く） |
| 2 | core ⇄ source / music | `contract/internal/include/snd/internal/source_if.hpp`, `music_if.hpp` |
| 3 | テーブルと列の所有者 | `contract/internal/include/snd/internal/schema.hpp`, `common.hpp` |
| 4 | コマンドとイベント | `contract/internal/include/snd/internal/commands.hpp`, `notify.hpp` |
| 5 | バンク | `contract/internal/include/snd/format/bank_format.hpp`, `bank_check.hpp`（1.1: §8 の検証規則、ヘッダのみ。bank と buildtool が共有）, `spec/interface/bank-format.md` |
| 5 | ライブリンク | `contract/internal/include/snd/format/livelink_protocol.hpp`, `spec/interface/livelink-protocol.md` |
| 6 | モック | `tests/mocks/`（`snd::mock::NullOutputDevice`, `MemoryFileSystem`, `DeterministicJobSystem`） |
| 7 | テスト枠組み | `tests/framework/`（ランナー、ゴールデン音声、RT 安全性）、`contract/internal/include/snd/internal/rt_check.hpp` |
| — | ビルドの枠 | ルート `CMakeLists.txt` のモジュール依存表（`SND_MODULE_DEPS_*`）、`cmake/SonusModule.cmake`、`cmake/SonusOptions.cmake` |
| — | この文書 | `spec/interface/CONTRACTS.md` |

`contract/` 以下は全員の入力であり、どのモジュールの担当者も直接編集しない。モック・テスト枠組みは「振る舞いを変えない
バグ修正と、既存テストを壊さない機能追加」に限り、tests 担当の合意で版を上げずに直せる（4 節）。

### 1.1 ターゲットと見える範囲

| CMake ターゲット | 中身 | 使う側 |
| --- | --- | --- |
| `sonus_contract_public` | `snd/*.h`, `snd/*.hpp`, `snd/ext/*` | physics（公開 API だけを使う上位層） |
| `sonus_contract` | 上記 + `snd/internal/*`, `snd/format/*` | 他の全モジュール、テスト |
| `sonus_<module>` | 各モジュール | 依存表に従う |
| `sonus` | 構成ルート（`api/compose/`）。ランタイムの全モジュールを束ねる | ゲーム、統合テスト |
| `Sonus::sonus`（`sonus_c_api`） | C API の窓口: 公開ヘッダの場所と `sonus` のリンク（`cxx_std_20` を伝えない、D-100） | 取り込む親（Ergo） |
| `Sonus::tool_core`（= `sonus_tool`） | ツールの核（UI に依存しない、Tela なし、`cxx_std_20` を伝える、D-101） | 外のホスト（Ergo Studio）、Tela のツール |
| `sonus_testkit`, `sonus_testkit_main`, `sonus_testkit_rthooks`, `sonus_mocks` | テスト枠組みとモック | 各モジュールの `test/` |

## 2. 変更手順（12.4）

1. **提案**: 変更したいファイル・理由・影響するモジュールを書いた変更提案を出す（PR の説明か `spec/interface/proposals/` に md）。
   後方互換か（minor）、互換を壊すか（major）を明記する。
2. **合意**: 影響するモジュールの担当者全員が合意する。依存表（ルート `CMakeLists.txt`）の変更は全員の合意。
3. **反映**: 同じ変更で版を上げる。
   - 契約: `SND_CONTRACT_VERSION_MINOR`（追加のみ）/ `MAJOR`（それ以外、MINOR は 0 に戻す）。
   - バンク形式・ライブリンクはそれぞれの版（各 md の互換性の節）。
   - この文書の「変更履歴」に 1 行足す。
4. **確認**: `contract_test`（契約の自己検査）と全テストが gcc / clang / サニタイザで通ること。

minor（追加のみ）として扱ってよいもの: 新しい関数、列挙値の末尾追加、`struct_size` で守られる構造体末尾へのフィールド追加、
新しいコマンド種別（末尾）、新しいスキーマ列（既存の所有者を変えない）、新しいバンクのチャンク・列、新しいライブリンクのメッセージ。
`Config` の既定値（「仮」の値）の変更は契約の変更ではない（コメントと 3 節の表だけ直す）。

## 3. 段階0での決定

設計書で決まっていなかった点、または設計書のままでは実装が食い違う点について下した決定。理由を添える。
番号はコード中のコメント（`D-xx`）から参照される。

### 構成と依存

| # | 決定 | 理由 |
| --- | --- | --- |
| D-01 | 凍結ヘッダはすべて `contract/` に置き、公開層（`public/`）と内部層（`internal/`）に分ける。モジュールのディレクトリには契約を置かない | 担当者が自分のディレクトリだけを触れば済み、凍結範囲がディレクトリで判る。physics に内部層を見せない |
| D-02 | モジュール依存は 12.1 の表をルート `CMakeLists.txt` に 1 か所だけ書き、`snd_add_module()` が読む。`base` と `platform` は PUBLIC（推移的に見える）、他は PRIVATE。よってモジュールの公開ヘッダに出せる型は契約・base・platform のものだけ。buildtool の依存は codec と契約のバンク形式定義（bank のランタイムには依存しない）。1.1 で buildtool の依存に dsp を加えた（`dsp::ComputeDspPoolNeed` と標準エフェクトの表を使うため。A1-5）。dsp は base / platform に依存するので、buildtool からは base / platform も見える | 依存方向をビルドで強制する。12.1 の「bankの形式定義」は契約側にあるため |
| D-03 | 構成ルート: 全モジュールを束ねるライブラリ `sonus` を `api/compose/` に置く。`sonus_api` 自身は 12.1 どおり core と music だけに依存し、source のソース種別・music の `IMusicSystem`・io の既定ファイルシステム・platform の既定デバイス・livelink は構成ルートが契約の IF 越しに渡す | 12.1 では api が source / io / platform / livelink に依存しないのに、`Create` はそれらを必要とする（G-03） |
| D-04 | ID ハッシュは 32bit FNV-1a（オフセット 2166136261、素数 16777619）を UTF-8 のバイト列にそのまま掛ける。大文字小文字・区切り文字は正規化しない。結果が 0 なら 1。0 は「ID なし」 | 単純で constexpr にでき、buildtool と一致させやすい。衝突はプロジェクト全体でビルドエラーにする（10.1） |

### 公開 API

| # | 決定 | 理由 |
| --- | --- | --- |
| D-05 | ハンドルは 64bit = 世代（上位 32）＋スロット（下位 32）。世代 0 は発行しないので値 0 が無効ハンドル。Bank と UserBuffer も同じ形。払い出しと返却の方式は D-46（Bank は bank モジュールがミューテックスの下で行う）。音楽 Playback が所有するトランスポートのハンドルは `slot = 0x80000000 \| playbackSlot, generation = playback の世代` で往復なしに導ける（`snd_get_playback_transport`） | 3.1。13.2 の例の `musicTransport` をゲームが得る手段が無かった（G-12） |
| D-06 | C API が ABI の正本。C++ API はヘッダだけの薄いラッパーで、POD は C 構造体の別名か同じレイアウトの型、列挙は C の値を持つ `enum class`。拡張点（12.3 の 6 IF）は C の関数表（`snd/ext.h`）と C++ の抽象クラス（`snd/ext/*.hpp`）の両方を持ち、相互の橋渡し（`snd::ToC`, `snd::C*` アダプタ）を契約に含める。`snd_create` は関数表を値でコピーする | 13.1「C APIはPODとハンドルだけ、C++はその薄いラッパー」。C++ 利用時の二重間接は許容（api が後で特別扱いしてよい） |
| D-07 | `Config` のスカラー項目は X マクロ（`snd/config.h`）1 か所で定義し、C 構造体（snake_case）、`snd_config_init`、C++ の `snd::Config`（camelCase、既定値付き）、C++→C 変換を生成する。先頭に `struct_size` と `contract_version`。音量は線形ゲイン | C と C++ で項目がずれない。本文中の上限と「仮」の値をすべて項目にした（下表） |
| D-08 | オーディオスレッドと RT ワーカーは `IJobSystem::Submit` も `IFileSystem` も呼ばない。有界のロックフリーキュー（`SourceServices::postJob` / `postIo`）に積み、非 RT スレッドが受けて投入する | 12.3 の「Submit はどのスレッドから呼ばれてもよい」をホストの実装に任せると原則 1 を破りうる（G-15） |
| D-13 | `Update()` が公開するのは所有スレッドのアリーナだけ。他の登録スレッドは `snd_flush()`（追加）でフレームごとに公開し、`snd_unregister_thread()`（追加）で抜ける | 他スレッドのアリーナを所有スレッドが公開すると書き込み中と競合する |
| D-28 | 13.1 に無い関数を足した: `snd_flush`, `snd_unregister_thread`, `snd_contract_version`, `snd_config_init`, `snd_get_bank_chunk`, `snd_get_playback_transport`, `snd_destroy_transport`, `snd_set_transport_state`, `snd_get_transport_position`, `snd_destroy_user_buffer`, `snd_get_user_buffer_released`, `snd_play_user_buffer`, `SetSwitch` の 3 スコープ、physics の `create/destroy/desc_init`。1.1 で `snd_declare_language` を追加（D-50）。`Resume` は C で `snd_resume`（システム）と `snd_resume_playback` に分ける。`PlayAt` と `PlayOneShot` はハンドルを返さず `snd_result` を返す | G-05, G-09, G-10 |
| D-29 | 無効ハンドル: 記録時に古いと判れば（値 0、またはゲーム側スロット表と世代不一致）何も記録せず `SND_ERR_INVALID_HANDLE`。記録後に古くなったものは適用時に世代不一致で無視。どちらも「何もしない」（13） | 呼び出し側が分岐しなくてよく、デバッグ時には理由が分かる |
| D-30 | カテゴリ ID `SND_ID_NONE` は「全カテゴリ」（全体のポーズ・音量、7 章）。全体のポーズではポーズ無視カテゴリを除外する。全体の音量（とミュート）はポーズ無視カテゴリにも掛かる（段階2で明記） | 全体ポーズ用の関数を増やさない |
| D-31 | プラグインの登録は `Config.plugins`（コーデックは fourcc、エフェクトは種別 ID）。同じ fourcc のコーデックは組み込みより優先。エフェクトも同じ種別 ID ならプラグインが標準エフェクトより優先（1.1 で明記） | 13.1 に `ICodec` / `IDspEffect` を登録する手段が無かった |
| D-21 | `When` は 32 バイトの POD。8.2 の全種に加え `CLOCK_SAMPLE`（`PlayScheduled` 相当）と `DELAY_US`（`PlayDelayed` 相当）を持つ。アウフタクトは `offset_ticks`（手動）と `PICKUP_ALIGN` フラグ（素材のピックアップマーカー）で表す。音楽系の種類で無効なトランスポートを渡すと即時扱い＋警告。`SetMusicState` の `When` は即時なら遷移ルールのタイミング、それ以外はルールの「タイミング」列を上書き | 7 章「時間制御」の行を表現するため（G-13） |
| D-20 | 音楽位置は内部では原点からの通し tick（i64、1 拍 960）。公開の `snd_musical_time` は小節・拍・tick で、すべて 0 起点 | 拍子変化をまたぐ計算を単純にする |
| D-40 | UserBuffer のサンプル領域はゲームの所有（`IAllocator` 由来ではない）。読み書き位置（アトミック）を持つ制御ブロックは `Create` 時に確保（`maxUserBuffers`）。書き手はバッファごとに 1 スレッド。解放はゲームが `snd_get_user_buffer_released` を見てから | 5.1 |
| D-42 | `PlayOneShot` / `PlayAt` も Playback スロットを 1 つ使う（`maxPlaybacks` に数える）。終了フラグは立つが `on_end` は呼ばない | 全再生を同じ経路で扱う |
| D-39 | C++ の `Create` は 1 回に各種 32 個までのプラグインを変換する | スタック上の一時配列で済ませるため |

### コマンドと通知

| # | 決定 | 理由 |
| --- | --- | --- |
| D-09 | コマンドはまず正規領域に積む。正規領域が満杯のときだけ、必達（`MustDeliver`）を予約領域に積む。適用順は「正規 → 予約 → フォールバック」で、これは記録順に一致する。必達の範囲: 停止・ポーズ・予定停止、Parameter・スイッチ、Emitter / Transport / UserBuffer の生成と破棄、バンクの登録・解放・言語、ミックス操作、音楽のステート・テンポ・シーク・トランスポート状態、ライブリンクの編集。落としてよい（`Droppable`）: `SetTransform`, `SetListener`, `PrepareMusicState`, 試聴 | 2.1。生成を落とすと後続のコマンドが宛先を失うので生成も必達にした。位置更新は次フレームで上書きされる |
| D-10 | 「優先度の低い再生要求から拒否」は 2 段で実現: 正規領域が満杯なら再生要求は拒否、ただし Event の優先度が `criticalPriority`（既定 224）以上なら予約領域を使える。優先度はロード済みバンクの `EVNT.priority` をゲーム側が読んで `priorityHint` に入れる（読めなければ 0）。拒否された再生も有効なハンドルを返し、次の `Update()` で `REJECTED / COMMAND_OVERFLOW` で終わる | 線形アリーナでは積んだ要求を追い出せない。ゲーム側は優先度を知らない（G-07） |
| D-11 | 予約領域も満杯のときは、アリーナ付属の `OverflowFallback` に畳み込む: 停止と各種破棄・アンロードはスロットごとのビット、Parameter とスイッチは (スコープ, 宛先, ID) ごとの最終値表（`overflowParamSlots`）。表から溢れた値だけが失われ、`stats.reserve_overflows` に数える。フォールバックで表せない必達コマンド（生成、ポーズ、ミックス操作、音楽など）は、予約領域も満杯のとき記録せず、その場で `SND_ERR_COMMAND_BUFFER_FULL` を返す（ハンドルを返す関数は無効ハンドル）。呼び出し側に分かるので「失われた」ことにはならない。`stats.reserve_overflows` は失われた値（最終値表から溢れた Parameter・スイッチの値）だけを数え、畳み込んで届けたものは数えない（段階2で明記） | 「必ず届ける」を量に関係なく守る |
| D-12 | 適用ブロック番号はアリーナ単位。オーディオスレッドが取り込んだブロックの番号を `ArenaHeader::appliedBlock` に書く。リプレイは (producer, appliedBlock, アリーナの中身) を記録する。生産者の取り込み順は登録順 | 2.5, 11.3 |
| D-16 | 通知: 終了は Playback スロットごとの記録＋ビット配列（取りこぼしなし）。Emitter / Transport / UserBuffer のスロット返却も同じ方式のビット配列。バンクの状態はスロットごとのアトミック。拍・マーカー・ログはイベントリング（48 バイトの記録、溢れたら捨てて数える）。問い合わせ用のトリプルバッファは各バッファにシーケンスロックを持ち、任意のスレッドから読める | 2.1。「任意のスレッド」から読むと `Update()` のスワップと競合する（G-08） |
| D-15 | 「短いフェード」（奪われたボイス、ストール、締切超過、アンロード）は一律 `tuning.shortFadeMs`（既定 5 ms）。サンプル単位で、ブロックをまたいでよい。短いフェードの途中に別の短いフェード（停止、アンロード、再度のスチール）が重なったときは、2 つのゲインを掛け合わせる（不連続は出ず、傾きは最大 2 倍、必ず `shortFadeMs` 以内に 0 に着く） | 設計書に値が無い |

### テーブルと所有者

| # | 決定 | 理由 |
| --- | --- | --- |
| D-17 | 3.1 に無いテーブル・列を足した: Category、Snapshot のテーブル。ParamValue はスコープ別の 3 表（Global / Emitter / Playback）にし、スイッチは `kind` 列で同じ表に置く。Playback に `request`, `logic`, `endInfo`, `dynamicSource`、Voice に `emitterSlot`, `limitGroup`, `lifecycle`, `filterTarget`, `renderStatus`, `clipRef`、RealVoice に `assign`, `appliedGain`、Emitter に `renderPosition`、Bus に `volume`, `gainTarget`, `effectBypass`, `graphOrder`、Transport に `ownerPlayback`, `lifetime`, `signature`, `loopRange`, `initial`、Reservation に `when`, `acceptedBlock`, `resolved`, `sequence`、Stream に `control`, `ringWrite`, `ringRead`, `stallCount` | 3.2 の読み書きを 1 列 1 書き手で表すのに必要（G-02） |
| D-18 | 所有者の決定: `Voice.audibility` と `Bus.gainTarget` は EvaluateDspParams。`Bus.gain`・`Bus.peak`（ダッキング込みの実際の値）と `RealVoice.panMatrix`（Spatial から計算）は Render。Transport の状態列はすべて UpdateTransport（即時の `SetTempo` / シーク / 状態変更も「今」で予約に入れる）。予約の要求時刻（ApplyCommands）と解決後の時刻 `Reservation.resolved`（UpdateTransport）を分ける。Emitter の遮蔽値は EvaluateLogicParams（Parameter のランプを通す）。行の生成・破棄: Playback は ApplyCommands が作り RunEventLogic が消す、Voice と Stream は RunEventLogic（ソースの `Start` は RunEventLogic の中で動く）、RealVoice は SelectVoices、Transport と Reservation は ApplyCommands が作り UpdateTransport が消す、ParamPlayback は Playback と一緒に RunEventLogic が消す。仮想ボイスの位置送りは Render が行う | 3.2 の「1 列 1 システム」を守る |
| D-19 | `eventId` はコールド列、ホット列には解決済みの `eventRef`（バンクスロット＋行）を置く | 3.1 の表（ホット）と本文（コールド）が矛盾（G-01） |
| D-35 | 所有者の食い違いはコンパイル時に検出する: 書き込みは `WriteColumn<System, Column>`、読み出しは `ReadColumn<System, Column>` を通し、所有者でない書き込み・宣言のない読み出しは `static_assert` で失敗する。読み出しの時点（同じブロック / 前ブロック / 非同期）も `ValidateReads()` が検査する。`contract_schema_violation_rejected` などのテストがコンパイル失敗を確認する | 12.2-3「所有者の食い違いを検出できる仕組み」 |
| — | オーディオスレッド以外の書き手（`ExternalIo`, `ExternalDecode`）を持てるのは `std::atomic` の列だけ（`Stream.readPos`, `Stream.ringWrite`）。他のシステムからは `Async` 読み出しとして宣言する。1.1: Stream は Direct 表で、これらの列はスロット番号で直接添字する（D-45） | 3.2 に Stream の書き手が無い |

### core ⇄ source / music と拡張 IF

| # | 決定 | 理由 |
| --- | --- | --- |
| D-22 | 記録データを読むソース（Sample, Stream, UserBuffer, Input, Granular の読み出し）はソースのレートでフレームを出し、リサンプルは core が dsp で行う。合成するソース（Oscillator, Noise, Patch, Callback）は内部レートで描き、ピッチを自分で反映する（`ISourceKind::RendersAtOutputRate`） | 12.1 で source は dsp に依存できない（G-04） |
| — | ソースの呼び出しは種別ごとの配列単位（`Start`, `Release`, `Control`, `QueryReady`, `RenderBatch`, `AdvanceVirtual`, `ScheduleBackground`）。仮想呼び出しは種別×バッチで 1 回 | 5.1 |
| — | 音楽: UpdateTransport は `IMusicSystem::UpdateTransport` が実装し、テンポ・シーク・状態の予約を自分で適用、残りを発火リストにする。音楽のロジックは RunEventLogic の中で `RunMusicLogic` を呼び、返ってきたボイス要求を core が通常のボイス経路で実行して結果を同じブロックで返す。拍・マーカーは Publish で `CollectNotifications` | 8 章、3.2 |
| D-23 | `ICodec::Decode` は入力を全部消費し、途中のパケットと書ききれないフレームを状態に保持する（`src = NULL` で吐き出す）。`Seek(frame)` は「次に渡すパケットの先頭フレーム」を告げる。シーク表で `target - preRoll` 以前のパケットを選び、先頭の余分は呼び出し側が捨てる。プリスキップはコーデックが初回に適用 | 12.3 の骨子では消費バイト数を返せない（G-06） |
| D-24 | `IFileSystem` に `Size()` を追加。`IoStatus` は `{state, bytes}`。トークンは呼び出し側（IO スケジューラ）が決める。完了・エラーは 1 回だけ報告され、その後トークンは無効 | 同上 |
| D-25 | `IOutputDevice` に `Start`, `Stop`, `Close`, `Channels` を追加。コールバックはインターリーブ float32。コールバックの無い出力 API はデバイス実装がタイマーで駆動する。イベントは変更・サスペンド・レジューム・喪失・フォーカス | 2.2, 2.6 |
| D-26 | `IDspEffect::Process` は in-place を許す。`ParamBlock` はブロック先頭値・末尾値（線形補間）・変化マスク・`RenderContext` | 5.1 のジッパー対策 |
| D-27 | 拡張 IF は公開の仮想デストラクタを持つ（RTTI は不要） | 警告なしで派生できる |
| D-14 | デコーダプール = `maxDecoders` × `decoderSlotBytes`。`StateBytes` がスロットより大きいコーデックの再生は `DECODER_POOL_FULL` で拒否 | 3.3 の「状態サイズ × 同時数」を固定サイズにする |
| — | Stream の IO エラー（開けない、読み込みの失敗、ヘッダの不一致、短い読み込み）: 新しい読みは出さず、すでにメモリにあるデータ（プリロード、失敗した読みより前に届いたチャンク、デコード済みのリング、それで賄えるループの周回）をすべて順に鳴らし切ってから、フェードして `STREAM_IO` で終える。読みを要しない無限ループ（全データがメモリにある）はそのまま鳴り続ける | 「届いた分を鳴らして終わる」と同じ扱い。ループの位置で鳴る長さが変わらないように |
| — | IO エラー後に「メモリにある」とみなすのは常駐のプリロードだけ。デコード済みのリングは流れていく一時バッファで、ループやシークの元に使わない。エラー後に付けたプリロード外のループはリングにある分を鳴らして終わる。エラー後の Seek: プリロード内へはプリロードの終わりまで鳴らして終わり、それ以外（読み済みの範囲を含む）への Seek と Devirtualize はすぐ終わって `STREAM_IO`。`streamIoErrors` は失敗した読みが届いた時点（IO 側の完了処理、`StreamChannel::Errors()`）で再生ごとに 1 回数え、デコーダが後で消費しても二重に数えない（読みが成功してヘッダが不一致のときはその検出時に 1 回） | 失敗が鳴り方に影響しない場合（プリロード内のループ）も数えられるように | 「届いた分を鳴らして終わる」と同じ扱い。ループの位置で鳴る長さが変わらないように |

### 形式・テスト

| # | 決定 | 理由 |
| --- | --- | --- |
| D-32 | バンク: 128 バイトのヘッダ、CRC-32C（ヘッダ自身にも別のチェックサム）、チャンク目次、テーブルは列ディレクトリ付きの SoA、列 ID はテーブルごとに 0 からの連番、未知のチャンク・列は無視、任意列は既定値、チャンクごとの版。ストリームのパスは除去不可の `SPTH`、物理マテリアル表はアプリケーションチャンク `PHYS`、依存は (bankId, contentHash) | 10.2。列単位で足せるので並列実装で minor 版を上げやすい（G-11） |
| D-33 | 版は 3 系統（契約、バンク形式、ライブリンク）を独立に持ち、いずれも major（互換なし）/ minor（追加のみ） | 2 節 |
| D-34 | ライブリンク: 16 バイトのフレームヘッダ、Hello / HelloReply / SyncDecision の照合、値の変更はバンクのテーブル列（チャンク fourcc ＋ 列 ID）で指定、リストは要素サイズ付き | 11.2。項目の番号を別に定義しなくて済む |
| D-36 | RT 安全性の印（`SND_RT_SCOPE`）と記録（`SND_RT_NOTE`）は契約のヘッダだけで完結し、`SND_RT_CHECKS=0` で消える。既定はテストをビルドするとき ON。スレッドローカルの深さは `volatile`（最適化で消されないため）。テスト実行ファイルでは `operator new/delete` を常に、malloc 系・pthread のロック・`fopen/open/read/write` をサニタイザ無しの glibc で差し替えて検出する。ライブラリ側はロックのラッパー、`IFileSystem` 実装、書式付きログ、`IAllocator` 実装に `SND_RT_NOTE` を置くこと | 14 章。サニタイザのランタイムも同じ関数を差し替えるので併用しない |
| D-37 | ゴールデン音声は `.sndg`（32 バイトのヘッダ＋チャンネル順の float32）。置き場所はテスト対象モジュールの `test/golden/`。更新は `--update-golden` か `SND_UPDATE_GOLDEN=1`。許容差は最大絶対誤差・RMS・超過サンプル数 | 14 章 |
| D-38 | モックは `snd::mock`。決定論ジョブは優先度を無視した投入順（FIFO）で、ジョブ内からの投入も投入順に実行。メモリ FS の遅延は「Poll の回数」で数える | 再現性 |

### 契約 1.1 での決定（段階1の持ち越し A1-1〜A1-10 の解消）

| # | 決定 | 理由 |
| --- | --- | --- |
| D-43 | 締切に遅れたチャンク（2.3）とメモリの再利用。(1) チャンクが触ってよいメモリは、そのチャンクのレンダー項目の配列、そのチャンクのバス別アキュムレータ、実行スレッドのスレッド別アリーナ、そのチャンクに属するボイスの描画状態だけ。描画状態は RealVoice のスロット番号で引く: RealVoice の列（RealVoice は Direct 表で行は動かない。playhead と renderStatus は RealVoice にある）と、RealVoice のスロット番号で引くプール（リサンプラ・フィルタ・ソースのボイス別状態）。`RenderItem` は RealVoice のスロット番号で参照する。Voice の行（Sparse、行が動く）はチャンクから書かない（`Voice.playhead` / `renderStatus` は Render の直列部分が RealVoice から写す）。チャンクがテーブルの列を直接読むことはなく、ブロックごとの入力は直列区間でレンダー項目に写す。(2) レンダーリストとチャンク別アキュムレータは 2 組を持ち、ブロックごとに交互に使う。並列描画（`RtWorkerPool::Run`）をしてよいのは、そのブロックの開始時に `RtWorkerPool::IsQuiescent()` が真のときだけ（このとき 2 組とも空いている）。偽なら、そのブロックはオーディオスレッドだけで、直前の並列ブロックが使わなかった組に直列描画する。よって遅れたチャンクが使用中の組は常に 1 組以下で、直列描画の描き先は必ずある。(3) 遅れたチャンクのボイスは「保留」（`Voice.renderHold`、Render の直列部分が書き、SelectVoices / BuildRenderList / RunEventLogic が前ブロック値として読む）。保留中はどのシステムもそのボイスの描画状態を読み書きせず、レンダーリストに入れず、破棄もしない。保留中のボイスの実スロット（RealVoice）は再割り当て（スチール）も解放もしない。出力は前ブロック末尾から短いフェード（D-15）で無音。`IsQuiescent()` が真になったら保留を解き、描画状態は「遅れたチャンクが書き終えた値」として次のブロックから通常どおり鳴らす。前の組の再利用の判定も `IsQuiescent()`。`StragglersInFlight()` は統計・診断用（直前の Run の遅れたチャンクを数えないので判定に使えない）。ソースのバッチ描画の約束は `source_if.hpp` の冒頭。保留中のボイスにはソースの `Control`（ポーズ、仮想化、再仮想化を含む）も送らない。保留中に来た要求は保留が解けたときに最終状態を1回だけ適用する。保留が解けたら、保留中に進むはずだったフレーム数（遅れたブロックの次から解除のブロックまで）だけ playhead を仮想ボイスと同じ扱い（`AdvanceVirtual`）で進めて時計に追いつかせ、次のブロックから短いフェード（D-15）で 0 から戻す（保留中は無音で playhead だけが進むので、フェードしないと解除の瞬間に不連続が出る。段階2で明記） | 遅れたワーカーは `Run` が返った後も次のブロックに入っても前のブロックの入出力に触る（A1-1、`rt_workers.hpp` の SAFE POINT）。世代で結果を捨てるだけでは、入力の書き換えと状態の二重更新を防げない。Sparse 表の行は詰めると動くので、チャンクの書き先は行の動かない表に限る |
| D-44 | リサンプラの状態は列にしない。`RealVoice.resamplerState`（64 バイト）を廃止し、core が持つ固定スロットのプールに置く。スロット数は `maxRealVoices + fadeReserveVoices`、スロットの大きさは `dsp::ResamplerStateBytes(Sinc, Config.maxVoiceChannels, Config.maxPitchRatio)`（64 バイト境界、`MemTag::Tables`）。プールの添字は RealVoice のスロット番号そのもの（列は不要。RealVoice は Direct 表、D-43）。`Config` に `maxVoiceChannels`（既定 8、1〜8）と `maxPitchRatio`（既定 4.0、1.0〜32.0 = `SND_MAX_PITCH_RATIO`。NaN・無限大・1 未満・32 超は `snd_create` が `SND_ERR_INVALID_ARGUMENT`）を足した（C 構造体では 1.0 の配置を保つため末尾）。ピッチ比（ソースのレート比込み）はこの上限で頭打ち | sinc リサンプラの状態は 8ch・比率 4 で 64 バイトを大きく超える（A1-2）。スロット番号で引けば行の移動と無関係 |
| D-45 | Stream テーブルは `Indexing::Direct`（スロット番号 = 行の位置。行は動かない）。オーディオスレッド以外の書き手（`ExternalIo` の `readPos`、`ExternalDecode` の `ringWrite`）を持つアトミック列は、スロット番号で直接添字する。Direct 表の行はハンドルで参照しない（`base/sparse_set.hpp` の注意）。io の `StreamChannel` のスロット単位のアトミック（`ReadPos()` など）との対応付けは段階2の Stream ソースが選ぶ: IO 側の公開値を source が Stream の列へ写すか、列の実体として io のチャンネルの値を参照するか | 密配列の行は詰めると動くので、IO スレッドとデコードジョブが位置を持って書けない（A1-3） |
| D-46 | ハンドルの払い出し: Playback / Emitter / Transport（単独）/ UserBuffer のスロットは `base::HandleAllocator`（ロックフリー）で、ゲーム側の登録スレッドのどれからでも払い出す。返却は所有スレッドの `Update()` が終了・解放フラグ（notify）を消費したときに行う。Bank のスロットはこの対象外: bank モジュール（`BankManager`）がミューテックスの下で払い出し・返却する（同じパスなら既存のハンドルを返すため。登録スレッドは非 RT なのでロックしてよい）。どのハンドルも発行する世代は 2^31 未満（最上位ビットは使わない。2^31−1 の次は 1） | A0-1。共有の空きリストを排他なしで使える。最上位ビットは allocator が「使用中」の印に使う。バンクの同一パス判定はロックフリーの空きリストでは表せない |
| D-47 | オーディオスレッドと RT ワーカーのスタックは 256 KiB 以上を保証する（`platform::kRtThreadMinStackBytes`、`ThreadDesc::minStackBytes`。OS の既定がそれより大きければ既定のまま）。プラグイン（`IDspEffect`、合成コールバック）が使ってよいスタックの目安は 64 KiB（`ext.h`） | sinc リサンプラは比率 4 超で約 32 KiB をスタックに置く。エフェクトとソースの一時領域も見込む（A1-10） |
| D-48 | `snd_effect_desc` の末尾に `sample_rate`（＋予約 `reserved`）を足した。ランタイム・ビルドツールが `state_bytes` / `init` の前に埋め、`init` の `sample_rate` 引数は常にこの値に等しい。`snd_effect_desc` に版の欄は無いので、プラグインは `snd_contract_version()` が 1.1 以上であることを確かめてから `sample_rate` を読む（1.0 のランタイムはこの欄を渡さない）。dsp の `EffectStateBytes(desc, sampleRate)` などはレートの引数を残し、`desc.sample_rate` は 0（未設定）か引数と同じ値でなければならない（違えば失敗）。プラグインに渡す desc には dsp が引数のレートを入れる | プラグインが状態サイズを見積もれなかった（A1-4）。標準エフェクトの既存の呼び出しを変えずに済む |
| D-49 | bank-format.md に標準エフェクトの種別 ID 表と組み込みコーデックのバイト配置（PCM f32 / s16、IMA ADPCM、ADPCM のブロックは 1 チャンネルあたり 1024 バイト以下）を追記した。バンク形式の版は 2.0 のまま（文書の追記のみ） | A1-6。buildtool と codec / dsp の実装の一致を文書で確かめられるようにする |
| D-50 | `snd_result` に `SND_ERR_BANK_ALREADY_LOADED`（-18）を末尾追加: 同じバンク（bankId＋言語）が別のパスからロード済み・ロード中のとき、後のロードはこの結果で失敗する。`snd_declare_language(sys, name)` を追加: 言語名をランタイムに伝える（登録スレッド、必達コマンド `DeclareLanguage`、名前はコマンドに複製。1〜31 バイト、`/` `.` `\` を含まない、最大 `SND_MAX_LANGUAGES` = 32 個）。`snd_set_language` の ID は `snd_hash_id(name)`。言語別バンクのファイル名は `<base>.<name>.sndb`。未宣言の ID を `snd_set_language` に渡すと記録せず `SND_ERR_NOT_FOUND`。宣言済みの名前の表はシステムに 1 つ置く: `snd_declare_language` は呼び出したスレッドで同期的に表へ登録してから（ロック可、登録スレッドは非 RT）必達コマンドを出し、`snd_set_language` はその表で判定する。よって同じスレッドで宣言の後に呼べば必ず見える。既に宣言済みの別の名前と同じ ID になる名前（ハッシュの衝突）は `SND_ERR_INVALID_ARGUMENT`、同じ名前の再宣言は成功で何もしない（`BankManager::DeclareLanguage` も同じ）。重複の勝ち負けは要求順（`RequestLoad` の受付順）: 後の要求の検証が先に終わっても先の要求の結果を待ち、先が成功なら後が `SND_ERR_BANK_ALREADY_LOADED`、先が失敗・取り消しなら後が登録される。同じバンクの古いスロットが UNLOADING の間は、同じパスでも別のパスでも新しい読み込みを失敗させず、古い方の `Release` が済むまで登録を待つ（読み込みと検証は先に進める）。Release 待ちのコピーに依存するバンクも（推移的に）待ち、依存不足として失敗させない（本当に依存が無い場合と循環は従来どおり失敗）。`AbortUnload` で古いコピーが LOADED に戻れば待っていたコピーは `SND_ERR_BANK_ALREADY_LOADED`、依存するバンクは古いコピーに対して解決される（ハッシュ一致なら登録、不一致なら `SND_ERR_BANK_DEPENDENCY`）。依存先の写しが先の写しの結果を待っている場合も、依存元は待つ。`SND_ERR_BANK_DEPENDENCY` は依存先がどの写しによっても最終的に登録されないと確定したとき（本当に無い、循環）だけで、その判定と失敗はスロット順に依存しない。依存は bankId と buildHash で照合し（両方が一致する登録済みの写しをスロットによらず探す。複数あれば要求順で最初の写しに解決。一致する写しが登録済みでなく、その bankId の写しがもう届きえないときだけ DEPENDENCY。`ComputeWillCome` は届きうる写しを bankId で数え、ハッシュは登録時に照合する）、言語は照合に使わない（どの言語の写しに解決すべきかは段階2の `snd_set_language` の実装で決める持ち越し）。要求順の副作用として、無関係な後のバンクも、先の要求の読み込みと検証が終わるまで登録を待つ | A1-7。二重読み込みを引数の誤りと区別する。言語名が無いと言語別バンクのパスを作れない |
| D-51 | `snd_codec.decode` の約束を明記: `src == NULL` の吐き出しは終端を意味しない（終端は `total_frames`）、保持できる未消費データには上限がある（組み込みは 16 KiB ＋ 1 パケット、超えると `SND_DECODE_ERROR`）、`max_frames` が返ったら未満が返るまで吐き出してから次を渡す | A1-8。codec の実装で決めたことを契約の正にする |
| D-52 | `notify.hpp`: トリプルバッファのフレームへの書き込みは `base::notify::SnapshotStore` / `SetFrameHeader` を通す。`SnapshotFrame::reserved` は base が「front になったときの seq」に使う。`snd::CCodec` を既定構築できるようにした（`Assign` / `Table`）。codec の登録表はこれを使う | 読み手の relaxed アトミックな読み出しと対になる書き込みが必要。codec が独自のアダプタを持たずに済む |
| D-53 | バンクの検証規則（bank-format.md §8）を契約側のヘッダだけの部品 `snd/format/bank_check.hpp`（`bank_format::check::CheckBank`）に置き、bank の `ParseBank` と buildtool の自己検査の両方がこれを使う。CRC は関数ポインタで差し替えられる（ランタイムは base の高速版、ビルドツールは契約の参照実装）。生成スクリプトと同期テストは削除 | 2 つの実装が食い違う余地を無くす（A1-13）。buildtool は bank に依存できない（D-02） |
| D-54 | DSP インスタンスプールの予約量はランタイムが決める: `BankManager` は読み込み時（検証ジョブ、通常ワーカー）に、実際の Config（`sampleRate`、`blockSize`、バスのチャンネル数）でバンクの EFFX / EFPV / EFBL からエフェクト記述を作り、`dsp::ComputeDspPoolNeed` を計算し直して予約する（bank は dsp に依存できないので、計算は `BankManagerDesc::dsp.compute` で構成ルートが渡す）。バスのチャンネル数は自分 → 親の順に 0 でない値、根まで 0 なら「出力（マスター）のチャンネル数」（`BankManagerDesc::outputChannels`、既定 2）。バンクの `DSPN` は照合用の目安で、ランタイムの値と違っても失敗にしない（`PreparedBank::builtDspNeed` に残す）。バンク形式は変えない | ビルド時の値はランタイムの Config を知らない。プラグインの状態サイズもランタイムでしか分からない |
| D-55 | `snd::WhenKind::ClockSample` を `AtClockSample` に改名した（内部ヘッダの `snd::ClockSample` 型と同じ名前で、公開ヘッダと内部ヘッダを同じ翻訳単位で読むと `-Wshadow` で失敗した）。公開と内部を同時に読む翻訳単位がコンパイルできることを `contract_test` が確かめる | 段階2の api は両方を読む |

### Opus と依存の方針（契約の版は 1.1 のまま、バンク形式も 2.0 のまま）

| # | 決定 | 理由 |
| --- | --- | --- |
| D-56 | **サードパーティ依存は無し。ただし libopus だけを例外とする。** libopus 1.5.2（xiph/opus のタグ `v1.5.2`、BSD 3 条項、特許は IETF 宣言の無償許諾）のソースを `third_party/opus/` に同梱し（ビルドに不要な `dnn/` `doc/` `tests/` と上流のビルド系は除く。版・取得元・除いたものは `third_party/opus/SONUS_README.md`）、`third_party/CMakeLists.txt` が静的ライブラリ `sonus_opus` を作る。オプション `SND_CODEC_OPUS`（既定 ON）。OFF なら libopus をビルドせず、Opus のデコーダ・エンコーダは登録されない（`CodecRegistry::Find` → `OpusUnavailable`、`ValidateHeader` → `CodecUnavailable`、buildtool は `OPUS` のアセットをエラー）。リンクするのは codec だけ（PRIVATE。モジュール依存表は変えない）。ランタイムに入るのはデコーダの部分だけ（`codec_runtime_has_no_encoder` が libopus のエンコーダの記号も検査）。libopus には Sonus の警告フラグと `-Werror` を掛けない（`-w`）。`SND_SANITIZE` の指定は掛ける。ルートの `CMakeLists.txt`（凍結）の変更はオプション 1 行と `add_subdirectory(third_party)` だけ。配布物に必要なライセンス表示は README の「サードパーティのライセンス」 | Opus はゲーム音声の標準的な圧縮で、ADPCM（約 4:1）より 1 桁小さい。自前実装は現実的でない。BSD 3 条項は表示だけで静的リンクできる。ソースを同梱すればネットワークもパッケージマネージャも要らず、全構成（サニタイザ込み）で同じソースが同じ設定でビルドされる |
| D-57 | Opus のバンク内の形式（bank-format.md §6.5「Opus」）: 48 kHz だけ（buildtool は他のレートを 48 kHz に変換して符号化。フレーム数は `ceil(元 × 48000 / 元のレート)` で全コーデック共通。`sampleRate` の指定が 48000 以外なら警告。`loadForm` が `pcm` ならビルドエラー）。1 パケット = 20 ms = 960 フレーム（`framesPerPacket` 960、0 も可）、`blockAlign` 0。データは `u16 LE の長さ + multistream パケット` の並び（長さは 1〜ストリーム数 × 1277）。setup は `ASST.extra*` に必須（版 1、ch、ストリーム数、結合ストリーム数、`mapping[ch]`）。チャンネルは 1〜8: 1〜2 はファミリ 0（1 ストリーム）、3〜8 はファミリ 1（サラウンドのマルチストリーム、`opus_multistream_surround_encoder`）。出力のチャンネル順は WAV の順（4ch は L R BL BR、5ch は L R C BL BR。全チャンネル数の表は bank-format.md §6.5）で、Vorbis の順との並べ替えは buildtool が `mapping` に織り込む（ランタイムは並べ替えない）。デコーダは RFC 7845 として有効な setup を受け付ける（1〜2ch で N ≠ 1 も可、`extra_bytes` はちょうど 4 + ch）。`totalFrames` を出し終えた後の入力は解釈せずに捨てる。`preSkip` はエンコーダの先読み（312）、`preRoll` 3840（80 ms）、`totalFrames` は元の長さで、末尾は無音で埋めた `ceil((totalFrames + preSkip) / 960)` パケット。`SEEK` はパケットの長さの前置の位置と、その再生フレーム（`frame + preSkip` が 960 の倍数）。**バンク形式の版は 2.0 のまま**: チャンク・列・列挙値・ヘッダは変わらず（`OPUS` は 1.0 から `ASST.codec` の列挙にあり、`extraOffset/extraBytes` も 1.0 からある）、決めたのは 1 コーデックのデータの中身だけ。その中身の版は setup の版バイトで持つ（1 以外はデコーダが無効なヘッダとして拒む） | パケット長の前置があれば、パケット境界をシーク表なしで歩けて、ストリームのチャンクの途中で切れたパケットを保持できる（D-23）。2 バイトで 20 ms の最大パケットが収まる。48 kHz に固定すれば Opus の内部レートと一致し、サンプル位置の換算が要らない。古いランタイムでも同じバンクを読める（Opus のアセットだけが鳴らない）ので major を上げる理由が無い |
| D-58 | Opus のデコーダの状態とスロット（A1-19 の見直し）: 状態は `[頭 128 B][入力の保持 16 KiB + 2 + ストリーム数 × 1277][デコード済み 960 × ch の float][libopus の OpusMSDecoder]`（各 64 バイト境界）。`opus_multistream_decoder_init` でスロットの中に置き、実行時に確保しない。D-51 の保持の上限は Opus でも「16 KiB ＋ 1 パケット」の未デコード入力で（1 パケットは最大のパケット = ストリーム数 × 1277 ＋ 2（長さの前置））、これに加えて `max_frames` に入りきらなかった 1 パケット分のデコード済みフレームを持つ（libopus はパケットの途中から再開できないため）。`StateBytes` の実測（libopus 1.5.2）: 1ch 40,192、2ch 52,800、3ch 76,160、4ch 88,768、5ch 112,192、6ch 135,552、7ch 148,160、8ch 171,584 バイト。**旧既定の `decoderSlotBytes` 32 KiB には 1ch も入らないので、既定値を 64 KiB に上げた**（`config.h`。`Config` の既定値の変更は 2 節により契約の変更ではない。受け入れ側の決定で反映）。3ch 以上の Opus は 192 KiB 以上のスロットが要り、足りなければ D-14 どおり `DECODER_POOL_FULL` で再生を拒否する。ランタイムで初めて分からないよう、buildtool は既定のスロットに入らない Opus のアセット（3ch 以上）に必要なバイト数を警告に出す（スロットの大きさを 2 種類にするのは契約の変更になるので、段階2では行わない） | 64 KiB × `maxDecoders` 64 = 4 MiB（現行 2 MiB）。ゲームの Opus はほとんどがモノラルかステレオ。サラウンドの Opus は少数の BGM・環境音で、プロジェクトがスロットを大きくすれば足りる |
| D-59 | libopus のビルド設定: 浮動小数点版、`VAR_ARRAYS`（作業領域は C99 の可変長配列でスタック。`NONTHREADSAFE_PSEUDOSTACK` は大域の作業領域を malloc しスレッド安全でないので使わない）、`ENABLE_HARDENING`（上流の既定）、SIMD の組み込み関数と実行時 CPU 判定は使わない（移植可能な C だけ。どの機械でも同じ結果）、深層学習の拡張（`dnn/`）なし、`-ffp-contract=off`。デコードの `Init` / `Decode` / `Seek` は確保・ロック・IO をしない（`SND_RT_SCOPE` の中で違反ゼロをテストで確認）。スタック使用量の実測はデコードするスレッド全体で 25〜32 KB（1〜8ch、gcc 13 Debug、塗りつぶしたスタックで測定）で、D-47 の 256 KiB に収まる。デコード時間は 1ch・20 ms のパケットあたり約 0.1 ms（Debug、実時間の約 0.5%） | RT 経路から呼ばれうる（先読みジョブ、D-08 の範囲外でもスタックと確保の約束を守る）。SIMD は後から上流の RTCD を有効にする余地を残す（その場合も D-47 のスタックは変わらない） |
| D-60 | Opus のエンコーダ（buildtool、オフラインなので確保可）: `codec::encode::EncodeOpus` / `BuildOpusSeekTable`（`encode.hpp`、ランタイムには入らない）。設定はプロジェクトの `convert` / `platforms` の `"opus": {bitrate, complexity, vbr, signal, independentFrames}`（project-format.md）。既定はチャンネルあたり 64 kbit/s、複雑さ 10、VBR、`OPUS_APPLICATION_AUDIO`、**パケット間の予測なし**（`OPUS_SET_PREDICTION_DISABLED`）。予測なしの CELT パケットはシーク点から 2 パケット後以降が通しのデコードとビット単位で一致するので、`preRoll` 80 ms でシークとループの再開がサンプル単位で正確になる（予測ありだと 80 ms 後も誤差 ~1e-1）。SILK / ハイブリッド（低ビットレートの `voice`）は予測なしでもパケットをまたぐ状態を持ち、80 ms 後も近似（誤差 ~3e-2 以下を観測）。エンコーダの版 `kOpusEncoderVersion`（上位 8 bit が Sonus の配置・既定の改訂、下位 24 bit が libopus の版 0x010502）と Opus の設定を差分ビルドのキャッシュキーに入れる | 6.4「ループ点はサンプル精度」とシーク（D-23）の正確さを、ビットレートの数 % と引き換えに得る。libopus を更新すると出力が変わるので版をキーに入れる |

### `Config` の項目（13.2 と本文の上限・「仮」の値）

| 項目 | 既定 | 出典 |
| --- | --- | --- |
| `sampleRate`, `blockSize`, `renderAheadBlocks` | 48000, 256, 2 | 1.1, 1.3, 2.2 |
| `maxRealVoices`, `maxVirtualVoices`, `fadeReserveVoices` | 128, 1024, 8（仮） | 13.2, 4.4 |
| `maxPlaybacks`, `maxEmitters`, `maxListeners`, `maxTransports`, `maxReservations` | 1024, 512, 4, 8, 256 | 3.1 |
| `maxStreams`, `maxBanks`, `maxUserBuffers`, `maxGlobalParams`, `paramOverflowBlocks` | 32, 64, 32, 256, 256 | 3.1, 6.3（段階0で決定） |
| `eventRingCapacity`, `maxRegisteredThreads`, `listenerMode`, `focusMode`, `seed` | 1024, 8, Nearest, Continue | 2.1, 4.5, 2.6, 2.5 |
| `memory.streamPoolBytes`, `streamChunkBytes`, `pcmCacheBytes`, `pcmCachePageBytes` | 8 MiB, 32 KiB, 16 MiB, 64 KiB（仮） | 13.2, 6.2 |
| `memory.frameArenaBytes`, `threadArenaBytes`, `maxDecoders`, `decoderSlotBytes`, `voiceRingFrames`, `dspPoolSpareBytes` | 256 KiB, 256 KiB, 64, 64 KiB（D-58。段階0〜1は 32 KiB）, 4096, 256 KiB | 3.3（段階0で決定） |
| `commands.arenaBytes`, `arenasPerThread`, `reserveBytes`, `criticalPriority`, `overflowParamSlots` | 64 KiB, 3, 8 KiB, 224, 256 | 2.1, D-10, D-11 |
| `jobs.realtimeWorkers`, `normalWorkers`, `voicesPerChunk`, `external` | 2, 2, 16, なし | 13.2, 2.3 |
| `tuning.rtSpinMicroseconds`, `loadTargetPercent`, `lodDownBlocks`, `lodUpBlocks` | 50, 60, 8, 64（仮） | 2.3, 2.4 |
| `tuning.emitterExtrapolateMs`, `underrunCatchupBlocks`, `callbackAheadBlocks` | 100, 2, 4（仮） | 2.1, 2.2, 5.2 |
| `tuning.streamPreloadMs`, `streamTargetAheadMs`, `ioBytesPer100ms`, `stealHysteresisDb`, `shortFadeMs` | 100, 200, 4 MiB, 3.0, 5.0（仮） | 6.1, 4.4, D-15 |
| `devReserve.buses`, `effects`, `events`, `bytes` | 16, 32, 64, 1 MiB | 11.2 |
| `liveLink.enabled`, `port`, `meterHz`, `sendBufferBytes`, `recordCommands` | 0, 27460, 30, 256 KiB, 0 | 11.2, 11.3 |
| `output.device`, `output.deviceIndex`, `plugins.*` | 既定デバイス, 0, なし | 2.6, D-31 |
| `maxVoiceChannels`, `maxPitchRatio`（1.1、C 構造体では末尾。1〜8 / 1.0〜32.0、NaN・無限大・範囲外は `SND_ERR_INVALID_ARGUMENT`） | 8, 4.0 | D-44 |
| `maxBuses`, `maxCategories`, `maxSnapshots`, `maxLimitGroups`, `maxParamDefs`, `maxDuckRules`（1.2、C 構造体では 1.1 の後ろ。1〜65535） | 128, 64, 64, 64, 512, 64 | D-92 |

### 段階2 platform（出力バックエンドと駆動部品）の決定

契約ヘッダは変えていない（`IOutputDevice` と `snd_output_device` は 1.1 のまま）。以下は platform モジュールの公開ヘッダ
（`platform/include/snd/platform/`）で決めたことと、契約の `IOutputDevice` の約束をどう満たすか。

| # | 決定 | 理由 |
| --- | --- | --- |
| D-61 | 出力バックエンドは `OutputBackend` の `SilentTimer`（全 OS）、`Alsa`（Linux、CMake の `SND_OUTPUT_ALSA` = AUTO / ON / OFF）、`Wasapi`（Windows）、`CoreAudio`（macOS と iOS）。ソースは `platform/src/<os>/` に分け、ビルド時に選ぶ。`Default` はビルドに入った OS のバックエンド（`ResolveOutputBackend`。Linux で ALSA が無ければ `SilentTimer`）。OS のデバイスが開けないとき、黙って無音タイマーに切り替えることはしない（`Open` が失敗し、呼び出し側が決める）。全バックエンドで揃える振る舞い（`PlatformOutputDevice`）: コールバックは FTZ/DAZ を設定し `SND_RT_SCOPE` の中で呼ぶ（無音タイマーも同じ。テストビルドで確保・ロック・IO が検出される）。バックエンド自身もコールバックの前後で確保・ロック・IO をしない（`snd_pcm_writei` や `ReleaseBuffer` などの OS 呼び出しは RT スコープの外、デバイスのスレッドで行う）。`Stop()` は最後のコールバックの終了後に返る。コールバックの中（とデバイスのスレッド）からの `Stop()` / `Close()` は何もせずに数える（`Stats().misuseFromCallback`）。イベントはデバイスのスレッドか OS の通知スレッドから、短い内部ロックの下で配る（リスナーは投函だけ。`SetEventListener` は動作中に替えてよい） | 2.2、2.4（FTZ/DAZ）、D-25。黙った切り替えは「音が出ない」原因を隠す |
| D-62 | コールバックの約束: `frames` は 1〜`PeriodFrames()`。OS が一度にそれより多く求めたときはバックエンドが分割して複数回呼ぶ。OS がそれより少なく求めることもある（WASAPI の空き、Core Audio のスライス）ので、長さは呼び出しごとに変わりうる。一定のブロックで描きたい側は駆動部品（D-66）を挟む（バックエンドは内部のリングを持たない）。`channels` はその時点のデバイス形式で、経路の変化の後に変わりうる。新しい形式での最初のコールバックより前に `CHANGED`（新しい値）を配る。バッファはインターリーブ float32、`SampleRate()` / `Channels()` の形式。float を受け付けないデバイス（ALSA の `hw:`、WASAPI の排他モード）には、バックエンドがデバイスのスレッドで整数に変換する（`S32`、`S24in32`、`S16`。[-1, 1] に切って最近接に丸める、NaN は 0）。`OutputLatencySeconds()` はいま書いたサンプルが聞こえるまでの見積もり（ALSA: バッファ長、WASAPI: バッファ長＋`GetStreamLatency`、Core Audio: デバイスの遅延＋安全余白＋バッファ＋ストリームの遅延＋AudioUnit の遅延、iOS: `outputLatency`＋IO バッファ）。出力位置の時計は `PlatformOutputDevice::OutputClock`（`framesWritten` = Open 以降に OS へ渡したフレーム数、`framesPlayed` = そのうち出ていったと OS が見積もる数、取得時刻） | 契約の `DeviceCallback` は長さを固定すると書いていない。分割だけならコピーが要らず、リングは駆動部品の 1 か所に置ける |
| D-63 | OS 別の実装の決定。**ALSA**: 専用スレッド（Realtime、D-47 のスタック）で、非ブロッキングの PCM に `snd_pcm_writei` で 1 周期ずつ書き、空きが無いときは `snd_pcm_wait`（100 ms で戻り、`Stop()` を待たせない）。mmap ではなく writei にしたのは、全てのプラグイン（dmix、pulse / pipewire、file、null、plug）で使えるからで、代償は 1 周期分のコピーだけ。形式は float → S32 → S16 の順に交渉し、チャンネル数・レート・周期・バッファ（既定 3 周期）は near で決める（`plug` 系は ALSA がレート変換する）。開始時はバッファ 1 本分の無音を先に書く（開始のしきい値 = バッファ長）。`-EPIPE` は `snd_pcm_prepare` して無音を詰め直し xrun として数える。`-ESTRPIPE` は `SUSPENDED` → `snd_pcm_resume` を `-EAGAIN` の間繰り返す（失敗なら prepare）→ `RESUMED`。デバイスが消えた（`-ENODEV` など）ら `LOST` を配って閉じ、`reopenOnLoss` なら同じ名前を一定間隔で開き直し、開けたら `CHANGED`。`OutputDeviceOptions::alsaConfig` を渡すとシステムの設定ではなくその文字列の設定で開く（`snd_pcm_open_lconf`。テスト用）。**WASAPI**: COM のオブジェクトは全てデバイスのスレッド（Open で作り Close まで動く）にあり、そこで COM（MTA）を初期化し、MMCSS（"Pro Audio"）に登録し、`IMMNotificationClient` を登録する。オーナーのスレッドからはコマンドを送って答えを待つだけで、COM に触らない。共有モードはイベント駆動で、要求が混合形式（ミックスフォーマット）と同じで `IAudioClient3` があれば `InitializeSharedAudioStream`（エンジン周期の倍数で要求以上の最小、低遅延）、無ければ `Initialize`。要求のレートやチャンネル数が混合形式と違うときは `AUTOCONVERTPCM` と `SRC_DEFAULT_QUALITY` を付けて WASAPI に変換させる（レート変換は呼び出し側ではなく WASAPI の自動変換。コールバックには常に要求どおりの形式が来る）。排他モード（`OutputDeviceOptions::exclusive`）は float32 → int32 → int24in32 → int16 の順に `IsFormatSupported` で確かめ、`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` は整列した周期で作り直す。`device_index` 0 で名前も無いときは既定（eRender / eConsole）に追従し、既定が変わったら開き直して `CHANGED`。`AUDCLNT_E_DEVICE_INVALIDATED` などと、デバイスが `ACTIVE` でなくなったときは `LOST` を配り、開始中なら一定間隔（デバイスが追加されたときはすぐ）で開き直す。共有モードで目覚めたときにバッファが空なら xrun として数える。**Core Audio**: macOS は既定なら `kAudioUnitSubType_DefaultOutput`（OS が既定の変更に追従する）、選んだデバイスなら `kAudioUnitSubType_HALOutput`、iOS は `RemoteIO`。クライアント形式はインターリーブ float32 の要求どおり（0 はデバイスの値）で、レートとチャンネルの変換は AudioUnit がする。レンダーコールバックが直接 `DeviceCallback` を呼ぶ。macOS は `AudioObjectAddPropertyListener`（既定の出力、デバイス一覧、生存、公称レート、バッファ長）、iOS は `AVAudioSession` の通知（割り込み → `SUSPENDED` / `RESUMED`、経路の変化 → 理由に応じて `CHANGED` / `LOST`、メディアサーバーの再起動 → `LOST` の後にセッションと AudioUnit を作り直して `CHANGED`）。リスナーと通知は印を立てるだけで、処理は専用の直列キュー（dispatch）で制御用のロックの下で行う。チャンネル数をデバイスに合わせる指定（`channels` 0）なら、経路が変わったときにクライアント形式を張り替える。iOS の Objective-C 部分は `ios_audio_session.mm`（ARC 前提、`-fobjc-arc`）。レート変換の責任: ALSA の `hw:` だけはデバイスのレートがそのまま報告されるので、内部レートと違えば呼び出し側（core の最終段、2.2）が変換する。他は OS が要求のレートに変換する（受け入れでの追記）ALSA: `alsaConfig` を指定したら `device_index` は 0 だけ（他は `Open` 失敗、PCM は `deviceName` で選ぶ）。WASAPI: チャンネル数 0 の要求で混合形式が `SND_MAX_CHANNELS` を超える（7.1.4 など）ときは `SND_MAX_CHANNELS` に抑え、AUTOCONVERTPCM で開く（排他モードも同じ値に抑える）。先書きの無音は `ReleaseBuffer` が成功した分だけ書き込み数に数える。Core Audio: チャンネル数の張り替えは「止める → 再初期化 → `CHANGED` → 再開」の順（新しい形式での最初のコールバックより前に `CHANGED`）、張り替えに失敗したら `LOST`。iOS は経路を選べないので、`deviceName` が "default" 以外、または `device_index` ≥ 1 なら `Open` 失敗。割り込みの began / ended がまとめて届いたら最後のものに従う。ended に `AVAudioSessionInterruptionOptionShouldResume` があればセッションを有効にし直して再開し `RESUMED`、無ければ `SUSPENDED` のまま、アプリが `Start()`（`AudioDriver::Resume()`）を呼んだときに再開して `RESUMED`。began を受けていないときの ended は何もしない。チャンネルの張り替えの後は、開始中で割り込み中でなければ再開する。ワークグループの保持配列（16）が満杯なら最も古いものを解放して新しいものを公開する | 設計書 15.2 の未決事項に対するユーザーの決定（Linux / Windows / macOS / iOS）。OS の作法（COM のアパートメント、ARC、通知のスレッド）に合わせつつ、契約の振る舞い（D-61、D-62）を揃える |
| D-64 | デバイスの選び方: `DeviceDesc::device_index` 0 = システムの既定（既定の変更に追従する）、k ≥ 1 = 同じバックエンドの `EnumerateOutputDevices()` の k-1 番目（OS の順序。抜き差しで変わる）。範囲外は `Open` が失敗する。名前で選ぶなら `OutputDeviceOptions::deviceName`（ALSA は PCM 名、WASAPI はエンドポイント ID の UTF-8、macOS はデバイスの UID。列挙の `id` がこの文字列）。名前か番号で選んだデバイスは既定に追従しない | 契約の `device_index` は「0 = 既定」だけを決めていた。順序の変わる番号より名前を保存できるようにする |
| D-65 | ワークグループ（設計書 12.1、2.3）: `PlatformOutputDevice::CurrentWorkgroup(&generation)` が RT スレッドの参加先を返し、`WorkgroupMembership::Join` / `Leave` を参加するスレッド自身が呼ぶ。Apple は AudioUnit の `kAudioOutputUnitProperty_OSWorkgroup`（macOS 11 / iOS 14 以降、`os_workgroup_join`）、Windows は WASAPI のデバイスのスレッドの MMCSS タスク（同じタスク番号で `AvSetMmThreadCharacteristicsW`）、Linux は無し（`NoWorkgroup`。同じ優先度の SCHED_FIFO で足りる）。デバイスの変更で入れ替わると世代が進むので、参加しているスレッドは世代を見て参加し直す（駆動部品のオーディオスレッドは自動で行う。RT ワーカーは jobs の担当）。参加は 1 回ごとにシステムコールなので、スレッドの開始時と世代の変化時だけ行う。入れ替わった古いワークグループは Close まで保持する。あわせて `ThreadPriority::Realtime` を Windows（`THREAD_PRIORITY_TIME_CRITICAL`）と Apple（Mach の time-constraint）でも実装した。Apple の time-constraint は、駆動部品のオーディオスレッドが開始時に実際の周期（デバイス周期とブロック長の大きい方）で掛け直す（`SetCurrentThreadRealtimePeriod`: 周期、計算 = 周期の 50%、制約 = 周期） | RT ワーカーをデバイスの IO スレッドと同じスケジューリンググループに置く（2.3 の優先度逆転対策） |
| D-66 | オーディオスレッドの駆動部品（設計書 2.2、2.4、2.6）: `DriverCore`（スレッド無し、決定的に試験できる）と `AudioDriver`（オーディオスレッドとデバイスの配線）。レンダー関数は `void (*)(void* user, float* interleaved, uint32_t frames, uint32_t channels, const RenderInfo*)` で、固定長 `blockFrames` のブロックをデバイスのレートとチャンネル数で描く。`RenderInfo` はレート、負荷 LOD の段、ブロック番号、前のブロック以降にデバイスが無音で流したフレーム数（`lostFrames`。共通クロックをその分進める）。目標残量 = デバイス周期（`PeriodFrames()`、`CHANGED` の値、これまでのコールバックの最大長のうち最大）＋ `renderAheadBlocks` ブロック。1 回の目覚めで描くのは最大 ceil(周期 / ブロック) ＋ `catchupBlocks`（= `underrunCatchupBlocks`、2.2 の追い付きの上限）。開始前と `RESUMED` の後は目標まで描く。アンダーラン: あるだけ出し、その末尾 `fadeFrames`（既定 64）を 0 へ直線でフェードし、残りを無音にして数え、次のデータの先頭をフェードインする。チャンネル数の変化: コールバックの `channels` が正で、違えば無音を出してオーディオスレッドに求め、オーディオスレッドは新形式の開始位置を返して `FormatFn` を呼び、デバイス側はその位置より前を捨てる。レートの変化（`CHANGED`）も `FormatFn` に渡すが音は捨てない。内部レートへの変換とアップ / ダウンミックスは呼び出し側（core の最終段、2.2、2.6）。負荷: 毎ブロックの処理時間をブロック長 × `loadTargetPercent` と比べ、`LoadMonitor` が `lodDownBlocks` 連続の超過で段を 1 つ上げ、`lodUpBlocks` 連続の余裕で 1 つ下げる（最大 3 段、2.4 の順。段の適用は core）。統計: コールバックの間隔（最小・最大・平均）、処理時間（直近・最大・平均）、予算超過、アンダーラン、失ったフレーム、描いたブロック、残量。レンダー関数とデバイスのコールバックの中からの `Stop()` は何もせずに数える（受け入れでの追記）チャンネル切り替えで捨てた古い形式のフレームは `lostFrames` に含めない（描いた時点で描画側の時計は進んでいるので、足すとずれが 2 倍になる。受け入れでの再決定）。`lostFrames` は無音で出したフレーム数だけで、捨てた量は統計の `droppedFrames` で知らせる。切り替えの直後は描画側の時計がデバイスより最大 `droppedFrames` 先に進むことを許容し、音と映像の同期など正確な再生位置が要る用途はデバイスの `OutputClock` を正とする。iOS の ShouldResume の無い割り込みの後のように、デバイスが SUSPENDED のまま再開を待つときは、イベントをそのまま上に伝え、`AudioDriver::Resume()` で再開する（api は `snd_resume` からこれを呼ぶ）。`Resume()` は `Start()` と同じく、プレフィルの完了を `prefillTimeoutMs` の時間制限つきで待ってからデバイスの `Start()` を呼ぶ（2.6。時間切れなら描いているブロックのレンダー呼び出しが戻るのを待ち、以後は描かず、デバイスは止めたまま false）。OS が自分で再開する場合（iOS の ShouldResume など）は待てないので、残量の不足はアンダーランの扱い（フェードイン、`lostFrames`）で受ける。ack は常に最後のチャンネル変更の位置を運び、デバイス側は新しい ack を見たら書き込み位置を読み直す（読み位置が書き込み位置を越えない。読める量は [0, 容量] にクランプ）。オーディオスレッドが最後の要求に別のチャンネル数で答えたら、デバイス側は要求し直す。フェードインの進み具合はコールバックをまたいで保ち、常に `fadeFrames` かけて 0 → 1 にする。周期の境目ちょうどでリングが空になった場合（直前の出力の末尾がフェードされずに無音になる）のクリックは許容する（遅延を増やさない。アンダーラン自体が異常なので）。`prefillTimeoutMs` を過ぎたら `Start` は失敗するが、そのとき描いているブロックのレンダー呼び出しが戻るまでは待ち、以後のブロックは描かない（`DriverCore::CancelService`）。`Stats()` / `LoadLevel()` はどのスレッドからでも、`Start` / `Stop` と並行に読んでよい（読む値はすべてアトミック）。`Start` / `Stop` / `Resume` / `Close` はオーナースレッドだけから呼ぶ（`Resume` と `Stop` を別スレッドから同時に呼ぶことはできない。api は `snd_resume` と `snd_shutdown` をオーナースレッドで直列に呼ぶ。受け入れでの決定） | core の入口が無い段階で、デバイスに依存しない駆動を platform に置く（2.2 のスレッド表どおり、コールバックはコピーと起床だけ） |

### 段階4（music）での決定

契約の版は変えない（ヘッダの変更なし）。設計書 8 章の解釈と、core ⇄ music の取り決め。

| # | 決定 | 理由 |
| --- | --- | --- |
| D-68 | tick は 4 分音符 = 960（PPQ）。拍は拍子の分母の音符（6/8 なら 480 tick）。BPM は 4 分音符を数える（`snd_transport_desc.bpm` も同じ）。`snd_render_context.transport_beats` も 4 分音符の数（PPQ 位置 = tick / 960）で、拍子の分母の拍ではない。設計書 8.1 を直した。`types.h` の `SND_TICKS_PER_BEAT` の注記は契約 1.2 で直す候補 | 8.1 の「1 拍 = 960 tick」と拍子の分母が食い違っていた |
| D-69 | 最小先読み（8.2）の「1 ブロック」は受理したブロック自身。リード時間はそのブロックの先頭からのプライム時間 | 受理したブロックの中でサンプル精度で発火できる |
| D-70 | NEXT_BEAT / NEXT_BAR / GRID などは、現在位置がちょうど拍頭・小節頭ならその点で発火する（≥） | core の最小トランスポートと同じ |
| D-71 | グリッド系の予約は音楽位置（小節線＋小節内 tick＋offset_ticks）で持ち、毎ブロック、その時点のテンポマップで tick とサンプル時刻を求め直す（後から計画された Segment の拍子・テンポ、SetTempo、シークに追従）。テンポマップは通過済みの点も持つ（同時に 64 点。満杯なら通過済みの最も古い点を捨てる）。1 つの Segment の点の数に上限は無い: Segment の点は現在位置から 4 秒先までを毎ブロック補充する（先の 4 秒に 64 点を超える変化がある場合だけ、入りきらない点は空きが出るまで待つ）。シークは、現在の Segment のテンポ点・拍子点を Segment のデータから組み直す（Segment の小節 0 の小節位置を目標の小節から求めて置き直す。D-75 によりシーク先は常に Segment の中で決まるので、捨てた古い点は影響しない）。音楽位置 → tick はマップ全体から求める | 8.2「テンポ変更やシークが途中で起きても予約は新しい時間軸に従う」 |
| D-72 | SetTempo はその時刻から次の SetTempo まで、以後の Segment のテンポ点を上書きする。マップに戻す手段は契約 1.2 の候補 | 8.1「テンポマップへの上書き」 |
| D-73 | MTRN の nextMarker は `timingCount` をマーカー ID として使う（0 = 任意のユーザーマーカー）。`snd_when` の NEXT_MARKER の `value` 0 も同じ。入口・出口・ピックアップのマーカーは数えない | 列が無い |
| D-74 | sameBar の小節番号は遷移元 Segment の中の小節。遷移先はその番号を自分の小節数で割った余りの小節。拍子が違えばその頭拍、同じなら小節内の位置も保つ | 8.5 |
| D-75 | 音楽 Playback のトランスポートのシークは、いまの Segment を新しい位置に置き直す（Segment 内の位置は Segment 長での剰余）。Playlist はそこから続く | 8.1 のシークと Segment の関係が未定義 |
| D-76 | 置き換え（overlap = replace）で外れた Stinger の予約は REJECTED / RESERVATION_MISSED で終わる。Stinger の予約は 1 ブロック早く発火し、music の PlayEvent の子 Playback は次のブロックの ApplyCommands で CLOCK_SAMPLE 付きに作られる（発火時刻のサンプルから鳴る）。なお現時点で `ReservationAction::Stinger` の予約を作る公開 API は無く、この経路に届くのは内部の口（core の FireList）からだけ。公開 API（MSTG.timing を使う Stinger の再生）は api の段で入れる（契約 1.2 の P6） | 子 Playback は ApplyCommands でしか作れない |
| D-77 | 割り込み遷移で出ていく音のフェードを短くするときは、いまのゲインから新しい終了時刻まで直線（core の StopVoice: リリース中なら max(at, now) + fade に終わる） | 包絡線は 1 本の直線しか持たない |
| D-78 | 「音優先」のストールは次のブロックで検出し、トランスポートを足りなかったフレーム数だけ遅らせる。ストールが続く間（不足したブロックが続く間）は、まだ配送していない拍・マーカーを保留し、不足したブロックごとにその時刻と拍・マーカーのカーソルを同じだけずらす。全フレームを描けたブロックを確認したら配送する。何ブロック続いても各拍・各マーカーは 1 回だけ通知し、誤差は最初の検出前に配送済みのものの最大 1 ブロック（2.5 の「保証しない」） | 8.4 |
| D-79 | Stopped のトランスポートへの予約（NEXT_BAR など）は再生開始位置（小節 0）に解決し、再生開始の瞬間に発火する | 未定義 |
| D-80 | SetMusicState は必達（D-09）。トランスポートが進んでいない間（Paused など）の要求も保持し（最後の要求が勝つ、現在の State なら取り消し）、再生再開後にルールのタイミングで評価する | 8.6 に一時停止中の例外は無い |
| D-81 | Playlist の件数に上限は無い。shuffle の並びは保存せず、パスごとの鍵による [0, n) の全単射で求める（直前のパスの最後と同じものを先頭にしない） | 無言の打ち切りを避ける |
| D-82 | core ⇄ music の取り決め（`core.hpp`）: `ClipRef.trackIndex == kClipRefAssetTrack` なら clipIndex は ASST 行。StartClip の `fadeSeconds` はフェードイン。StopVoice の `startClock` はリリース開始（0 = 今）。`FireEntry.lateRuleApplied == kFireTransportGone` はトランスポート消滅。SetGain は目標値で止まるブロック単位のランプ。契約 1.2 で `music_if.hpp` に移す候補 | music_if.hpp に書いていない |

### 段階2 livelink での決定

| # | 決定 | 理由 |
| --- | --- | --- |
| D-83 | ライブリンクの結果はセッションで照合する。Server は最初のコマンドより前に印（`LiveSetValue{objectId 0, field 0xFFFFFFFE, value = セッション番号}`、livelink 内部の約束）を記録し、`LiveRuntime` は producer ごとに最新の印を覚えて適用結果に付ける。Server は自分のセッション以外の結果を捨てて数える（`ServerStats::staleResults`）。戻しの印は `field 0xFFFFFFFF` | 同じ `LiveRuntime` で Server を作り直すと、前の Server の未適用のコマンドの結果が新しい Server の Ack に結び付いた。`LiveSetValue` に空きの欄が無いので、印のコマンドで区切る |
| D-84 | 再生（Replayer）が core に渡すコマンドは、規則の表（`livelink/command_check.hpp` の `kCommandRules`）で検査する。表は種別ごとに「API の入口の規則」（いまの `recorder.cpp` と完全に同じ。API が受けるものは通し、弾くものは弾く）と「記録側が書く値の決まり」（ハンドルの形、scope と target、0/1、フラグ、0 以上に揃えたフェード）の 2 つを持つ。api 段で C API の入口もこの表を使う | 記録は外部入力。API と再生の規則が食い違うと、API で受けたものが再生で消える、またはその逆になる |
| D-85 | core は API が受ける極端な値で落ちない・UB にならない: bpm は 1e-3〜1e5、pitch は 0〜16（非有限は 1）、1 ブロックで通知する拍は 256 まで、予約の待ちは 4e18 サンプルで頭打ち（NaN は 0）、フェード・時間は NaN を既定値、上限 1 日、音量は 0〜1000（+60 dB）、スナップショットの重みは 0〜1。契約 1.2 で API の入口を締める（範囲は段階2 livelink の報告） | API が受ける値（巨大な bpm や音量、+Inf のフェード）で無限ループや非有限の出力、整数変換の UB が出た |
| D-86 | ライブリンクの応答と背圧: 各 seq にちょうど 1 回答える。適用された結果の Ack は seq 順、受け付け時の拒否の Ack はそれより前の適用待ちより先に届くことがある。受信は適用待ちの結果と応答の送信待ちの空きの分だけ行い、コマンドバッファの空きは見ない（`SND_ERR_COMMAND_BUFFER_FULL` の再送はツールの責任）。切断は心拍のタイムアウトか、ツールが送信をまったく読まないときだけ（livelink-protocol.md §5 の末尾） | Ack を大量に求めると送信待ちがあふれ、読んでいるツールを切断していた |
| D-87 | 古いセッションの編集は適用しない: `LiveRuntime` は最新のセッション番号を知っており、それより古いセッションのアリーナにある編集（`LiveSetValue`、`LiveReplaceChunk`、`LiveOverride`、`LiveClearOverrides`）を適用せず捨てて数える（`RuntimeDrops::staleEdits`、ReplaceChunk の枠は解放）。core は `LiveTap::admit` で `LiveOverride` / `LiveClearOverrides` の適用前に問い合わせる。セッションは `Server::Start` の中（スレッドを起動する前）で始まる（Start から戻った時点で前の Server の未適用の編集はもう適用されない）。例外: 切断時の戻し（revertOnDisconnect の `LiveClearOverrides` と戻しの印 `field 0xFFFFFFFF`）は古いセッションでも適用する。戻しの `LiveClearOverrides` は直前の印 `field 0xFFFFFFFD`（同じ producer）でツールの `LiveClearOverrides` と区別する。戻しの印は、そのセッションが最後に書いたセルだけを元に戻し、新しいセッションが書いたセルは戻さない（上書き層は各セルに最後に書いたセッションを覚える）。オーバーライドも同じ: livelink は各オーバーライド（種別, ID）を最後に設定したセッションを覚え、古いセッションの戻しの `LiveClearOverrides` は全消しの代わりに、そのセッション以前が最後に設定したものだけを外す（core の `System::LiveRemoveOverride`）。最新のセッションの戻しと `LiveClearOverrides` は全部を消す。戻しの開始の印は、続く 1 つの `LiveClearOverrides` で消費され、セッションの印でも消える（未完の戻しが後のセッションに残らない） | 前の Server が Destroy の時点で公開しきれなかったアリーナが新しい Server の後に適用され、新しいツールの値を古い値で上書きしえた |

### 契約 1.2（api 段）での決定

契約 1.2 の候補一覧のうち api 段で入れたもの。すべて追加（minor）。版は 1.2（冒頭、受け入れ側の決定・ユーザー承認）。

| # | 決定 | 理由 |
| --- | --- | --- |
| D-88 | **トランスポートのピッチ**: `snd_set_transport_pitch(sys, transport, pitch, when)`（C++ `SetTransportPitch`）。コマンドは新しい種別を足さず、`cmd::SetTempo` の予約欄を `variant`（`kTempoVariantBpm` = 0 = 従来、`kTempoVariantPitch` = 1）にして表す（1.1 の記録は 0 を書いていたので互換）。入口の規則は bpm と同じ（正の有限、規則表 `SetTempo` をそのまま共有）、core が [1/16, 16] に丸めて `ReservationAction::SetPitch`（末尾に追加、テンポと同じトランスポート変更、"今" も予約を通す D-18）を作る。`Transport.pitch` は tick の進む速さに掛かり（テンポ × ピッチ）、その transport を所有する音楽 Playback のボイスのピッチにも掛かる（8.1「テンポとピッチが連動」、EvaluateDspParams が `Playback.transportSlot` と `Transport.pitch` を読む。読み出しの宣言を schema.hpp に追加）。ボイスのピッチはさらに `Config.maxPitchRatio` で頭打ち。Stinger など子 Playback と単独トランスポートに従う合成ボイスには掛けない。規則表の shape に `variant` ≤ 1 を足した | 8.1 の「トランスポートのピッチ（再生速度）」を API にする（P6）。新しいコマンド種別を足すと 1.1 のテスト（最後のコマンド種別の検査）が壊れるため、予約欄で表した |
| D-89 | **Stinger の公開 API**: `snd_play_stinger(sys, music, stinger, when)`（C++ `PlayStinger`）。`stinger` は音楽 Event の `stingers`（MSTG）の id。`when` が NULL / IMMEDIATE なら MSTG.timing / timingCount（NEXT_MARKER なら timingCount はマーカー ID、D-73）をその Playback のトランスポートで使い、それ以外の種類は上書き（`snd_set_music_state` と同じ形）。コマンドは `cmd::SetMusicState` の `variant = kMusicStateVariantStinger`（state 欄が MSTG id）。ApplyCommands が `ReservationAction::Stinger` の予約を作る（ペイロード: MSTG id、`kStingerPayloadRuleTiming`）。music の Resolve が規則のタイミングを MSTG から引く。Stinger の予約は 1 ブロック早く発火し、子 Playback は発火時刻のサンプルから鳴る（D-76 のまま）。overlap replace の Stinger は、同じ音楽 Playback の同じ MSTG id のより古い未発火の予約を取り消す（ハンドルが無いので終了通知は無い）。stack は積む。音楽 Event に無い id は何も鳴らさない（入口では成功）。stinger id 0 は `SND_ERR_INVALID_ARGUMENT`（規則表の api 側に追加） | P6、D-76 の「公開 API は api の段で」。予約の作り手を ApplyCommands に置けば、テンポ変更・シークに追従する音楽位置の予約（D-71）と遅れの規則がそのまま使える |
| D-90 | **`snd_render_context.transport_beats`** は 4 分音符の数（PPQ 位置 = tick / 960、D-68）。合成ボイス（`snd_play_synth`）は開始の `when` が名指すトランスポート（種類を問わず、生きていれば）に従う。無ければ -1、バス・エフェクトは -1。core は RunEventLogic の最後に、従うトランスポートの位置からブロック先頭の値と 1 サンプルあたりの 4 分音符の数を求め、`RenderItem` の末尾に足した `transportBeats` / `transportBeatsPerSample` に写す（行を読まない遅れたチャンクの約束 D-43 のとおり、直列区間で写す）。トランスポートが消えたら -1。`types.h` の `SND_TICKS_PER_BEAT` の注記も 4 分音符に直した。`RenderItem` は core だけが作る内部の構造体で、末尾への追加は既存のソース種別のコードを変えない（minor として扱った） | D-68 と music の再確認（c63a6e8）で api 段の検証項目とされた。合成ボイスにトランスポートを渡す欄が `snd_synth_desc` に無く、構造体を伸ばすと ABI が変わるので、開始の `when` で決めた |
| D-91 | **snd_create の Config の検査**（A15-3）: 版の major が違えば `SND_ERR_VERSION_MISMATCH`。`struct_size` は 1.0 の大きさ（`max_voice_channels` の手前）、1.1 の大きさ（`max_buses` の手前）、現在の大きさ以上（新しい minor）だけを受け付け、それ以外（フィールドの途中、1.0 より短い）は `SND_ERR_VERSION_MISMATCH`。短い構造体の続きは読まず、`snd_config_init` の既定値で補う（1.0 の呼び出し側には 1.1 と 1.2 の既定値）。続けて `max_voice_channels` 1〜8、`max_pitch_ratio` は有限で [1, 32]（NaN・±∞・1 未満・32 超は `SND_ERR_INVALID_ARGUMENT`）、1.2 の容量 1〜65535、プラグインの表（数が正なら表が要る、各 32 まで）、ライブリンクを動かすときの `max_registered_threads` ≤ 63 を確かめる。他は core の `Create` が同じ結果で検査する。`snd_declare_language` の実体は BankManager の名前の表（D-50、呼んだスレッドで同期登録してから必達コマンド）。プラグインの表・ジョブシステム・デバイスの関数表は snd_system の中に写す（C++ のラッパーが渡すのは一時オブジェクト。core はエフェクトの表を指し続ける） | A15-3。1.1 の呼び出し側が構造体の後ろのゴミを読まれないように |
| D-92 | `Config` に 1.2 の容量 `max_buses`（128）、`max_categories`（64）、`max_snapshots`（64）、`max_limit_groups`（64）、`max_param_defs`（512）、`max_duck_rules`（64）を足した（C 構造体の末尾、`SND_CONFIG_TOP_1_2`）。構成ルートが `CoreDesc::limits` に写す（既定は従来の `CoreLimits` と同じ） | core の候補。CoreDesc::limits はゲームから設定できなかった |
| D-93 | **構成ルートと C API**: C API（`snd/sonus.h` の全関数）は `sonus`（`api/compose/`）に置く。記録関数の入口は、記録するコマンドのペイロードを作って livelink の規則表 `RuleOf(type).api`（`livelink/command_check.cpp`、表は 1 つ）で検査し、通らなければ `SND_ERR_INVALID_ARGUMENT`（ハンドルを返す関数は無効ハンドル）。値 0 のハンドルはその前に `SND_ERR_INVALID_HANDLE`（D-29）。古いハンドルの判定は core。再生できない（ゲームのメモリを指す）コマンドの関数は core の検査だけ。`sonus_api` は依存表どおり core と music だけ（中身はモジュール名だけ）。作る順: Config の検査 → FS（ゲームのもの、無ければ `io::DefaultFileSystem`、完了通知を IO スケジューラに）→ 通常ワーカー（`jobs.external`、無ければ `jobs::WorkerPool`）→ コーデック表 → StreamPool と IoScheduler（チャンネル数 = `max_streams`）と IO スレッド → SourceKinds → [ライブリンク: LiveRuntime::Create → Install] → core::System::Create（sources、`music` = snd_system が持つ MusicSystem（core より長く生きる）、jobs、bankScheduler = IO、bankUnregistered、findCodec、容量、出力遅延）→ [Bind] → AudioDriver::Start → [Server::Create → Start]。壊す順はその逆（Server::Destroy → AudioDriver::Stop → io.Shutdown → ワーカー Shutdown → core Destroy → LiveRuntime::Destroy → SourceKinds::Destroy → io / pool Destroy → デバイス、FS）。途中で失敗したらそこまでを壊して返す。ライブリンクは `Config.livelink.enabled` かつ `SND_LIVELINK=1` のビルドのときだけ動き、core には `max_registered_threads + 1` を渡す。`snd_update` と `snd_get_bank_chunk` は作ったスレッド以外から `SND_ERR_WRONG_THREAD`。core の `outputLatencySeconds` はデバイスを開く前の見積もり（core を先に作るため）。api は LiveOverride を記録しないので「セッション 0 は戻しの対象外」の配慮は要らない | D-03、候補一覧の「作る順・壊す順」と livelink の配線。規則表を livelink から動かさずに共有するため C API を構成ルートに置いた |
| D-94 | **Callback と UserBuffer のソース種別**（source モジュール、`source::Runtime` が登録）。Callback: 先行描画（既定）は通常ワーカーのジョブが声ごとのリング（`ahead_blocks`、0 なら `callbackAheadBlocks`）に描き、レンダーは写すだけ。リングは声の playhead と歩調を合わせて消費し（f フレーム目は開始から f フレーム後に聞こえる）、間に合わなかったフレームは無音にして捨て、その後はリングが ahead ぶんたまるまで鳴らさない（5.2）。最初のブロックは必ず無音。リングは min(`maxRealVoices`, 32) 個、無ければ開始を `VOICE_LIMIT` で拒否。低遅延は RT ワーカーで同期的に呼び、1 ブロックの時間を超えた呼び出しの次のブロックから無音にして先行描画に降格（リングが無ければ無音のまま）。コンテキスト: clock_sample（先行描画は最後に描いたブロックからの外挿）、playback、param_* は無し（1.2 の合成には Parameter の表が無い）、FIRST_BLOCK / LOW_LATENCY、transport_beats（D-90）。合成はピッチを反映しない。`cpp_callback`（C++ のラッパー）は同じコンテキストを `snd::RenderContext` として呼ぶ（`CallbackParams` の注記）。RELEASING の印は立てない（未実装）。UserBuffer: 元のレートで読む種別（core がリサンプル）。Stream はゲームの書いた分だけ読んで `readPos` を進め、無い分は無音（鳴った後なら starved）で終わらない。1 つの Stream バッファを読む再生は同時に 1 つ（2 つ目は `VOICE_LIMIT`）。Static は書いたフレームを鳴らして終わる（loop なら繰り返す）。バッファが解放されたら次のブロックで終わる | transport_beats（D-90）を合成コールバックで検証するのに Callback 種別が要った。`snd_play_user_buffer` も種別が無く常に UNSUPPORTED だった。Input（デバイス入力）は platform に入力が無いので UNSUPPORTED のまま |
| D-95 | **出力の橋渡し**（構成ルート）: core は `Config.output_channels`（0 なら 2）で描き、デバイスのチャンネル数が違えば写す（共通のチャンネルはそのまま、モノラルのデバイスには L と R の平均、モノラルの core はデバイスの 1・2 チャンネルへ、余るデバイスのチャンネルは無音、余る core のチャンネルは捨てる）。デバイスのレートが内部レートと違っても変換しない（D-62 の ALSA `hw:` の場合）。最初に気づいたときに `on_error(SND_ERR_UNSUPPORTED, 0, レート)` を 1 回。`RenderInfo::lostFrames`（アンダーランでデバイスが流した無音）は、次のブロックから最大 `underrun_catchup_blocks` ブロックずつ捨てて描いて共通の時計を追い付かせる（2.2「アンダーラン時も進む」）。デバイスのイベント: LOST は次の `snd_update` で `on_error(SND_ERR_NO_DEVICE, SND_DEVICE_EVENT_LOST, 0)`、`SND_FOCUS_SILENCE` ならフォーカスを失っている間は出力を 1 ブロックかけて 0 に（戻すときも 1 ブロック）。`SND_FOCUS_PER_CATEGORY` は core にカテゴリの印が無いので CONTINUE と同じ（未実装）。`snd_suspend` は Suspend を記録するだけでデバイスは止めない（core が無音を描いて時計を止める）。`snd_resume` は Resume を記録し、デバイスが OS の都合で SUSPENDED のまま（RESUMED が来ていない）なら `AudioDriver::Resume()` を呼ぶ（作ったスレッドならその場で、他の登録スレッドなら作ったスレッドの次の `snd_update` で。D-66 の「オーナースレッドだけ」） | D-62、D-66 で呼び出し側の責任とされたもの |
| D-96 | （core の修正）`snd_create_transport` と `snd_destroy_transport` が同じブロックで適用されると、Publish が一度もその行を見ないためスロットが返らなかった。ApplyCommands が作った行の世代を覚え、Publish が返す | api の入口のテストで見つかった。スロットが尽きると `snd_create_transport` が無効ハンドルを返し続ける |
| D-97 | （受け入れ側の決定）**合成の `user` の寿命**: `on_end` は、その再生のコールバックの最後の呼び出しが戻った後にだけ届ける。先行描画のジョブが投入済みか実行中なら、構成ルートが `on_end` の記録を預かり（`snd_update` の中、作ったスレッド）、ジョブの終わりを待ってから出す（`source::Runtime::CallbackBusy`）。低遅延の呼び出しは遅れたチャンクの保留（D-43）でボイスが残るので待つ必要が無い。**Buffered とリング**: 実ボイスだけがリングを持つ。仮想ボイスはリングを返し、リングが空いていない実ボイス（開始時・実に戻ったとき）は無音のまま空きを待つ（仮想のまま待つのと同じ扱い。拒否しない、`callbackRejected` は待った回数）。**Static の UserBuffer** は容量の全体（`frames`）を鳴らす（メモリに直接書く使い方を正とする）。Stream は `writePos` までを鳴らす。**壊すことが決まったバッファへの再生は鳴らさない**: `DestroyUserBuffer` を適用した時点で鳴っている再生は止め、その後（同じブロックの後の記録、または解放待ちの間）に適用された `PlayUserBuffer` は始めずに `SND_END_DISCARDED` で終わる（on_end はちょうど 1 回）。記録の側で既に壊されていると分かるものは従来どおり記録しない。`PlayUserBuffer` の適用では必ずバッファの世代を確かめ、参照（解放を待たせる数）を取るのは世代が現在のもので、壊すことがまだ適用されていないときだけ。解放済み（世代が合わない、例えば音声側が解放を済ませたがゲームがまだ読んでいない間の記録）や壊す途中なら参照を取らず、`When` が何であっても（止めたトランスポートの NEXT_BAR で Pending のままでも）始めずに `SND_END_DISCARDED` で終わる。参照を取る経路はこの 1 か所だけで、Static / Stream、仮想、ループのどれも同じ。一時停止したボイスも `Virtualize` を受けてリングを返すので、再開してから ahead の分だけ無音になる（受け入れ済みの振る舞い）。リングを返すのは `Virtualize` を受けたボイスだけで、D-43 により保留中（遅れたチャンクが描いている）ボイスには `Virtualize` が来ないので、返すリングを遅れたチャンクが書いていることはない | 受け入れでの決定（独立テストの仕様の穴、欠陥 `IndepFix.Defect_PlayRecordedAfterDestroyDoesNotKeepTheBuffer`） |
| D-98 | （受け入れ側の決定）ライブリンクのスレッドの登録枠: `Server::Start` はスレッドを起こした後、そのスレッドが `RegisterThread` を済ませるまで待って戻る（失敗なら止めて `SND_ERR_LIMIT_REACHED`）。よって `snd_create` が戻った時点で D-93 の「+1」の枠はライブリンクが持っており、ゲームのスレッドが先に取ることはない | 独立テストの欠陥 `IndepEntry.Defect_LiveLinkThreadPlaceCannotBeTakenByTheGame` |
| D-99 | snd_create の確保の失敗: どの確保が失敗しても、途中まで作ったものだけを壊し、確保したものは全部ゲームのアロケータに戻して `SND_ERR_OUT_OF_BUDGET` を返す。core::System::Create の失敗の後の Destroy はサービスのキューが無ければ触らない（core の修正）。デバイスとオーディオドライバの確保は見張り付きのアロケータを通し、確保の失敗（`OUT_OF_BUDGET`）とデバイスが開けないこと（`NO_DEVICE`）を区別する | 独立テストの欠陥 `IndepCreate.Defect_AllocationFailure*` |

追記（受け入れでの決定、D-93〜D-95 の注記）:

- D-93: 入口の検査の順序は、値 0 のハンドル → `SND_ERR_INVALID_HANDLE`、規則表で値の誤り → `SND_ERR_INVALID_ARGUMENT`、古いハンドル（世代の不一致）は core が記録時に見つけて `SND_ERR_INVALID_HANDLE`、記録後に古くなったものは適用時に無視（D-29）。
- D-90 / D-95 の 4 件（transport_beats は開始の `when` のトランスポート、アンダーランで失ったフレームは描いて捨てて追い付く、Stream の UserBuffer は同時に 1 つだけ読む、取り消された stinger は通知しない）はこのまま決定とする。
- D-95: `snd_suspend` は時計を止めて無音を描くだけで、**デバイスは開いたまま**（止めない）。コールバックを入れる前に起きた通知（レートの違い、LOST）は消さずに印だけ持ち、`on_error` が入った後の最初の `snd_update` で 1 回だけ届ける。`outputLatencySeconds` は、デバイスを開いた後に `core::System::SetOutputLatency` で実際の値に置き換える（次のブロックの通知時刻と統計から）。
- D-94 の Callback の注記にある「Buffered の開始でリングが無ければ `VOICE_LIMIT`」は D-97 で置き換えた（待つ）。

### 段階6 組み込み（Ergo から取り込む）での決定

契約の版は 1.2 のまま（契約ヘッダは変えない）。凍結ファイルの変更はルートの `CMakeLists.txt` だけで、受け入れ側の決定で
「サブプロジェクトとして安全にする変更・オプションの追加・`tool/` を読む条件の変更」に限って許した。`cmake/` は変えていない。

| # | 決定 | 理由 |
| --- | --- | --- |
| D-100 | **サブプロジェクトとしての取り込み**: 親（ゲームエンジン）は `add_subdirectory(<sonus>)` で取り込み、窓口の別名ターゲットを使う。`Sonus::sonus` = C API（`snd/*.h` を含む公開ヘッダの場所）と構成ルート `sonus`（全ランタイムモジュール）。実体は INTERFACE の `sonus_c_api` で、`$<LINK_ONLY:sonus>` でリンクだけを伝え、`sonus_contract_public` の `cxx_std_20` は伝えない（C ヘッダは C11 / C++17 で通る。C++ のラッパー `snd/sonus.hpp` は C++20 のまま）。ルートの変更: `SND_BUILD_TESTS` の既定を `PROJECT_IS_TOP_LEVEL`（サブプロジェクトでは OFF、`SND_RT_CHECKS` の既定もそれに従う）、ビルド種別の既定（Debug）はトップレベルのときだけ設定、`cmake/` は `include(<パス>)` で読む（親のモジュールパスの同名ファイルを拾わない）。それ以外はもともと親を変えない: 警告・`-Werror`・`-fno-exceptions -fno-rtti`・`-ffp-contract=off`・サニタイザは `snd_apply_options` でターゲットごと、`CMAKE_CXX_STANDARD 20` とモジュールパス・`add_compile_definitions` / `add_link_options` は Sonus のディレクトリの中だけ、パスは `CMAKE_CURRENT_SOURCE_DIR` / `PROJECT_SOURCE_DIR`。検査はテスト `embed_subproject`（`tests/embed/`）: C++17・例外 ON・ディレクトリ全体に `-Wall -Wextra -Werror` の親から取り込み、親の C++ 標準・ビルド種別が変わらないこと、テストが既定で無いこと、Sonus のターゲットが `-fno-exceptions` のままのこと、`snd/sonus.h` だけで（C++17、`-Wpedantic -Wshadow -Wconversion -Wsign-conversion` でも警告なし）無音デバイスを作って回して壊せること、`Sonus::tool_core` が Tela なしでリンクでき C++20 を伝えることを確かめる。`install()` とパッケージ設定ファイルは作らない | Ergo（C++17）にサウンドとして組み込むため。親の設定を壊さず、親の例外・標準が Sonus に入っても Sonus のターゲットの設定が後から勝つ |
| D-101 | **ツールの核を Tela なしで**: `SND_BUILD_TOOL_CORE`（既定 OFF）で UI に依存しない `sonus_tool`（別名 `Sonus::tool_core`、`sonus::tool_core`。`cxx_std_20` を INTERFACE で伝える）だけを作る。`SND_BUILD_TOOL=ON` は従来どおり核・UI・`sonus-tool`（Tela の `find_package` は UI とアプリの部分だけ）。ルートは `SND_BUILD_TOOL OR SND_BUILD_TOOL_CORE` のとき `tool/` を読む。テストも分けた: モデルのテスト（model、live_mixer、live_profiler、indep_sync、新しい core_host）は核だけで、UI・アプリのテストは `SND_BUILD_TOOL` のときだけ。外のホスト向けに `Session` へ `Tick()`（引数なし）、`NowNs()`（`Tick(nowNs)` の時計。クライアントの受信時刻と比べるので同じ時計が要る）、`StartRecording()` / `StopRecording()`（ライブリンクのクライアントのヘッダ無しで録画できる）を足した。ホストはポーリング型（自分のループから `Tick`、コールバックは無い）、1 つの `Session` へは同時に 1 つのスレッドから（作ったスレッドでなくてよい）。Tela のツールの振る舞いは変えない | Ergo Studio が自前の UI でミキサー・プロファイラ・上書き・録画を使うため。Tela が無い環境でも核をビルド・テストできるように |

### 段階6 ホスト API（Ergo の組み込みで足りなかったもの）での決定

Ergo への組み込み（Ergo の PR #99）で C API に足りなかった 3 つ。すべて追加（minor）。1.2 は公開前なので版は 1.2 のまま
（受け入れ側の決定）。契約ヘッダの変更は `config.h`（`snd_output_kind` と `SND_CONFIG_TOP_1_2_HOST`、`snd_config` の末尾）、
`sonus.h`（関数 4 つ）、`sonus.hpp`（同じものの C++ 版）だけで、既存の構造体の途中には何も入れていない。内部の契約
（`commands.hpp`、`schema.hpp`）と D-91 の `struct_size` の規則は変えていない（1.2 の途中の大きさは公開されていないので受け付けない）。

| # | 決定 | 理由 |
| --- | --- | --- |
| D-102 | **内蔵の無音デバイス**: `snd_config.output_kind`（`SND_OUTPUT_PLATFORM` = 0 = 既定・従来どおり、`SND_OUTPUT_SILENT` = 1、`SND_OUTPUT_MANUAL` = 2。C++ `Config::outputKind` / `snd::OutputKind`）。`output.device` が NULL のときだけ使う（ゲームのデバイスと PLATFORM 以外の組、未知の値は `SND_ERR_INVALID_ARGUMENT`）。SILENT は platform の `TimerOutputDevice`（自分のスレッドで実時間）、MANUAL は新しい `platform::ManualOutputDevice`（`OutputBackend::Manual` = 5 を末尾に追加。スレッドもタイマーも無く、`Advance` の中でだけ呼んだスレッドでコールバックを回す）。どちらも内部レート・`output_channels`（0 なら 2）・1 周期 = `block_size` で開く（`DeviceDesc.period_frames` に渡す）。手で進めるのは `snd_advance_output(sys, periods, out, out_frames)`（C++ `AdvanceOutput`）: 作ったスレッドだけ（他は `WRONG_THREAD`）、MANUAL でない系は `UNSUPPORTED`、`out` は NULL か periods × block_size フレームのインターリーブ（足りなければ `INVALID_ARGUMENT`）。各周期の前に、オーディオスレッドが先行描画の目標まで描き終えるのを待つ（最長およそ 2 秒、過ぎたらそのまま進めてアンダーランとして時計が追い付く）ので、ゆっくり進めても描いたフレームは順にすべて届き、最初に届くフレームは共通の時計の 0 | Ergo はテスト用に自前の null デバイス表を書いていた。platform の無音デバイスは C API から選べず、手で進めるデバイスは無かった（モックの `NullOutputDevice` はテスト枠組みの中） |
| D-103 | **内蔵のファイルシステムの根**: `snd_config.file_system_root`（UTF-8、C++ `fileSystemRoot`）。`snd_create` の `file_system` が NULL のとき `io::DefaultFileSystem` の根になる。NULL / "" は作業ディレクトリ（従来どおり）。絶対パスか、`snd_create` の時点の作業ディレクトリからの相対で、そこで一度だけ解決する（後の chdir に動かされない。POSIX は realpath、Windows は GetFullPathNameW と最終パス）。存在するディレクトリでなければ `SND_ERR_NOT_FOUND`、2046 バイトを超えれば `INVALID_ARGUMENT`、ゲームのファイルシステムと一緒に渡せば `INVALID_ARGUMENT`。文字列は写す。**バンク・ストリームのパスは根からの相対だけ**: 絶対パス（`/` 始まり）、`\`、`:`（ドライブ名 `C:`、Windows の代替ストリーム。どの OS でも同じに振る舞うよう POSIX でも拒否）、根の上に出る `..` は拒否（`ValidatePath`）。リンクは解決後の実体が根の中なら辿り、外なら拒否（従来の D-24 の決定 b）。普通のファイルだけを開く。**OS 別の実装を分けた**（`io/src/native_file.hpp`: POSIX = Linux・macOS・iOS、Win32、その他は常に失敗）。Windows は以前の「Open が常に失敗するスタブ」を Win32 の実装に置き換えた: UTF-8 → UTF-16（不正な UTF-8 は拒否）、根を開いて `GetFinalPathNameByHandleW` の最終パス（`\\?\` 形式）を持ち、相対パスを字句的に正規化して繋ぎ、`CreateFileW`（共有は読み・書き・削除、ディレクトリは開かない）、`GetFileType` がディスクでディレクトリでないこと、開いたハンドルの最終パスが根の最終パス + `\` で始まること（大文字小文字を区別しない。シンボリックリンクとジャンクションは OS が解決するので入れ替えの競合も無い）、読み出しは OVERLAPPED のオフセット付き `ReadFile`。macOS / iOS は POSIX の実装（openat2 は無いので realpath と dev/inode の照合）。アプリのバンドルから起動した時の作業ディレクトリはバンドルではないので、絶対パスの根を渡す | Ergo は作業ディレクトリを変えずにアセットの場所を指したい。Windows の既定のファイルシステムは何も開けなかった |
| D-104 | **再生ごとの音量とピッチ**: `snd_set_playback_volume(sys, playback, volume)` / `snd_set_playback_pitch(sys, playback, pitch)`（C++ `SetPlaybackVolume` / `SetPlaybackPitch`）はどの種類の再生にも効く（イベント、合成、user buffer）。音量は線形（≥ 0、core が 1000 で頭打ち）でその再生の全ボイスのゲインに掛かり、ピッチはレート比（> 0、core が [1/16, 16] に丸め、ボイスの他のピッチと合わせて `max_pitch_ratio` で頭打ち）で全ボイスのピッチに掛かる。合成はピッチを反映しない（D-94）。新しい再生は 1 / 1 から始まり（`CreatePlayback` が戻す）、変更は適用したブロックから 1 ブロックかけて移る（ボイスのゲイン・ピッチの Ramp）。入口: sys NULL → `INVALID_ARGUMENT`、ハンドル 0 → `INVALID_HANDLE`、NaN・±∞・負の音量、NaN・±∞・0 以下のピッチ → `INVALID_ARGUMENT`、古いハンドルは core が `INVALID_HANDLE`。**コマンド**: 新しい種別は足さず、再生スコープの `cmd::SetParam` に予約 id（`core::kParamPlaybackVolume` = 0xFFFFFFFD、`kParamPlaybackPitch` = 0xFFFFFFFC。`bank_format::kInputDistance` = 0xFFFFFFFE の隣）で載せる。よって必達で、予備領域と合体のフォールバック（D-11）でも最後の値が残り、規則表（有限の値）と再生ファイルの形の規則もそのまま通る。ApplyCommands はこの id をパラメータの行にせず `ApplyPriv::playbackVolume / playbackPitch`（再生のスロットごと）に写し、EvaluateDspParams が掛ける。他のスコープとスイッチではこの id は何もしない。**開始時の指定**: `snd_play_user_buffer_ex(sys, buffer, bus, volume, pitch, emitter, when)`（C++ `PlayUserBuffer(sys, b, UserBufferPlay{bus, volume, pitch}, ...)`）は PlayUserBuffer と、1 でない値の SetParam を同じアリーナに続けて記録する（同じ ApplyCommands で順に適用されるので最初のブロックから効く。開始の `when` が先でも Pending の行に入る）。値が拒否なら無効ハンドル。**Static の UserBuffer は複数の読み手を許す**（もともと音声側は Static のメモリを読むだけで、位置はボイスごと、参照の数も再生ごと。D-97 の「Static は容量の全体を鳴らす」のまま）。写さずに 1 つのバッファを何本でも同時に、それぞれの音量・ピッチで鳴らせる。Stream は従来どおり同時に 1 つ（D-94） | Ergo は再生のたびに波形を写し、`sample_rate` を変えてピッチの代わりにしていた。再生ハンドルの音量・ピッチの API はもともと無かった |

追記（受け入れでの決定、D-102〜D-104 の注記。独立テスト c4b8ee2 の欠陥と仕様の穴）:

- D-102: 手で進める周期の前の待ちは、リングの残量（`DriverStats::queuedFrames >= targetFrames`、オーディオスレッドが描く条件と同じ）で判定する。描いた量 − 渡した量は、アンダーランで流した無音（描いていない）を含むので、一度上限で抜けると二度と成り立たず、以後の周期が毎回 2 秒待っていた（欠陥 `IndepHostOutput.Defect_BoundedWaitThenBackToNormal`）。
- D-91 に足す: `struct_size` は、1.2 の最初の大きさ（host のフィールドの手前、`offsetof(snd_config, file_system_root)`、`kConfigSize12`。1.2 は 8c81f4e で版として出ている）も受け付け、host の既定値（`file_system_root` = NULL、`output_kind` = PLATFORM）で補う（欠陥 `IndepHostOutput.Defect_EarlierOneTwoStructSizeGetsTheHostDefaults`）。
- D-103（Win32 の長い根）: 根を開くときも、ファイルを開くときも、`\\?\` の形（共有は `\\?\UNC\`）に直してから `CreateFileW` に渡す（`GetFullPathNameW` の結果を直し、`\\.\` のデバイスの名前は拒否）。設定が許す 2046 バイトまでの根で動く。
- D-103（大文字小文字）: どちらの OS でも、OS が返す最終パス（正規の名前）同士を、大文字小文字を区別して比べる（POSIX は realpath / `/proc/self/fd`、Windows は `GetFinalPathNameByHandleW` の結果で、ディスク上の綴り。ディレクトリごとに区別する設定でも正しい）。入力のパスの大文字小文字をどう合わせるかは OS に任せる。
- D-103（POSIX で確かめる前に開かない）: 種類と場所を確かめる前に、普通でないファイルを読み出し用に開かない。Linux は `O_PATH` の記述子（openat2 の RESOLVE_BENEATH でも、EXDEV などの代わりの経路でも）で `fstat`（普通のファイル）と `/proc/self/fd/N` の正規のパス（根の中）を確かめ、その記述子から `/proc/self/fd/N` で開き直す（確かめた inode そのもの。dev / inode も照合）。`O_PATH` か `/proc` が無いとき（macOS / iOS など）は、realpath が根の中で、その正規のパスの `lstat` が普通のファイルのときだけ `O_NOFOLLOW` で開き、開いたものが同じ dev / inode であることを確かめる。
- D-104（予約 id）: `SND_ID_RESERVED_FIRST`（0xFFFFFFFC）〜`SND_ID_RESERVED_LAST`（0xFFFFFFFE）を `snd/id.h` に置いた。公開の `snd_set_param_*` はこれを `SND_ERR_INVALID_ARGUMENT` で拒否する。定義と規則は規則表に一か所（`livelink::IsReservedId`、`SetParam` の api 規則と `CheckOverflowParam`）: 記録された `SetParam` が予約 id を持てるのは、`snd_set_playback_*` が記録する形（再生スコープ、音量 ≥ 0 / ピッチ > 0）だけで、再生ファイルの検査もこれに従う。buildtool は名前（エイリアスを含む、種類を問わない）のハッシュが予約 id に当たればエラーにする。スイッチの入口（`snd_set_switch_*`）は従来どおり規則表を通さない（core は予約 id のスイッチを無視する）。
- D-104（子の再生）: 音量とピッチは、その再生自身のボイスにだけ掛かる（ボイスの `playbackSlot` で引く）。Stinger や音楽のアクションが作る子の再生は別のスロット・ハンドルの再生で、1 / 1 から始まり、親の値は及ばない。
- D-11 / 統計: `reserve_overflows` は従来どおり（D-11 と既存の独立テストの約束）失われた値の数とし、`types.h` の注記をそれに合わせた。合体のフォールバックに入れた（届けた）コマンドの数は、`snd_stats.reserved0` を `fallback_commands` にして数える（大きさを変えない。`snd_stats` は `notify.hpp` の `SnapshotFrame` に埋まっていて、末尾に足すと内部の契約の配置とライブリンクの統計が変わるため）。**受け入れ側の指示（`reserve_overflows` に合体も数え、失われた数を別に）とは逆の割り当てで、既存のテスト（`core_system_test`、`indep_core_cmd_test`、`indep_core_fixcheck_test`）の約束を守るための選択。受け入れ側の判断を待つ。**
- 既存の `PlatformOutputDevice::OutputClock`: seqlock の読み手が 64 回で諦めて偽の失敗を返していたのを直した（欠陥）。書き手が書いている間（奇数）と食い違ったときは yield して、整合した値が読めるまで読む。書き手は奇数にするのを acq_rel の fetch_add にして値の書き込みがその前に動かないように、読み手は値を acquire で読んで 2 度目の数の読み出しがその前に動かないようにした（fence は TSan が扱えないので使わない）。

### 契約 1.2 の候補で入れなかったもの

api 段の依頼の範囲（トランスポートのピッチ、Stinger、transport_beats）と、api の配線に要るものを入れた。残りは受け入れ側の判断のため候補のまま。理由は段階2 api の報告。

## 4. モックとテスト枠組みの約束

- テスト実行ファイルは `snd_add_test(<name> SOURCES ... LINK ...)` で作る。1 実行ファイル = 1 CTest。
- 各テストの終わりに、`ExpectViolations` の外で記録された RT 違反があればそのテストを失敗にする。`SND_RT_ABORT=1` で最初の違反で abort。
- モックの差し替え点: C++ API では `Config.output.device = &nullDevice`, `Config.jobs.external = &jobs`, `Create(cfg, alloc, &memFs, &sys)`。
  C API では `snd::ToC(...)` で関数表にして渡す。
- 枠組みとモックの API（ヘッダ）を変えるときは 2 節の手順に従う。実装の修正と機能の追加は tests 担当の判断でよい。
- 負荷 LOD（2.4）は仕様どおり壁時計で判断する。テストは LOD そのものを試すもの以外 `Config.tuning.lod_down_blocks = 0` で LOD を切る（混んだ機械でボイスが仮想化されて結果が変わらないように）。

## 5. 設計書の矛盾・不足（段階0で見つけたもの）

設計書は書き換えていない。以下は設計書側で解消すべき点と、契約での暫定の扱い。

| # | 箇所 | 内容 | 契約での扱い |
| --- | --- | --- | --- |
| G-01 | 3.1 | Playback の `eventId` を表ではホット列、本文ではコールド列としている | D-19 |
| G-02 | 3.1, 3.2 | Stream の列の書き手（IO スレッドとデコードジョブ）、バスゲイン（スナップショット・ダッキング・カテゴリ）、`Voice.audibility`、`RealVoice.panMatrix`、Emitter の補間状態、カテゴリとスナップショットの状態の置き場所が表に無い | D-17, D-18。Stream の外部の書き手は 1.1 の D-45 で解消（行が動かない Direct 表、スロットで添字） |
| G-03 | 12.1, 13 | api の依存先が core と music だけだが、`Create` はソース種別・既定 FS（io）・既定デバイス（platform）・livelink を必要とする | D-03 |
| G-04 | 12.1, 4 | source は dsp に依存しないが、4 章の「リサンプル」をどこでするかが無い | D-22 |
| G-05 | 13.1 | UserBuffer を鳴らす関数、トランスポート・UserBuffer の破棄、単独トランスポートの開始・停止、非所有スレッドの公開と登録解除、`ICodec`/`IDspEffect` の登録、物理モジュールがバンクのマテリアル表を読む手段、`SetSwitch` のスコープが無い | D-28, D-31 |
| G-06 | 12.3 | `ICodec::Decode` が消費バイト数を返せない。`IFileSystem` にサイズ取得が無い。`IOutputDevice` に開始・停止・閉じる・チャンネル数が無い | D-23, D-24, D-25 |
| G-07 | 2.1 | 「優先度の低い再生要求から拒否」は線形アリーナでは追い出しができず、ゲーム側は優先度を知らない | D-10 |
| G-08 | 2.1, 13.1 | 問い合わせは「任意のスレッド」だが、トリプルバッファの読み手側スワップは 1 スレッド前提 | D-16 |
| G-09 | 13.1 | `Resume` がシステムと Playback で同名（C では衝突） | D-28 |
| G-10 | 9.5, 13.1 | `PlayAt` はワンショット（9.5）だが 13.1 では再生系としか書いていない | D-28 |
| G-11 | 10.2 | 「依存するバンクの一覧」の識別方法と照合の基準が無い | D-32 |
| G-12 | 8.1, 13.2 | 音楽 Playback が所有するトランスポートのハンドルを得る方法が無い（13.2 の `musicTransport`） | D-05 |
| G-13 | 7, 8.2 | 7 章で `PlayScheduled` / `PlayDelayed` を「同等」としているが 8.2 の `When` に絶対時刻・遅延が無い | D-21 |
| G-14 | 12.3, 1.2 | `IJobSystem::Submit` を「どのスレッドから呼んでもよい」とすると、オーディオスレッドからの投入がホスト実装次第でロック・確保を含みうる | D-08 |
| G-15 | 2.1 | 「停止・パラメータ・解放系は予約領域」だが、Emitter 等の生成を落とした場合の扱いが無い（後続の操作が宛先を失う） | D-09 |
| G-16 | 6.1, 3.1 | Stream の表にデコード済みリングの書き込み位置が無い（`readPos` は IO の位置） | `Stream.ringWrite` を追加 |
| G-17 | 3.2 | RunEventLogic が「Voice の終了」を書くが、ソースの終端を知るのは Render | `Voice.renderStatus`（Render が書き、RunEventLogic が前ブロック値として読む） |
| G-18 | 4.1, 13.1 | 「未ロード」は 4.1 の終了理由の一覧に無い | `SND_END_NOT_LOADED` を独立の理由にした |
| G-19 | 15.1 | 段階1 で bank と io を並列に作るが bank は io に依存する | bank 担当はメモリ FS のモックで単体を完結させる（6 節） |

## 6. 段階1の担当者への注意

共通:

- 自分のモジュールのディレクトリ（`<module>/include/snd/<module>/`, `<module>/src/`, `<module>/test/`）だけを触る。
  依存は宣言済み（ルート `CMakeLists.txt`）なので `CMakeLists.txt` にはソースとテストを足すだけ。
- 公開ヘッダに出してよい型は契約・base・platform のものだけ（D-02）。他モジュールのヘッダが必要なら契約変更の提案。
- RT 経路のコードは `SND_RT_SCOPE()` の中でテストする。ロックのラッパー・`IFileSystem`・書式付きログ・`IAllocator` 実装には
  `SND_RT_NOTE(...)` を置く。リリース相当（`-DSND_BUILD_TESTS=OFF`）でもビルドが通ること。
- 例外・RTTI なし、警告ゼロ（gcc 13 / clang 18、`-Werror`）、`SND_SANITIZE=address,undefined` と `thread` でテストが通ること。

base / platform / jobs:

- base は `schema::TableRows`（密配列＋疎インデックス＋世代）の操作、SPSC（コマンドの公開と返却、通知リング、`postJob`/`postIo` の有界キュー）、
  アリーナ、固定ブロックプールを提供する。通知のビット配列とシーケンスロックの手順は `notify.hpp` の冒頭どおりに実装する。
- `SND_RT_NOTE(Lock)` を入れたミューテックスのラッパーを base に用意し、非 RT コードはそれを使う。
- jobs の通常ワーカーは `IJobSystem` を実装し、`snd::RunJob` で契約（取り消しと完了カウンタ）を守る。決定論モック（`tests/mocks`）と同じテストが通ること。
  RT ワーカーのチャンク締切（2.3）は `IJobSystem` を通さない。
- platform の SIMD 抽象と FTZ/DAZ は dsp と core が使う。デバイスは `IOutputDevice`（D-25）。テストはモックの無音デバイスと同じ使い方で書く。

dsp:

- 標準エフェクトは仮想関数を通さず種別ごとのループで処理する。プラグインは `IDspEffect`（in-place 可、D-26）。
- バッファは 64 バイト境界、非インターリーブ、float32。リサンプラは core から source のレートの入力を受ける（D-22）。
- ゴールデン音声は `dsp/test/golden/` に置き、許容差を決めて `SND_CHECK_GOLDEN` で比較する。

codec / io:

- `ICodec` の Decode / Seek の意味は D-23（`snd/ext.h` のコメントが正）。状態はデコーダプールのスロット（`decoderSlotBytes`）に収まること。
- io の既定 FS は `IFileSystem`（D-24、IO スレッドからだけ呼ばれる）。各関数の先頭で `SND_RT_NOTE(FileIo)`。
  IO スケジューラは `source::IoReadRequest` を受け、優先度と 100 ms あたりの帯域で並べる。遅延と失敗はメモリ FS のモックで試験する。

bank / buildtool:

- バンク形式は `bank-format.md` と `bank_format.hpp`。`format_test.cpp` の `MinimalBankRoundTrip` が最小の読み書き例。
  検証項目は `bank-format.md` §8。壊れた入力で落ちないことをファズで確認する。
- ID は `snd_hash_id` / `snd::HashId`（D-04）。buildtool はプロジェクト全体で衝突検査し、ID 表とゲーム用ヘッダ（`"..."_id` と同じ値）を出す。
- bank はロード経路（IO → 通常ワーカーで検証 → `cmd::BankRegister` を投入）を持つが、オーディオスレッド側の参照表の差し替えは core の担当。
  `BankRegister::prepared` が指す構造は bank が定義し、core と合意してから bank の公開ヘッダに置く。

## 7. 変更履歴

| 版 | 日付 | 内容 |
| --- | --- | --- |
| 1.0 | 2026-11-15 | 段階0で凍結 |
| 1.0（バンク形式 2.0） | 2026-11-15 | バンク形式 2.0: headerChecksum をヘッダ全体に、予約バイト 0 を検証、`ALIS` チャンク、行の並びの規則、`loopEnd`/`SEEK`/ストリームファイル/エンディアン/`fileBytes` の明文化（bank-format.md §7） |
| 1.1 | 2026-11-15 | 段階1の持ち越し A1-1〜A1-10 を反映（D-43〜D-55）: 遅れたチャンクの規則と `Voice.renderHold`、`RealVoice.resamplerState` の廃止と `Config.maxVoiceChannels` / `maxPitchRatio`、Stream を Direct 表に、`snd_effect_desc.sample_rate`、依存表の buildtool に dsp、bank-format.md の追記（形式 2.0 のまま）、`SND_ERR_BANK_ALREADY_LOADED` と `snd_declare_language`（コマンド `DeclareLanguage`）、`decode` の約束、ハンドルの払い出し、RT スレッドのスタック、`notify.hpp` の注記、`CCodec` の既定構築、検証規則の共有ヘッダ `bank_check.hpp`（D-53）、DSP プールの予約をランタイムで計算（D-54）、`WhenKind::AtClockSample`（D-55）、RealVoice を Direct 表にして playhead / renderStatus を RealVoice に（D-43）、Bank を D-46 の対象から外す。列の削除と Stream の索引方式の変更は厳密には追加ではないが、段階0以降にそれらを使う実装が無く、契約も未公開なので minor として扱った（受け入れ側の決定） |
| 1.1（Opus） | 2026-11-16 | 契約の版・バンク形式の版は変えない。D-56〜D-60: サードパーティ依存の例外として libopus 1.5.2 を `third_party/opus/` に同梱し `SND_CODEC_OPUS`（既定 ON）でビルド（ルート `CMakeLists.txt` にオプションと `add_subdirectory(third_party)`）、Opus を組み込みコーデックに、bank-format.md §6.5 に Opus のバイト配置と setup、project-format.md に Opus の設定、デコーダのスロット（既定の `decoderSlotBytes` を 32 KiB から 64 KiB に上げた。`config.h` の X マクロの既定値とコメント、受け入れ側の決定。3ch 以上の Opus は buildtool が必要なスロットを警告。`codec/test/encode_test.cpp` の既定値の検査を機械的に追従） |
| 1.1（Opus、差し戻し） | 2026-11-17 | 独立テストの欠陥と仕様の穴の決定: 終端後の入力（セクタの埋めなど）は解釈せず捨てる、3〜8ch の WAV / Vorbis の並びの表、`loadForm: "pcm"` の Opus はビルドエラー、レート変換後のフレーム数 `ceil(元 × 新 / 元のレート)`（全コーデック共通）、保持の「1 パケット」= ストリーム数 × 1277 ＋ 2、`extra_bytes` はちょうど 4 + ch、1〜2ch の N ≠ 1 も受け付ける、`Seek` の境界の規則（bank-format.md §6.5、project-format.md、D-51 / D-57 / D-58 の文言） |
| 1.1（段階2 platform） | 2026-11-17 | 出力バックエンド（ALSA、WASAPI、Core Audio）と駆動部品の決定 D-61〜D-66 を追記。契約ヘッダと版は変えていない（platform の公開ヘッダでの決定）。ルートの `CMakeLists.txt` に iOS のときだけ `enable_language(OBJCXX)` を追加 |
| 1.1（段階2 livelink） | 2026-11-17 | D-83〜D-87 を追加（契約ヘッダは変えない。core への追加: `CoreDesc::liveTap`、`LiveTap::admit`、`System::Live*`。livelink-protocol.md に応答・背圧・セッションの段落を追加。ライブリンクの版は 1.0 のまま） |
| 1.1（ツール D-67） | 2026-11-17 | **D-67 ツールの UI は LUDIARS Tela**（設計書 11 章・15.2 の「Dear ImGui か Qt か」をユーザーの決定で置き換え）。契約の版は変えない（tool は凍結対象の外）。(1) Tela は同梱も自動取得もしない: `-DSND_BUILD_TOOL=ON` のときだけ `tool/CMakeLists.txt` が `find_package(Tela CONFIG REQUIRED)` で読む（`Tela_DIR` / `CMAKE_PREFIX_PATH`）。既定の構成（OFF）は Tela を必要としない。(2) 使う部品: `Tela::Core`（宣言・配置・ヒット領域・ボタンの状態・ジェスチャーの所有）と `Tela::Raster`（CPU 描画）をすべての OS で、`Tela::Windows`（`WindowsView`）/ `Tela::MacOS`（`MacOSOverlay`）と `Tela::Pictor`（文字）をホストだけで。Linux はウィンドウのホストが無いので `--headless` で画像を出す。(3) サードパーティ依存の例外: Tela（LUDIARS 自前）と、Tela のホストが要る Pictor（Windows / macOS のホストのみ）。(4) 例外と RTTI: Sonus のコードは従来どおり `-fno-exceptions -fno-rtti`。Tela は検証失敗で例外を投げるので、宣言・描画の前に Sonus 側で範囲を検査して投げない呼び方だけをする（`tool/ui/safe_drawing.hpp`、`App::SetViewport` 等）。投げうる Tela のホスト API（`PictorSurface`、`WindowsView`、`MacOSOverlay`）を呼ぶ `tool/app/host_windows.cpp`・`host_macos.mm`・`pictor_text.cpp` だけを `-fexceptions` でコンパイルし、境界ですべて捕まえる。(5) フォントは同梱しない: ウィンドウのホストは `--font <ttf>` が必須。**D-67a**（独立テスト後の決定）: 受け付ける DPI は 0.25〜4（`--dpi` と `App::SetViewport` で同じ。8 では最悪の処理時間グラフが Tela の作業予算に収まらない）、図形の作業量は実際の DPI で見積もり、超えるときは線を分けるか点を間引く。記録ファイルは版 2（チェックサムがヘッダとレコードを覆い、読めないレコードは拒否）。詳細は `tool/README.md` |
| 1.2（api 段） | 2026-11-18 | D-88〜D-99（D-97〜D-99 は独立テスト後の受け入れの決定）: `snd_set_transport_pitch`（`cmd::SetTempo` の `variant`、`ReservationAction::SetPitch`、EvaluateDspParams の読み出し 2 つ）、`snd_play_stinger`（`cmd::SetMusicState` の `variant`、MSTG.timing、`kStingerPayloadRuleTiming`）、`transport_beats` の注記と `RenderItem` 末尾の 2 欄、`SND_TICKS_PER_BEAT` の注記、`Config` 末尾の容量 6 つ（`SND_CONFIG_TOP_1_2`）、D-82 の定数を `music_if.hpp` へ、`CallbackParams` の注記、規則表の 1.2 の規則。C API の本実装と構成ルート、Callback / UserBuffer 種別、出力の橋渡し、同じブロックで作って壊したトランスポートのスロット返却（core）。版を 1.2 に上げた（`SND_CONTRACT_VERSION_MINOR = 2`、`contract11_test.cpp` の版の検査を「minor が 1 以上」に。受け入れ側の決定、ユーザー承認） |
| 1.2（段階6 組み込み） | 2026-11-19 | D-100〜D-101: サブプロジェクトとしての取り込み（`Sonus::sonus`、ルートの `SND_BUILD_TESTS` の既定を `PROJECT_IS_TOP_LEVEL`、ビルド種別の既定はトップレベルだけ、`cmake/` をパスで読む）、`SND_BUILD_TOOL_CORE` と `Sonus::tool_core`（`tool/` を読む条件を `SND_BUILD_TOOL OR SND_BUILD_TOOL_CORE` に）、`Session` のホスト向けの関数、テスト `embed_subproject` と `tool_core_host_test`。契約ヘッダと版は変えない（凍結ファイルの変更はルートの `CMakeLists.txt` だけ、受け入れ側の決定） |
| 1.2（段階6 ホスト API） | 2026-11-19 | D-102〜D-104: `snd_config` の末尾に `file_system_root` と `output_kind`（`SND_CONFIG_TOP_1_2_HOST`、`snd_output_kind`）、`snd_advance_output`、`snd_set_playback_volume` / `snd_set_playback_pitch`、`snd_play_user_buffer_ex` と C++ 版（`OutputKind`、`AdvanceOutput`、`SetPlaybackVolume` / `SetPlaybackPitch`、`UserBufferPlay`）。platform に `ManualOutputDevice`（`OutputBackend::Manual`）、io の OS 別の実装（`native_file*.cpp`、Windows の実装、パスの `:` の拒否）、core の予約パラメータ id と再生ごとの音量・ピッチ。版は 1.2 のまま（公開前、受け入れ側の決定）。内部の契約と D-91 は変えない |
| 1.2（段階6 ホスト API、差し戻し） | 2026-11-19 | D-102〜D-104 の注記: 手動デバイスの待ちをリングの残量で、`struct_size` に 1.2 の最初の大きさ（`kConfigSize12`）、Win32 の `\\?\` の形、最終パスを大文字小文字を区別して比べる、POSIX は確かめる前に開かない（O_PATH / lstat）、予約 id（`snd/id.h` の `SND_ID_RESERVED_*`、規則表、`snd_set_param_*` の拒否、buildtool のエラー）、子の再生には及ばない、`snd_stats.fallback_commands`（`reserved0` を改名）、`OutputClock` の seqlock |

## 段階 7-A（physics、設計書 9 章）

セッション A（`int/s7-physics`）の決定。**契約（`contract/`）は変えていない**。版の数字も、コマンド種別も、スキーマの列も、凍結ファイルも変えていない。
新しい公開 API は physics の持ち場のヘッダ（`physics/include/snd/physics/physics.h`、`physics.hpp`）に置き、凍結ヘッダ `snd/physics.h` の型と 4 関数はそのまま本実装にした。
動作の正確な規則は `physics/include/snd/physics/physics.h` の冒頭のコメントが正で、下の表はその決定と理由。他の持ち場への依頼は `spec/handoff/parallel/requests/A-001`〜`A-004`。

| # | 決定 | 理由 |
| --- | --- | --- |
| D-110 | **API の置き場所と型**: 新しい関数は `snd_physics_*`（C、`physics/include/snd/physics/physics.h`）と `snd::phys` の薄いラッパー（C++、`physics.hpp`、ヘッダだけ）。凍結ヘッダ `snd/physics.h` を include して足す形で、オブジェクトは `snd_phys` の 1 種類（`snd_physics` は別名）、`snd_contact`（64 バイト）・`snd_surface_weight`・接触種別・モーションは凍結の型をそのまま使う。凍結の 4 関数（`snd_phys_create` / `destroy` / `submit_contacts` / `play_footstep`）は本実装（`physics_stub.cpp` は削除）。`snd_phys_create` は `snd_phys_desc` を設定に写して作り、`bank` と `table` が両方あればそのバンクの `PHYS` から表を読む（両方無ければ空の表。片方だけは `INVALID_ARGUMENT`。無効なバンクハンドルも凍結ヘッダの一覧に合わせて `INVALID_ARGUMENT`）。`impulse_threshold == 0` は表の閾値 | 指示書は `snd_physics_*` と `struct_size` の流儀を求めるが、凍結の `snd_phys_*` は既にあり消せない。同じものの型を二重にしない。契約を変えないので、4 セッションの統合で衝突しない |
| D-111 | **設定・スレッド・メモリ**: `snd_physics_config` は先頭に `struct_size` と `version`（`snd_physics_config_init` で既定値）。小さい `struct_size` は前半だけを読み残りは既定値、**大きい `struct_size` は既知の部分だけを読む**（`snd_create` と同じ向き）。`version` は互換でない変更のときだけ上げる番号で（項目の追加は `struct_size` が伸びるだけ）、知らない値は `INVALID_ARGUMENT`。8 未満・4 の倍数でないもの・範囲外の値は `INVALID_ARGUMENT`。`snd_physics_table_desc` と `snd_physics_stats` も `struct_size` つき。呼べるのは `snd_phys` を作ったスレッド（システムの登録スレッド）だけで、ほかのスレッドでは `SND_ERR_WRONG_THREAD` で何も起きない（破棄も何もしない。オブジェクトは生きたままで、作ったスレッドで破棄し直せる）。バンクを読む関数は `snd_get_bank_chunk` に従いシステムの所有スレッドだけ。メモリは生成時に `allocator`（NULL は malloc 系）から 1 回だけ取り、以後は確保しない（表の差し替えも固定容量へのコピー）。足りなければ `OUT_OF_BUDGET`、容量超えは `LIMIT_REACHED`。既定の容量: 追跡 64、クールダウン 256、作業領域 2048、材質 64（上限 1024）、ゲーム材質 1024、保持する Emitter 64 | 9 章「ゲームスレッドで動く」、3.3 の「初期化の後に確保しない」。別スレッドの破棄でメモリだけ解放すると、追跡中のループ音と Emitter が残る |
| D-112 | **データの置き場所**: API で渡す形が正（`snd_physics_set_table` ほか。全機能を表せる）。加えて、今のバンク形式 2.0 の `PHYS` チャンクを `snd_get_bank_chunk` で読む（`snd_physics_load_bank_table`、`snd_phys_create`）。physics からは契約の内部層が見えないので、`PHYS` の配置は bank-format.md §6.2 に従って physics の中に同じ配置の構造体を持ち（大きさを `static_assert`）、中身は信用せず検査する（範囲・整列・個数、非有限、閾値の逆転、`\|cents\|` > 4800、`\|dB\|` > 200 は `SND_ERR_CORRUPT`）。`PhysCurves`（t0〜t3、cents 2、dB 2）の写し方: 衝撃量→音量 (t0, .25)(t1, .5)(t2, .75)(t3, 1)、衝撃量→ベロシティ (t0, 0)(t1, 1/3)(t2, 2/3)(t3, 1)、質量→ピッチは 1〜100 kg を対数 8 点で 2^(c/1200)、速度→音量は 0.1〜10 m/s を対数 8 点で 10^(d/20)、表の閾値は各材質の t0 の最小、足音はモーション共通の列。`PHYS` で表せないもの（接触種別ごとの組の上書き、モーション別の足音、点列のカーブ、ゲーム材質の対応表と表面パラメータ、閾値）はバンク形式 2.1 への依頼 A-001 | 指示書 2.3。`PHYS` は形式 2.0 に既にあり buildtool も出しているので、形式を変えずに読める。音量の 0.25 刻みは形式に音量の欄が無いための仮の写しで、A-001 の点列カーブが入れば不要になる |
| D-113 | **表と代替の順（9.3）**: 単体の Event `[N][4 種別]`、組の上書きは種別ごとに `pair[k][min(a,b)*N + max(a,b)]` の 1 回の参照（0 = 上書きなし）、全体の既定 `[4 種別]`。順は 組 → 両方の単体を重ねる（片方しか無ければある方だけ。同じ材質どうしは 1 回だけ）→ 全体の既定 → 何も鳴らさない。**材質不明**（対応表に無い、`UNMAPPED`、N 以上）: 組は引かない。片方だけ不明なら分かっている側の単体（無ければ全体の既定）、両方不明なら全体の既定。どの段で決まったかを統計に数える（`resolved_pair` / `fallback_pair_to_single` / `fallback_single_to_default` / `fallback_none`、材質不明は加えて `fallback_unknown_material`）。表は実行中に丸ごと・1 項目ずつ差し替えられ、次の呼び出しから効く。追跡中の継続接触は Event を開始時のまま保ち、カーブだけ新しい表を使う | 9.3。片方が分かっているなら、その材質の音の方が全体の既定より情報がある |
| D-114 | **対応表と表面パラメータ（9.1）**: 対応表を登録すると、`snd_contact.material_a/b` と足音の material を「ゲームの物理マテリアル番号」として写す。未登録なら恒等（番号はサウンドマテリアル番号そのもの。凍結ヘッダのコメントどおり）。1 項目は「サウンドマテリアル番号 + 表面パラメータ（Parameter の ID と値）4 つまで」。単体の音にはその材質のもの、組の上書き・全体の既定の音には両方の材質のもの（同じ ID は大きい方）を Playback スコープの Parameter で渡す。値だけを実行中に変えられる（`snd_physics_set_surface_param`）。1 項目の設定で番号が飛ぶときは、間を恒等で埋める。まとめとクールダウンのキーは写した後のサウンドマテリアルで、まとめた組の表面パラメータは代表の接触のもの | 9.1「別マテリアルにせず、表面パラメータとして Event に渡す」 |
| D-115 | **強さのカーブ（9.3）**: 材質ごと（未設定なら表の既定、それも無ければ組み込み）の点列の区分線形（最大 8 点、x は非減少で同じ x は段差、範囲外は端の値）: 衝撃量→音量、衝撃量→ベロシティ、質量→ピッチ、相対速度→擦れ音の音量、相対速度→擦れ音のピッチ、重み→足音の音量（既定は恒等）。y の範囲は登録時に検査する（音量 [0, 1000]、ピッチ [1/16, 16]、ベロシティ \|y\| ≤ 1e6。範囲外・非有限は `INVALID_ARGUMENT`）。単体の音はその材質のカーブとその物体の質量（A の音は massA）、組の上書き・全体の既定の音は軽い方の物体の質量とその物体の材質のカーブ（同じなら A。非有限・負の質量は 0 扱い）。Slide / Roll のピッチは速度のカーブだけ（質量は Parameter で渡すだけ）。音量・ピッチは値がちょうど 1 のとき記録しない | 9.3「ツールのカーブで決める」。範囲を登録時に断れば、使うときに丸めで形が変わらない。軽い物体の方が高く響く |
| D-116 | **時刻**: `dtSeconds` を設定の上限（既定 0.1 秒）で頭打ちにし、µs の整数に丸めて `uint64` に積算する。クールダウン・タイムアウトも µs の整数で比べる。dt が負・NaN・無限大は何もせず `INVALID_ARGUMENT`（時刻も進めない）。`count == 0`（`contacts` は NULL 可）でも時刻・予算の端数・タイムアウトは進む。毎フレーム呼ぶ（一時停止中も dt 0 で呼ぶ。サブステップで 1 フレームに複数回呼んでよい） | 境界（ちょうど）を件数で厳密に検査できるようにする。長い停止の後に予算が跳ねない |
| D-117 | **間引き（9.4）**: Impact / Break を 1 回のパスで、この順。(1) 閾値: Impact は `impulse < 閾値` を捨てる（ちょうどは通す）。非有限・負の値と未知の種別は「不正」として別に数える。(2) まとめ: キーは（格子、材質の組（順序なし）、種別）。格子は軸ごとに `floor(position / grid_size)`（ちょうど k×g は k）。代表は衝撃量最大（同値は配列で先）で、位置・質量・物体・材質の向きは代表のもの。個数（1 以上）を Parameter で渡す。(3) クールダウン: キーは代表の（物体の組（順序なし）、材質の組（順序なし）、種別）。記録が無いか、最後に鳴らしてからの経過がクールダウン以上（ちょうどは通す）なら通す。記録するのは (4) を通って実際に再生を要求したものだけ。同じ呼び出しの中で先に鳴らした同じキーも効く（予算は消費しない）。表が溢れたら、最後に鳴らした時刻がいちばん古い記録から追い出す（同時刻は先に記録したもの）。(4) 予算: Break を先、それぞれ衝撃量の大きい順（同値は配列で先）。枠は `carry + budget_per_second × dt` の整数部で、端数だけを持ち越す（使わなかった整数分は持ち越さない。毎秒 300・1/60 秒ならちょうど 5）。予算は `SubmitContacts` のワンショットだけに掛かる。Event が何も決まらない組は (3) の前に捨てる。作業領域（まとめた組）は固定容量で、溢れは `dropped_work_overflow` | 9.4。設計書に境界の規則が無い。予算で落ちたものがクールダウンに入ると、次のフレームでも鳴らせない |
| D-118 | **Break**: ワンショット。表の Break の列を使う。閾値は掛けない（壊れるのは 1 回きりで、衝撃量が小さくても落とさない）。まとめ・クールダウン・予算は掛かり、予算では Impact より先。追跡表には触らない（壊れた物体の Slide / Roll はタイムアウトで止まる） | 指示書 2.2「Break の扱いを決める」。`snd_contact` からは、どちらの物体が壊れたかが分からない |
| D-119 | **位置つきの再生と Emitter**: `snd_play_at` はハンドルを返さず Parameter・音量・ピッチを渡せないので、Impact / Break は `snd_create_emitter` → `snd_set_transform` → `snd_play`（重ねる 2 音は同じ Emitter）→ Parameter・音量・ピッチで鳴らす。Emitter はすぐには破棄しない（下の注）。保持して、後の `snd_phys_submit_contacts` で、**音声側がその再生を適用したと観測できてから**破棄する: 各再生の `snd_get_playback_state` が NONE 以外、または終わって古くなっている（NONE のとき、既に持っている音量で `snd_set_playback_volume` を記録して `INVALID_HANDLE` が返る）。物理の時刻には依らない。保持は `max_held_emitters`（既定 64）までで、**満杯なら早めに破棄せず、新しい組を捨てる**。**Emitter が取れない組（保持が満杯、`snd_create_emitter` の失敗）は `snd_play_at` に落とさず、捨てて `dropped_no_emitter` に数える**（予算の枠は消費したまま、クールダウンは記録しない）。容量の目安: システムの `max_emitters` ≥ ゲーム自身の分 + `max_tracked_pairs` + `max_held_emitters`、`max_held_emitters` ≥ 1 回の `snd_update` のあいだに鳴らす組の数 × 適用までの周期（約 3）。単発の再生の音量とピッチは physics が決める（観測で受け取ったハンドルで音量を変えても、Emitter を返すまでは physics が元の値で上書きしうる）。**分かっている制限**: 時間をおいて入れ子の Event を鳴らすアクションを持つ Event では、Emitter を返した後に始まる子の音が位置を失う（A-004 の 5 節。子の音が終わるまで Emitter を持つと、音の長さのぶんゲームの Emitter を占有するのでやらない）。`snd_physics_play_at` は `snd_play_at` そのまま（9.5 の位置だけのワンショット）。注: 同じ `snd_update` の前に Emitter を作成・移動・再生・破棄すると、`sonus.h` の「再生は最後の transform を保つ」が成り立たずリスナーの位置で鳴る（core の欠陥、依頼 A-004。独立テストが公開 API だけで再現）。ハンドルを返す `PlayAt`（依頼 A-002）が入れば保持は要らなくなる | Emitter が枯渇すると `snd_play_at` の暗黙の Emitter も同じプールから取れず、位置なし・減衰なしで鳴る。遠くの衝突が耳元で鳴るのは、鳴らないより悪い（保持が満杯のときに早めに破棄するのも同じ理由でやめた）。保持を物理の時刻で測ると、1 回の `snd_update` の前のサブステップで位置を失った |
| D-120 | **Parameter の ID**: physics が Event に渡す Parameter は設定で差し替えられ（`SND_ID_NONE` = 渡さない、予約 ID は生成が `INVALID_ARGUMENT`）、既定は名前の `snd_hash_id`: `Phys/Impulse`・`Phys/Velocity`・`Phys/Count`（Impact / Break）、`Phys/Speed`・`Phys/Mass`（全接触）、`Phys/Motion`・`Phys/FootMaterial`・`Phys/SurfaceWeight`（足音） | 9.3 のベロシティレイヤーと 9.4 の個数を、Event 側のカーブ・レイヤーで受けられるようにする |
| D-121 | **足音（9.2）**: 重みが有限で正のものだけが候補で、大きい 2 つまで（同値は配列で先）。音量は「選んだものの重みの和で割った値」を重み→音量のカーブに通す（和と割り算は double。1 つだけなら 1 を通す）。Event は `footstep[材質][モーション]` → その材質のモーション共通 → 足音の全体の既定 → 何も鳴らさない（材質不明は全体の既定へ）。`foot` の Emitter で鳴らす（無効なら 2D）。モーション・足の材質（サウンドマテリアルの番号。不明なら渡さない）・正規化した重みを Parameter で、地面の表面パラメータと一緒に渡す。未知のモーションは `INVALID_ARGUMENT`。予算の対象外 | 9.2 |
| D-122 | **継続する接触（9.5）**: Slide / Roll。キーは（物体の組（順序なし）、材質の組（順序なし）、種別）。相対速度が下限（設定、既定 0）未満は閾値で捨てる。1 回の呼び出しの順は、(1) 追跡中のキーへの報告を項目に反映（同じ呼び出しの重複は最大速度。作業領域は使わない）、(2) 報告が無く、最後の報告からの経過がタイムアウト以上（ちょうどで止める）の項目を `snd_stop`（フェード時間つき）で止めて Emitter を返す、(3) 新しいキーを始める（Emitter を作って再生。新しいキーの候補は追跡表の容量ぶんの別の領域で、溢れは `dropped_work_overflow`）。表が満杯のときだけ、最後の報告がいちばん古いもの（同じなら始まりが古いもの）を追い出してフェードで止める。今回の呼び出しで報告された項目は追い出さず、空きを作れなければ新しい接触を落とす（`tracking_dropped`。Emitter が取れないときも同じ）。以後の報告では位置・速度の Parameter・音量・ピッチを更新する。再生が自分で終わっていたら、見つけた報告で同じ Emitter に始め直す。破棄は追跡中の再生を止め、Emitter をすべて返す。予算の対象外 | 9.5。タイムアウトを先に処理しないと、タイムアウトした項目が追い出しとして数えられる。衝突のバーストで、鳴っている擦れ音の報告が押し出されて途切れないようにする |
| D-123 | **戻り値と無効な入力**: `SubmitContacts` は入力を受け付ければ `SND_OK`（途中の C API の失敗は統計 `api_failures` と観測の記録の `result`）。断る入力（NULL、dt が負・非有限、`count > 0` で `contacts == NULL`、未知のモーション、`play_at` の Event が NONE・位置が非有限）は何もせず `INVALID_ARGUMENT`（`calls_rejected`）。接触ごとの不正（未知の種別、非有限の位置・衝撃量・速度、負の値）は捨てて `dropped_invalid`。観測コールバックの中から physics を呼ぶと `SND_ERR_UNSUPPORTED`（破棄は何もしない） | 1 件の不正で 1 フレーム分の接触を全部捨てない |
| D-124 | **統計と観測**: `snd_physics_stats`（累積、取得とリセット）は、間引きの各段で捨てた数（理由ごと）、まとめた数、予算で落とした数、追い出した数（追跡表・クールダウン表）、代替の段ごとの数、再生を要求した数（種類ごと）、継続接触の開始・更新・タイムアウト・やり直し、足音、API の失敗を持つ。勘定の恒等式はヘッダに書いた（受け取った接触 = 各理由で捨てた数 + まとめた数 + 組 + 擦れの報告、など）。観測コールバック（設定か `snd_physics_set_observer`）は、再生の要求・開始・更新・停止・代替・捨てた組のたびに、同じスレッドで 1 件ずつ `snd_physics_record` を渡す。9.3「代替が起きたことはツールで一覧できる」の土台で、ツールとライブリンクで運ぶ口は依頼 A-003 | 指示書 2.1 の統計。physics は公開 API だけの上位層で、ライブリンク（core に依存）には直接送れない |

変更履歴（統合のときに §7 へ移す）:

| 版 | 日付 | 内容 |
| --- | --- | --- |
| 1.2（段階 7-A physics） | 2026-11-20 | D-110〜D-124: physics の本実装。**契約ヘッダ・版・コマンド種別・スキーマ・バンク形式の変更なし**。新しい公開ヘッダ `physics/include/snd/physics/physics.h` / `physics.hpp`（`snd_physics_*`、`snd::phys`）、凍結の `snd_phys_*` 4 関数の本実装、`physics_stub.cpp` の削除。`api/compose/c_api.cpp` は変えていない。依頼 A-001（バンク形式 2.1 の PHYS 拡張）、A-002（ハンドルを返す `PlayAt`）、A-003（代替の一覧のツール表示）、A-004（Emitter を同じ更新で作成・破棄すると位置を失う core の欠陥、`snd_play_at` の暗黙の Emitter、Emitter の破棄の後に始まる子の再生） |
