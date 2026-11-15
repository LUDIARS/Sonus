# 契約（段階0「契約の凍結」）

設計書 `spec/architecture/design.md`（版 0.2）の 12.2 で凍結すると決めた 7 項目を、このリポジトリのどのファイルが表すか、
どう変えるか、段階0で何を決めたかをまとめる。段階1以降の並列実装はこの文書と下記のファイルだけを入力にする。

- 契約の版: **1.0**（`SND_CONTRACT_VERSION_MAJOR = 1`, `SND_CONTRACT_VERSION_MINOR = 0`、`contract/public/include/snd/version.h`）
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
| 5 | バンク | `contract/internal/include/snd/format/bank_format.hpp`, `spec/interface/bank-format.md` |
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
| D-02 | モジュール依存は 12.1 の表をルート `CMakeLists.txt` に 1 か所だけ書き、`snd_add_module()` が読む。`base` と `platform` は PUBLIC（推移的に見える）、他は PRIVATE。よってモジュールの公開ヘッダに出せる型は契約・base・platform のものだけ。buildtool の依存は codec と契約のバンク形式定義（bank のランタイムには依存しない） | 依存方向をビルドで強制する。12.1 の「bankの形式定義」は契約側にあるため |
| D-03 | 構成ルート: 全モジュールを束ねるライブラリ `sonus` を `api/compose/` に置く。`sonus_api` 自身は 12.1 どおり core と music だけに依存し、source のソース種別・music の `IMusicSystem`・io の既定ファイルシステム・platform の既定デバイス・livelink は構成ルートが契約の IF 越しに渡す | 12.1 では api が source / io / platform / livelink に依存しないのに、`Create` はそれらを必要とする（G-03） |
| D-04 | ID ハッシュは 32bit FNV-1a（オフセット 2166136261、素数 16777619）を UTF-8 のバイト列にそのまま掛ける。大文字小文字・区切り文字は正規化しない。結果が 0 なら 1。0 は「ID なし」 | 単純で constexpr にでき、buildtool と一致させやすい。衝突はプロジェクト全体でビルドエラーにする（10.1） |

### 公開 API

