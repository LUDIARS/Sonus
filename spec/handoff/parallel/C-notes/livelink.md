# 組 L（livelink）のメモ — ライブ接続の残り・記録と再生

ブランチ `c/livelink`（ローカルコミットだけ）。仕様の正本はヘッダ（`contract/internal/include/snd/format/livelink_protocol.hpp`）と
`spec/interface/livelink-protocol.md`（1.1、§3・§4・§6・§7 の追記と §8）。

## 作ったもの

| ファイル | 役割 |
| --- | --- |
| `contract/internal/include/snd/format/livelink_protocol.hpp` | 1.1 の追加（下の「契約の変更点」） |
| `livelink/include/snd/livelink/struct_edit.hpp`（新） | 構造の変更の継ぎ目 `IStructEditor`、`StructEditRequest`、予備容量の費用 `StructCostOf` |
| `livelink/include/snd/livelink/wire.hpp`, `src/wire.cpp` | 1.1 の検査（`CheckStructArgs`、`CheckReloadBank`、`CheckCellAddress`、minor 付きの `CheckSetProfilerMask`）、印 `kFieldAuditionStop`、値の種類 `kValueTypeRevertCell` / `kValueTypeReadCell`、`IsMarkerField` |
| `livelink/include/snd/livelink/value_applier.hpp`, `src/value_applier.cpp` | `RevertCell`（項目単位の Revert）、`ReadCell`（書かずに読む）、`WriteCell`（再生の開始時の状態）、`CopyEdits`、`ReadBits`。`PendingChunk` に予備容量の費用、`CellReading`、`EditView` |
| `livelink/include/snd/livelink/runtime.hpp`, `src/runtime.cpp` | 試聴の追跡と全停止の受け渡し、ReadCell の答えの経路、構造の変更（予備容量の数え、編集器、適用の記録、切断時に新しい順で Undo）、StreamList と BlockDetail、記録の開始時の状態（Begin / Cell / Override）と差し替えデータ（LiveData） |
| `livelink/include/snd/livelink/server.hpp`, `src/server.cpp` | 1.1 の要求（RevertValue、ReadCell、QueryBanks（AwaitSync でも）、ReloadBank、試聴の全停止、StructEdit）、ホットリロードの状態機械、相手の minor、照合の端の規則、新しいプロファイラの流れ、統計 |
| `livelink/include/snd/livelink/client.hpp`, `src/client.cpp` | 1.1 の送信と答え、`PeerMinor` / `Supports`、決定を保留する照合（`deferSyncDecision`、`AwaitingSync`、`DecideSync`）、更新番号（`Sequences`、`Copy*Since`）、バンク一覧・ストリーム・BlockDetail・ReloadStatus の保持 |
| `livelink/include/snd/livelink/record_format.hpp`, `src/record_format.cpp` | 1.1 のレコードの読み取り（検査付き）、`IsKind` |
| `livelink/include/snd/livelink/replayer.hpp`, `src/replayer.cpp` | 差し替えデータの再生（ポインタを鍵に組み合わせて写しを指す）、構造の変更の再生（編集器）、開始時の状態（先行ブロック `leadBlocks`）、2.5 の条件（`conditions`、`BitExactExpected`）、決定のログ（`CopyDecisions`）、`OutputDigest` |
| `livelink/src/command_check.cpp` | 再生の規則: 新しい印と値の種類を通す |
| `livelink/test/livelink_v11_common.hpp` | テストの補助（Session、偽の編集器 `FakeEditor`、手で動かすゲーム側 `RawGame`、volumeDb だけの BUSS チャンク） |
| `livelink/test/livelink_v11_unit_test.cpp` | 契約の追加、検査、費用、記録の読み取り、再生の条件と壊れた開始時の状態 |
| `livelink/test/livelink_v11_live_test.cpp` | 本物の core で: minor の取り決め（両方向）、BuildMismatch と BankList と保留の決定、端の規則、Revert、ReadCell、ReplaceChunk（列が減る、戻し）、試聴の全停止と上限、構造の変更（UNSUPPORTED、予備容量、超過、外すと返る、切断時に新しい順で戻す）、ホットリロード（成功、失敗、無ければ読み込む、最中の NOT_READY、切断中も完了）、StreamList / BlockDetail / budget の定義、更新番号と取りこぼし |
| `livelink/test/livelink_v11_record_test.cpp` | 途中からの記録の開始時の状態と再生でのビット一致、作成時からの記録、差し替えの再生、2 回の再生で決定とハッシュが一致、構造の変更の再生、再生中の LOD の報告 |
| `livelink/test/livelink_v11_fuzz_test.cpp` | サーバに壊れた 1.1 のメッセージ、クライアントに壊れた 1.1 の返事、壊れた 1.1 の記録（読み取りと再生） |
| `livelink/test/livelink_v11_stress_test.cpp` | 並行性: UI スレッドの読み出しと要求の混在、要求の途中での再接続（TSan / Release 20 回の対象） |
| `livelink/test/CMakeLists.txt` | 上のテストの登録（`tests/integration` のフィクスチャを include。変更はしない） |
| `spec/interface/livelink-protocol.md` | 1.1 |
| `spec/handoff/parallel/requests/C-01〜C-09.md` | 依頼 |

