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

（最後の節に記録する）

## 未検証の箇所

（最後の節に記録する）