| # | 決定 | 理由 |
| --- | --- | --- |
| D-05 | ハンドルは 64bit = 世代（上位 32）＋スロット（下位 32）。世代 0 は発行しないので値 0 が無効ハンドル。Bank と UserBuffer も同じ形。音楽 Playback が所有するトランスポートのハンドルは `slot = 0x80000000 \| playbackSlot, generation = playback の世代` で往復なしに導ける（`snd_get_playback_transport`） | 3.1。13.2 の例の `musicTransport` をゲームが得る手段が無かった（G-12） |
| D-06 | C API が ABI の正本。C++ API はヘッダだけの薄いラッパーで、POD は C 構造体の別名か同じレイアウトの型、列挙は C の値を持つ `enum class`。拡張点（12.3 の 6 IF）は C の関数表（`snd/ext.h`）と C++ の抽象クラス（`snd/ext/*.hpp`）の両方を持ち、相互の橋渡し（`snd::ToC`, `snd::C*` アダプタ）を契約に含める。`snd_create` は関数表を値でコピーする | 13.1「C APIはPODとハンドルだけ、C++はその薄いラッパー」。C++ 利用時の二重間接は許容（api が後で特別扱いしてよい） |
| D-07 | `Config` のスカラー項目は X マクロ（`snd/config.h`）1 か所で定義し、C 構造体（snake_case）、`snd_config_init`、C++ の `snd::Config`（camelCase、既定値付き）、C++→C 変換を生成する。先頭に `struct_size` と `contract_version`。音量は線形ゲイン | C と C++ で項目がずれない。本文中の上限と「仮」の値をすべて項目にした（下表） |
| D-08 | オーディオスレッドと RT ワーカーは `IJobSystem::Submit` も `IFileSystem` も呼ばない。有界のロックフリーキュー（`SourceServices::postJob` / `postIo`）に積み、非 RT スレッドが受けて投入する | 12.3 の「Submit はどのスレッドから呼ばれてもよい」をホストの実装に任せると原則 1 を破りうる（G-15） |
| D-13 | `Update()` が公開するのは所有スレッドのアリーナだけ。他の登録スレッドは `snd_flush()`（追加）でフレームごとに公開し、`snd_unregister_thread()`（追加）で抜ける | 他スレッドのアリーナを所有スレッドが公開すると書き込み中と競合する |
| D-28 | 13.1 に無い関数を足した: `snd_flush`, `snd_unregister_thread`, `snd_contract_version`, `snd_config_init`, `snd_get_bank_chunk`, `snd_get_playback_transport`, `snd_destroy_transport`, `snd_set_transport_state`, `snd_get_transport_position`, `snd_destroy_user_buffer`, `snd_get_user_buffer_released`, `snd_play_user_buffer`, `SetSwitch` の 3 スコープ、physics の `create/destroy/desc_init`。`Resume` は C で `snd_resume`（システム）と `snd_resume_playback` に分ける。`PlayAt` と `PlayOneShot` はハンドルを返さず `snd_result` を返す | G-05, G-09, G-10 |
| D-29 | 無効ハンドル: 記録時に古いと判れば（値 0、またはゲーム側スロット表と世代不一致）何も記録せず `SND_ERR_INVALID_HANDLE`。記録後に古くなったものは適用時に世代不一致で無視。どちらも「何もしない」（13） | 呼び出し側が分岐しなくてよく、デバッグ時には理由が分かる |
| D-30 | カテゴリ ID `SND_ID_NONE` は「全カテゴリ」（全体のポーズ・音量、7 章）。ポーズ無視カテゴリは除外 | 全体ポーズ用の関数を増やさない |
| D-31 | プラグインの登録は `Config.plugins`（コーデックは fourcc、エフェクトは種別 ID）。同じ fourcc のコーデックは組み込みより優先 | 13.1 に `ICodec` / `IDspEffect` を登録する手段が無かった |
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
| D-11 | 予約領域も満杯のときは、アリーナ付属の `OverflowFallback` に畳み込む: 停止と各種破棄・アンロードはスロットごとのビット、Parameter とスイッチは (スコープ, 宛先, ID) ごとの最終値表（`overflowParamSlots`）。表から溢れた値だけが失われ、`stats.reserve_overflows` に数える | 「必ず届ける」を量に関係なく守る |
| D-12 | 適用ブロック番号はアリーナ単位。オーディオスレッドが取り込んだブロックの番号を `ArenaHeader::appliedBlock` に書く。リプレイは (producer, appliedBlock, アリーナの中身) を記録する。生産者の取り込み順は登録順 | 2.5, 11.3 |
| D-16 | 通知: 終了は Playback スロットごとの記録＋ビット配列（取りこぼしなし）。Emitter / Transport / UserBuffer のスロット返却も同じ方式のビット配列。バンクの状態はスロットごとのアトミック。拍・マーカー・ログはイベントリング（48 バイトの記録、溢れたら捨てて数える）。問い合わせ用のトリプルバッファは各バッファにシーケンスロックを持ち、任意のスレッドから読める | 2.1。「任意のスレッド」から読むと `Update()` のスワップと競合する（G-08） |
| D-15 | 「短いフェード」（奪われたボイス、ストール、締切超過、アンロード）は一律 `tuning.shortFadeMs`（既定 5 ms） | 設計書に値が無い |

### テーブルと所有者