`api/compose/c_api.cpp` には追記していない（新しい公開関数は要らなかった）。`tool/`、`buildtool/`、`tests/`、`core/` は変更していない。

## 決定の案（受け入れ側が D-番号を振る）

1. **minor の取り決め**: 双方が自分の minor を名乗り、送り手は相手の minor が `MinorOf(type)` 以上のときだけ送る。1.0 のゲームに
   1.1 のツールは 1.1 の要求を送らず（`Client` の送信関数は 0、`Supports`）、プロファイラのマスクを `ProfilerMaskOf(0)` に絞る。
   1.1 のゲームは 1.0 のツールを受け付け、1.1 の流れは 1.1 のマスクのビットか 1.1 の要求への返事だけ。
   `CheckSetProfilerMask(m)`（引数 1 つ）は 1.0 の規則のまま、サーバは `CheckSetProfilerMask(m, kProtocolMinor)` を使う
   （既存のテストの「bit7 は INVALID」を壊さないため）。
2. **照合の端の規則**: バンクが無い → 0、ツールも 0 なら Accepted。バンクどうしが食い違う → BuildMismatch、報告は最初に登録された
   バンクのハッシュ（ツールのハッシュがどれかと一致しても）。照合は接続のときだけ。`QueryBanks` は AwaitSync の間も答える。
   `Client` は `deferSyncDecision` で決定を保留（`ClientState` は足さず Handshake のまま。`tool/src/connection.cpp` の switch を壊さない）。
3. **RevertValue / ReadCell は `LiveSetValue` の `valueType` に印**（`'RVRT'` / `'READ'`）を入れて運ぶ（新しいコマンド種別は足せない）。
   ReadCell の答えは `CellReading` のリング（seq とセッションで組み合わせる）。古いセッションの ReadCell には答えない。
   読み取りは `LiveWriteCell(apply=false)` を 2 回（`ReadBits`）で、core を変えずに書かずに読む。
4. **試聴の全停止**: `LiveSetValue` の印 `kFieldAuditionStop`。オーディオスレッドが記録の走査（`LiveTap::arena`）で試聴のハンドルを覚え、
   印のところでそれまでの試聴をライブリンクのスレッドに渡し、ライブリンクのスレッドが `System::Stop`（`tuning.short_fade_ms`）を記録する。
   ゲームの再生は止めない。同時の試聴は `RuntimeDesc::maxAuditions`（32）まで、超えると LIMIT_REACHED。切断しても試聴は止めない。
5. **構造の変更**: `LiveReplaceChunk` に `kStructEditTag` を付けて運び、`PendingChunk` の枠を ReplaceChunk と共有する。
   引数の表（protocol §8.4）、費用 = bus 1 / effect 1 + bytes（64 + 8 × 引数）、remove は費用なし（ライブで足したものを外すと返す）。
   予備容量の超過は SND_ERR_OUT_OF_BUDGET（Error の文 "dev reserve exhausted: rebuild and hot reload"）、core の手段が無いのは
   SND_ERR_UNSUPPORTED。予備容量は `LiveRuntime` で数える（Server を作り直しても続く）。反映されなかった分はオーディオスレッドが返す。
   切断時の戻しは、そのセッション以前の構造の変更を新しい順に Undo。
6. **ホットリロード**: パスで読み直す（バイト列の転送は C-03 まで無し）。`UnloadBank(STOP_AND_UNLOAD)` と `LoadBankAsync` を同じ呼び出しで
   記録する。そのバンクの音は短いフェードで止まる。読み込みに失敗するとバンクは外れたまま。最中の編集（SetValue / ReplaceChunk /
   RevertValue / StructEdit）は NOT_READY。同時に 1 つ（2 つ目は LIMIT_REACHED）。切断しても完了する。上書き層は古い登録の覚えを捨てる。
7. **ReplaceChunk の受け付け表**（protocol §8.3）: 置き換えに無い列は今の値のまま、未知の列は読み飛ばす、行数が違う・参照の列が
   変わる・ブロブは UNSUPPORTED。
8. **budget の定義**: `budgetMicroseconds = 1e6 × block_size / sample_rate × load_target_percent / 100`（core の `block_budget_us` と同じ。
   テストで binary32 の一致を固定）。BlockTiming は区間でいちばん重いブロック、`lateChunks` だけ区間の合計（既存のとおり）。
9. **記録の新しいレコードは `RecordKind` の列挙子ではなく定数**（`kRecordLiveData` = 5 〜 `kRecordOverride` = 8）。
   `tool/src/recording_file.cpp` の `switch (item.kind)`（default なし）を `-Wswitch -Werror` で壊さないため。
   Begin は 16 バイト（1.0 の小さいレコードと同じ大きさ。既存のテストが 16 バイトで写す前提に合わせた）。
   最初のレコードは従来どおり Lod、Begin / Cell / Override は BankMap の後。
10. **開始時の状態の再生**: 先行ブロック 1 つ（`ReplayInfo::leadBlocks`）でセルを書き、オーバーライドを `LiveOverride` で当てる。
    記録の最初のブロックは Start から leadBlocks 後（Start を 1 ブロック早める）。先行ブロックが無いとバスの音量の変化が最初のブロックで
    ずれる（1 ブロックぶんの補間）ため。状態が無い記録では leadBlocks = 0（従来どおり）。
11. **作成時からの判定**: 記録の開始より前に、空でないアリーナが 1 つも適用されていなければ FromCreation。
12. **再生での LOD・ストール**: 再現しない（C-05）。記録にあるもの・再生中に起きたものを `conditions` で報告し、`BitExactExpected()` を false。
    途中からの記録（MidRun）は警告だけ（ビット一致を壊す条件には数えない）。
13. **決定のログ**: Replayer がタップから取れるもの（Play / LiveAudition / Stop の適用、終わり、イベントリング、ボイスの一覧の要約）を
    再生の相対ブロック付きで残す。範囲は記録の最初から最後のブロックまで。Clip と遷移は C-07。
14. **Client の更新番号**: 流れごとに受け取った数。履歴の流れは項目の番号で `Copy*Since` し、取りこぼしは `first - after - 1`。

### 差し戻し（受け入れ側の決定 D-170〜D-175）で決まったことと変えた点