| # | 決定 | 理由 |
| --- | --- | --- |
| D-17 | 3.1 に無いテーブル・列を足した: Category、Snapshot のテーブル。ParamValue はスコープ別の 3 表（Global / Emitter / Playback）にし、スイッチは `kind` 列で同じ表に置く。Playback に `request`, `logic`, `endInfo`, `dynamicSource`、Voice に `emitterSlot`, `limitGroup`, `lifecycle`, `filterTarget`, `renderStatus`, `clipRef`、RealVoice に `assign`, `appliedGain`、Emitter に `renderPosition`、Bus に `volume`, `gainTarget`, `effectBypass`, `graphOrder`、Transport に `ownerPlayback`, `lifetime`, `signature`, `loopRange`, `initial`、Reservation に `when`, `acceptedBlock`, `resolved`, `sequence`、Stream に `control`, `ringWrite`, `ringRead`, `stallCount` | 3.2 の読み書きを 1 列 1 書き手で表すのに必要（G-02） |
| D-18 | 所有者の決定: `Voice.audibility` と `Bus.gainTarget` は EvaluateDspParams。`Bus.gain`・`Bus.peak`（ダッキング込みの実際の値）と `RealVoice.panMatrix`（Spatial から計算）は Render。Transport の状態列はすべて UpdateTransport（即時の `SetTempo` / シーク / 状態変更も「今」で予約に入れる）。予約の要求時刻（ApplyCommands）と解決後の時刻 `Reservation.resolved`（UpdateTransport）を分ける。Emitter の遮蔽値は EvaluateLogicParams（Parameter のランプを通す）。行の生成・破棄: Playback は ApplyCommands が作り RunEventLogic が消す、Voice と Stream は RunEventLogic（ソースの `Start` は RunEventLogic の中で動く）、RealVoice は SelectVoices、Transport と Reservation は ApplyCommands が作り UpdateTransport が消す、ParamPlayback は Playback と一緒に RunEventLogic が消す。仮想ボイスの位置送りは Render が行う | 3.2 の「1 列 1 システム」を守る |
| D-19 | `eventId` はコールド列、ホット列には解決済みの `eventRef`（バンクスロット＋行）を置く | 3.1 の表（ホット）と本文（コールド）が矛盾（G-01） |
| D-35 | 所有者の食い違いはコンパイル時に検出する: 書き込みは `WriteColumn<System, Column>`、読み出しは `ReadColumn<System, Column>` を通し、所有者でない書き込み・宣言のない読み出しは `static_assert` で失敗する。読み出しの時点（同じブロック / 前ブロック / 非同期）も `ValidateReads()` が検査する。`contract_schema_violation_rejected` などのテストがコンパイル失敗を確認する | 12.2-3「所有者の食い違いを検出できる仕組み」 |
| — | オーディオスレッド以外の書き手（`ExternalIo`, `ExternalDecode`）を持てるのは `std::atomic` の列だけ（`Stream.readPos`, `Stream.ringWrite`）。他のシステムからは `Async` 読み出しとして宣言する | 3.2 に Stream の書き手が無い |

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

### 形式・テスト

| # | 決定 | 理由 |
| --- | --- | --- |
| D-32 | バンク: 128 バイトのヘッダ、CRC-32C（ヘッダ自身にも別のチェックサム）、チャンク目次、テーブルは列ディレクトリ付きの SoA、列 ID はテーブルごとに 0 からの連番、未知のチャンク・列は無視、任意列は既定値、チャンクごとの版。ストリームのパスは除去不可の `SPTH`、物理マテリアル表はアプリケーションチャンク `PHYS`、依存は (bankId, contentHash) | 10.2。列単位で足せるので並列実装で minor 版を上げやすい（G-11） |
| D-33 | 版は 3 系統（契約、バンク形式、ライブリンク）を独立に持ち、いずれも major（互換なし）/ minor（追加のみ） | 2 節 |
| D-34 | ライブリンク: 16 バイトのフレームヘッダ、Hello / HelloReply / SyncDecision の照合、値の変更はバンクのテーブル列（チャンク fourcc ＋ 列 ID）で指定、リストは要素サイズ付き | 11.2。項目の番号を別に定義しなくて済む |
| D-36 | RT 安全性の印（`SND_RT_SCOPE`）と記録（`SND_RT_NOTE`）は契約のヘッダだけで完結し、`SND_RT_CHECKS=0` で消える。既定はテストをビルドするとき ON。スレッドローカルの深さは `volatile`（最適化で消されないため）。テスト実行ファイルでは `operator new/delete` を常に、malloc 系・pthread のロック・`fopen/open/read/write` をサニタイザ無しの glibc で差し替えて検出する。ライブラリ側はロックのラッパー、`IFileSystem` 実装、書式付きログ、`IAllocator` 実装に `SND_RT_NOTE` を置くこと | 14 章。サニタイザのランタイムも同じ関数を差し替えるので併用しない |
| D-37 | ゴールデン音声は `.sndg`（32 バイトのヘッダ＋チャンネル順の float32）。置き場所はテスト対象モジュールの `test/golden/`。更新は `--update-golden` か `SND_UPDATE_GOLDEN=1`。許容差は最大絶対誤差・RMS・超過サンプル数 | 14 章 |
| D-38 | モックは `snd::mock`。決定論ジョブは優先度を無視した投入順（FIFO）で、ジョブ内からの投入も投入順に実行。メモリ FS の遅延は「Poll の回数」で数える | 再現性 |

### `Config` の項目（13.2 と本文の上限・「仮」の値）