| D | 採った案 | 差し戻しで変えた点（コミット） |
| --- | --- | --- |
| D-170 プロトコル 1.1 | 1・2・14 | Client の保持数を `client.hpp` に定数で書き（Ack 1024、CellValue 256、Error 256、ReloadStatus 64、Stall / LOD 1024、履歴は秒あたりの数）、押し出した数を `ClientStats::*Lost` に流れごとに出す（d8e5291）。同じ `LiveRuntime` に 2 つ目の Server は Start できない（SND_ERR_LIMIT_REACHED。SND_ERR_INVALID_STATE は無いので「空きが無い」の LIMIT_REACHED にした。先の Server を Stop / 破棄すれば Start できる）（6a4377e） |
| D-171 Revert・読み返し・全停止 | 3・4 | 文書だけ: 存在しない Event の試聴の Ack は記録の結果（OK）で枠は戻る（§8.6）、SetValue は全部か無しか（リリースバンクが同じオブジェクトを持つと UNSUPPORTED、§5）（201b601） |
| D-172 構造の変更と予備容量 | 5 | 戻しの記録の枠を受け付けで確保（`LiveRuntime::ReserveStructLog`）、入らない編集は LIMIT_REACHED で予備容量も編集器も使わない。枠は適用されなかった・打ち消された・戻した分をオーディオスレッドが返す。`RuntimeDrops::structLog` は「起きたら不具合」の数に。ライブで足したバスを外すと、その上のライブのエフェクトとそのバスから出るライブのセンドも一緒に記録から外して費用を全部返し、Undo を渡さない（6a4377e）。Remove は対応する Add の費用を全部返すと §8.4 に明記（201b601） |
| D-173 差し替えとホットリロード | 6・7 | ReloadBank で読んだバンクの bankId か言語が違えば、それを外して Failed（SND_ERR_INVALID_ARGUMENT、Error の文あり）。要求したバンクは外れたまま。外す前のヘッダの確認はしていない（ライブリンクのスレッドがゲームのファイルシステムを読むと IO スケジューラと IoToken を取り合うため。§8.5 に理由）（10d36af）。列 0 本の ReplaceChunk を表に（201b601） |
| D-174 プロファイラと budget | 8 | 変更なし |
| D-175 記録 1.1 と再生 | 9〜13 | 述語を 2 つに: `BitExactExpected()`（再生どうし、`kReplayStateLost` は外した）と `ReproducesRecordedRun()`（`kReplayBreaksRecordedRun` = それ ＋ StateLost ＋ NoBegin）。開始時に core で生きていた再生の数を `lostState` に足す（見分けられない core の状態は既定値と見なす、とヘッダに）。`Finished()` は最後の記録ブロックを描いた時点（ブロックの終わりのフック）（483e6e4） |

差し戻し 2 回目（D-172・D-173 への追記）:
- D-173: 失敗したリロードが外すのは、そのリロードが新しく作った登録だけ。`LoadBankAsync` が読み込みの前からあった登録（同じパスの
  ゲーム自身のバンク）を返したときは外さない（`Reload::preexisting`）。読み込み中で一覧にまだ無いゲームのバンクは見分けられない（未検証）。
- D-172: Remove は記録が一杯でも受け付ける。打ち消しなら枠なしで適用、バンクのものを外すならオーディオスレッドが適用時に枠を取り、
  取れなければ編集器を呼ばずに LIMIT_REACHED。枠を取るのは受け付けか適用のどちらか一方、返すのはオーディオスレッドだけ。

決定の番号を CONTRACTS.md の段階 7-C の節に書くのは、持ち場の外（`spec/interface/CONTRACTS.md`）なので、この組ではしていない。

## 契約の変更点（`contract/internal/include/snd/format/livelink_protocol.hpp` への追加だけ）

- `kProtocolMinor` 0 → 1（major は 1 のまま）。
- `MessageType`: RevertValue 0x0019、ReadCell 0x001A、QueryBanks 0x001B、ReloadBank 0x001C、CellValue 0x0108、BankList 0x0109、
  StreamList 0x010A、BlockDetail 0x010B、ReloadStatus 0x010C（末尾）。
- `ProfilerMask`: `kProfStreamList` = bit7、`kProfBlockDetail` = bit8（末尾）。
- 構造体: `ReloadBank`(16)、`CellValue`(32)、`BankEntry`(32)、`StreamInfo`(16)、`BlockDetail`(48)、`ReloadStatus`(24)、
  `RecordLiveData`(24)、`RecordBegin`(16)、`RecordCell`(40)、`RecordOverride`(24)。列挙 `ReloadFlags`、`CellFlags`、`StreamState`、
  `ReloadPhase`、`RecordOrigin`。定数 `kMaxReloadPathBytes`、`kRecordLiveData`〜`kRecordOverride`、`kStructEditTag`。
  関数 `MinorOf`、`ProfilerMaskOf`。`FixedPayloadBytes`、`IsKnown` に新しい種別。
- 既存の値・大きさ・`static_assert` は変えていない。`commands.hpp` は変えていない（新しいコマンド種別なし）。契約の版の数字も変えていない。

## 他の持ち場への依頼

| 番号 | 依頼先 | 要約 |
| --- | --- | --- |
| C-01 | core | 構造の変更（予備の行を有効にしてバスグラフ・エフェクトを組み直す）関数。livelink の `IStructEditor` の本物の実装に使う |
| C-02 | core | 試聴の再生ハンドルを記録の時点で返す（試聴ごとの停止） |
| C-03 | bank / core / api | 継ぎ目のないホットリロード（同じバンクを並べて登録して差し替え）、バイト列からの読み込み |
| C-04 | core | 記録の開始時の core の状態（鳴っている音、パラメータ、エミッタ、トランスポートなど）の書き出しと読み戻し |
| C-05 | core | 再生で LOD の段を固定する・ストールを起こす手段 |
| C-06 | core / source / jobs | ストリームの詳細（先読みの残り、状態）、ワーカーごとの時間 |
| C-07 | core / music | 決定のタップ（選ばれた Clip、遷移） |
| C-08 | 組 P（tool） | ツールが使う 1.1 の API の一覧 |
| C-09 | tests の持ち主 / 受け入れ側 | 1.1 と食い違う既存のテスト 3 件 |

## ライブでつないでいない箇所

- 構造の変更の本物の反映（C-01）。既定は SND_ERR_UNSUPPORTED。`RuntimeDesc::structEditor` を構成ルートで渡す配線も無い。
- 試聴ごとの停止（C-02）。
- 鳴ったままのホットリロード、バイト列の転送（C-03）。今はそのバンクの音を止めて読み直す。
- 途中からの記録の core の状態（C-04）。
- 再生での LOD・ストールの再現（C-05）。
- StreamList の先読みの残り・状態の詳細、CPU のワーカー別の内訳（C-06）。
- 決定のログの Clip・遷移（C-07）。
- ツールの画面（組 P、C-08）。
- `Config.dev_reserve.events`（イベントを足す op が無い）。

## Tela への提案

なし（組 L は Tela を使っていない）。

## Ergo Studio の sonus プラグインに足せるもの

UI に依存しない livelink の API はそのまま使える: `Client` の 1.1（照合の保留とバンク一覧、項目の Revert、読み返し、試聴の全停止、
ホットリロード、更新番号）、`Replayer` の決定のログと `OutputDigest`（不具合報告の記録を 2 回再生して比べる）、`RecordReader`。

## 既存のテストとの食い違い（直していない。C-09）

- `tests/integration/livelink_override_test.cpp:249`（全停止が UNSUPPORTED を期待）
- `tests/integration/indep_livelink_protocol_test.cpp:246`（minor 0 を期待）
- `tests/integration/indep_livelink_profile_record_test.cpp:380-381`（再生が LiveReplaceChunk を外す前提）

## 回した構成と結果

すべて -j2、ビルドは $S/livelink/b-*（終わったら消した）。「既知の 3 件」= C-09 の 3 件
（`integration_livelink_override_test`、`integration_indep_livelink_protocol_test`、`integration_indep_livelink_profile_record_test`。
1.1 の決めた振る舞いとアサーションが食い違う。直していない）。それ以外の失敗は無し、警告 0。