| 項目 | 既定 | 出典 |
| --- | --- | --- |
| `sampleRate`, `blockSize`, `renderAheadBlocks` | 48000, 256, 2 | 1.1, 1.3, 2.2 |
| `maxRealVoices`, `maxVirtualVoices`, `fadeReserveVoices` | 128, 1024, 8（仮） | 13.2, 4.4 |
| `maxPlaybacks`, `maxEmitters`, `maxListeners`, `maxTransports`, `maxReservations` | 1024, 512, 4, 8, 256 | 3.1 |
| `maxStreams`, `maxBanks`, `maxUserBuffers`, `maxGlobalParams`, `paramOverflowBlocks` | 32, 64, 32, 256, 256 | 3.1, 6.3（段階0で決定） |
| `eventRingCapacity`, `maxRegisteredThreads`, `listenerMode`, `focusMode`, `seed` | 1024, 8, Nearest, Continue | 2.1, 4.5, 2.6, 2.5 |
| `memory.streamPoolBytes`, `streamChunkBytes`, `pcmCacheBytes`, `pcmCachePageBytes` | 8 MiB, 32 KiB, 16 MiB, 64 KiB（仮） | 13.2, 6.2 |
| `memory.frameArenaBytes`, `threadArenaBytes`, `maxDecoders`, `decoderSlotBytes`, `voiceRingFrames`, `dspPoolSpareBytes` | 256 KiB, 256 KiB, 64, 32 KiB, 4096, 256 KiB | 3.3（段階0で決定） |
| `commands.arenaBytes`, `arenasPerThread`, `reserveBytes`, `criticalPriority`, `overflowParamSlots` | 64 KiB, 3, 8 KiB, 224, 256 | 2.1, D-10, D-11 |
| `jobs.realtimeWorkers`, `normalWorkers`, `voicesPerChunk`, `external` | 2, 2, 16, なし | 13.2, 2.3 |
| `tuning.rtSpinMicroseconds`, `loadTargetPercent`, `lodDownBlocks`, `lodUpBlocks` | 50, 60, 8, 64（仮） | 2.3, 2.4 |
| `tuning.emitterExtrapolateMs`, `underrunCatchupBlocks`, `callbackAheadBlocks` | 100, 2, 4（仮） | 2.1, 2.2, 5.2 |
| `tuning.streamPreloadMs`, `streamTargetAheadMs`, `ioBytesPer100ms`, `stealHysteresisDb`, `shortFadeMs` | 100, 200, 4 MiB, 3.0, 5.0（仮） | 6.1, 4.4, D-15 |
| `devReserve.buses`, `effects`, `events`, `bytes` | 16, 32, 64, 1 MiB | 11.2 |
| `liveLink.enabled`, `port`, `meterHz`, `sendBufferBytes`, `recordCommands` | 0, 27460, 30, 256 KiB, 0 | 11.2, 11.3 |
| `output.device`, `output.deviceIndex`, `plugins.*` | 既定デバイス, 0, なし | 2.6, D-31 |

## 4. モックとテスト枠組みの約束

- テスト実行ファイルは `snd_add_test(<name> SOURCES ... LINK ...)` で作る。1 実行ファイル = 1 CTest。
- 各テストの終わりに、`ExpectViolations` の外で記録された RT 違反があればそのテストを失敗にする。`SND_RT_ABORT=1` で最初の違反で abort。
- モックの差し替え点: C++ API では `Config.output.device = &nullDevice`, `Config.jobs.external = &jobs`, `Create(cfg, alloc, &memFs, &sys)`。
  C API では `snd::ToC(...)` で関数表にして渡す。
- 枠組みとモックの API（ヘッダ）を変えるときは 2 節の手順に従う。実装の修正と機能の追加は tests 担当の判断でよい。

## 5. 設計書の矛盾・不足（段階0で見つけたもの）

設計書は書き換えていない。以下は設計書側で解消すべき点と、契約での暫定の扱い。

| # | 箇所 | 内容 | 契約での扱い |
| --- | --- | --- | --- |
| G-01 | 3.1 | Playback の `eventId` を表ではホット列、本文ではコールド列としている | D-19 |
| G-02 | 3.1, 3.2 | Stream の列の書き手（IO スレッドとデコードジョブ）、バスゲイン（スナップショット・ダッキング・カテゴリ）、`Voice.audibility`、`RealVoice.panMatrix`、Emitter の補間状態、カテゴリとスナップショットの状態の置き場所が表に無い | D-17, D-18 |
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