| 構成 | コミット | ctest | 結果 |
| --- | --- | --- | --- |
| 1+8 gcc Debug + `SND_BUILD_TOOL_CORE=ON` | 7cc7d14 | 全部（221） | 既知の 3 件だけ失敗（platform_device_play_test は Skipped） |
| 2 clang Debug | 7cc7d14 | `livelink\|api\|tool`（54） | 既知の 3 件だけ失敗 |
| 3 ASan/UBSan | 7cc7d14 | `livelink\|api_`（49） | 既知の 3 件だけ失敗。ファズ（livelink_v11_fuzz_test）は SND_FUZZ_ITERATIONS=60000 でも通過（91378fe） |
| 4 TSan | 7cc7d14 | `livelink`（26） | 既知の 3 件だけ失敗。並行性（livelink_v11_stress / livelink_v11_live / integration_livelink_stress）を -j1 で `--repeat until-fail:20`: 全通過 |
| 5 Release | 8871931（コードは 7cc7d14 と同じ） | `livelink\|api_`（49） | 既知の 3 件だけ失敗。並行性 3 本を `--repeat until-fail:20`: 全通過 |
| 6 テストなし Release | 8871931 | — | ビルド成功、警告 0 |
| 7 Opus なし | 8871931 | `livelink`（26） | 既知の 3 件だけ失敗 |
| 9 ツール入り（Tela） | 7cc7d14 | `livelink\|tool`（50） | 既知の 3 件だけ失敗。既存の tool/test は全通過 |
| MinGW 構文検査 | 7cc7d14 | — | livelink/src の全 .cpp と新しいテスト 5 本、変えたヘッダ、`livelink_protocol.hpp` を `-Werror -Wall -Wextra -Wpedantic -Wshadow …` で通過（MinGW は apt で入れた。テストの std::thread を含めて通った） |

途中で見つけて直したもの: UBSan（テストの空ベクタの memcpy、91378fe）、Release の `-Wstringop-overflow` の誤検知（テストの補助、d51b8df）、
Release で 7/20 落ちたホットリロードのテスト（ReloadStatus Done を新しい登録の公開前に送っていた → サーバを直した。テスト側の再接続の
競合も直した、7cc7d14）、再生中の LOD のテストが機械の速さに依存（単体テストで固定、7cc7d14）。
gcc Debug の全件を d51b8df で回したとき `api_indep_runtime_test`（IndepRuntime.EndsFromConcurrentProducersAreExactlyOnce）が 1 回失敗し、
単独で 10 回回すと全通過。livelink を使わないテストで、7cc7d14 の全件では通過（負荷による揺れと見ている。未解明）。

### 差し戻しの後（6d3c2cd）

| 構成 | ctest | 結果 |
| --- | --- | --- |
| 1+8 gcc Debug + TOOL_CORE | 全件（227） | 失敗は決定と食い違う独立テスト 2 件だけ（`IndepS7Replay.LostStateIsReported`、`IndepS7Impl.Defect_RemoveOfLiveBusAddedWithFullStructLogKeepsReserve`）。ほかの indep_s7（live 31、record 8、fuzz 7、stress 2）と既存の全テストは通過 |
| 3 ASan/UBSan | `livelink\|api_`（55） | 同じ 2 件だけ失敗 |
| 4 TSan | `livelink`（32）、並行性 4 本（v11_stress、v11_live、indep_s7_stress、integration_livelink_stress）を -j1 で 20 回 | 同じ 2 件だけ失敗。20 回: 全通過 |
| 5 Release | `livelink\|api_`（55）、同じ 4 本を 20 回 | 同じ 2 件だけ失敗。20 回: 全通過 |
| clang Debug | `livelink\|tool`（37） | 同じ 2 件だけ失敗 |
| MinGW 構文検査 | — | 変えた runtime.cpp / server.cpp / client.cpp / replayer.cpp、v11 のテスト 5 本、変えたヘッダ 6 本: 通過 |

途中の gcc Debug の `ctest -R 'livelink|tool'`（483e6e4）で `tool_live_mixer_test`（StateSurvivesDisconnectAndReconnect: 切断後 16 ブロックで戻しが
まだ届いていない）が 1 回失敗し、単独で 10 回回すと全通過、6d3c2cd の全件でも通過（負荷による時間の揺れと見ている。未解明）。
回していない構成（差し戻しの後）: 6（テストなし Release）、7（Opus なし）、9（ツール入り）。

### 差し戻し 2 回目の後（d399b0b）

直した内容: D-173（失敗したリロードはゲーム自身の登録を外さない）と D-172（記録が一杯でも Remove は受け付ける）。上の「差し戻し 2 回目」の節。
`IndepS7Recheck.Defect_FailedReloadUnloadsTheGamesOwnOtherBank` は通過。

決定（D-172 の追記）と食い違って落ちる独立テスト（変えていない）:
- `IndepS7Impl.StructEditBeyondTheLogIsRefusedAtAcceptance`（indep_s7_impl_test.cpp:79-80: 記録が一杯でライブのバス B1 の RemoveBus を LIMIT_REACHED と期待。今は打ち消しとして OK）
- `IndepS7Recheck.StructLogAndReserveFollowTheModel`（模型が「Remove も受け付けで枠を取る」前提: indep_s7_recheck_test.cpp の RemoveBus live の分岐）

| 構成 | ctest | 結果 |
| --- | --- | --- |
| 1+8 gcc Debug + TOOL_CORE | 全件（228） | 失敗は上の 2 件だけ。indep_s7 のほか（unit 15、live 31、record 8、fuzz 7、stress 2、recheck の残り 9、impl の残り 8）と既存の全テストは通過 |
| MinGW 構文検査 | runtime.cpp、server.cpp、livelink_v11_live_test.cpp、struct_edit.hpp、runtime.hpp | 通過 |
| 3 ASan/UBSan | `livelink\|api_`（56） | 上の 2 件だけ失敗 |
| 4 TSan | `livelink`（33）、並行性 4 本（v11_stress、v11_live、indep_s7_stress、integration_livelink_stress）を -j1 で 20 回 | 上の 2 件だけ失敗。20 回: 全通過 |
| 5 Release | `livelink\|api_`（56）、同じ 4 本を 20 回 | 上の 2 件だけ失敗。20 回: 全通過 |
| clang Debug | `livelink\|tool`（38） | 上の 2 件だけ失敗 |
| 6 テストなし Release | — | ビルド成功 |
| 7 Opus なし | `livelink`（33） | 上の 2 件だけ失敗 |
| 9 ツール入り（Tela） | `livelink\|tool`（57） | 上の 2 件だけ失敗。tool/test は全通過 |

警告はどの構成も 0。recheck の繰り返し（ConcurrentStartsNeverOverlap など）を `--filter` で 20 回ずつ回すことはしていない（ctest の 20 回の対象は上の 4 本）。

## 未検証の箇所

- 構造の変更が core に実際に反映されること（C-01 まで偽の編集器でしか確かめていない）。
- ストリームを読む再生での StreamList（Active、ストールの数）。テストの資産は常駐なので Idle だけを見た。ストールの数は core の stall タップ経由で数えるが、本物のストールでは確かめていない。
- TCP での 1.1（テストはメモリの転送路だけ。1.0 の TCP テストは通っている）。
- 試聴の全停止で、試聴の上限（maxAuditions）を超えて追跡できなかった試聴（`RuntimeDrops::auditions`）が出る場合（予約で防ぐので起きない想定）。
- ホットリロードで、依存元があって外せないバンク（BANK_IN_USE）と、言語バンク（languageId != 0）。
- 途中からの記録でセルが記録用リングに入りきらない場合（lostState）、LiveData が入りきらない場合（大きなチャンク × 記録中）。
- 1.1 の Client と 1.0 の本物のゲーム（テストは手で書いた 1.0 のゲーム側で確かめた）。
- Windows / macOS での実行（MinGW は構文検査だけ）。
