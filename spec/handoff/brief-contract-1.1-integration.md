# 依頼文: 契約 1.1 への改訂とモジュール間の統合

実装担当（Opus）のエージェント 1 体に渡す依頼文。`{WORKTREE}` は作業場所に置き換える。
前の担当が途中まで進めた契約ヘッダの編集が `spec/handoff/contract-1.1-wip.patch` にある（下の「途中までの作業」）。

---

あなたは C++ サウンドライブラリ「Sonus」の **契約 1.1 への改訂と、段階1で作ったモジュール間の統合** を 1 人で担当します。作業場所は `{WORKTREE}`（段階0の契約と、段階1の base / platform / jobs、codec、dsp、io、bank / buildtool がすべてマージ済みで、全構成で 62/62 テスト通過の状態）です。

## 途中までの作業

前の担当が第1部の契約ヘッダを途中まで編集しています（14 ファイル、未検証・未ビルド）。`git apply spec/handoff/contract-1.1-wip.patch` で当てられます。当てたうえで、下の決定 1〜12 と 1 項目ずつ照合し、合っていない箇所・足りない箇所を直してください。使わずに最初からやり直しても構いません。どちらにしたかを報告してください。

## 最初に読むもの

1. `README.md`、`spec/interface/CONTRACTS.md`（全部。特に §2 変更手順、§3 決定、§5、§7）、`spec/interface/acceptance/stage0.md` と `stage1.md`（持ち越し A1-1〜A1-19）
2. `spec/architecture/design.md` の 2.3、2.5、3.1〜3.4、6章、10章、12章、14章
3. 各モジュールの公開ヘッダの冒頭コメントと README: `base/include/snd/base/`、`platform/include/snd/platform/`、`jobs/include/snd/jobs/`（特に `rt_workers.hpp` の SAFE POINT）、`codec/README.md` と `codec/include/snd/codec/`、`dsp/include/snd/dsp/`（`resampler.hpp`、`effects.hpp`）、`io/include/snd/io/`（`io_scheduler.hpp` の不変条件と `FileRequest`）、`bank/include/snd/bank/`（`bank_manager.hpp`、`prepared.hpp`）、`buildtool/README.md` と `buildtool/include/snd/buildtool/`（`plugins.hpp` の差し込み口）
4. 契約ヘッダ: `contract/public/include/snd/`（`ext.h`、`types.h`、`config.h`、`sonus.h`、`sonus.hpp`、`version.h`）、`contract/internal/include/snd/internal/`（`schema.hpp`、`common.hpp`、`notify.hpp`、`source_if.hpp`）、`contract/internal/include/snd/format/bank_format.hpp`、`spec/interface/bank-format.md`

## 第1部: 契約 1.1（受け入れ側で決定済み。そのまま反映する）

契約ファイルの編集をこの作業に限って許可します。`SND_CONTRACT_VERSION_MINOR` を 1 に上げ、`CONTRACTS.md` の冒頭の版・§3（新しい決定 D-43 以降を理由つきで追記）・§5（解消した G-xx があれば注記）・§7（変更履歴）を更新してください。契約の自己検査（`contract/test/`）も追従させます。

1. **遅れたチャンクとメモリの再利用（D-43、A1-1）**: 設計書 2.3 の「締切に遅れたチャンクは出力に加えず、遅れて届いた結果は世代で捨てる」に対し、遅れたワーカーは `Run` が返った後も、次のブロックに入っても、そのチャンクの入力を読み出力を書く。規則を次のとおり定め、`CONTRACTS.md` と `source_if.hpp`（ソースのバッチ描画が守るべきこと）に書く。
   - チャンクが触ってよいメモリは、そのチャンクのレンダー項目の配列、そのチャンクのバス別アキュムレータ、実行スレッドのスレッド別アリーナ、そのチャンクに属するボイスの描画状態（RealVoice の列、リサンプラ・フィルタ・ソースのボイス別状態、playhead）だけ。
   - レンダーリストとチャンク別アキュムレータは 2 組を持ち、ブロックごとに交互に使う。前の組を再利用できるのは `RtWorkerPool::StragglersInFlight() == 0` を確認したときだけ。確認できないブロックでは並列描画をせず、呼び出し側（オーディオスレッド）だけで、使用中でない組に描く。2 組とも使用中になる状況（遅れが 2 ブロック続けて残る）では、新しい遅れを作らないようこの直列描画を続ける。
   - 遅れたチャンクに属していたボイスは「保留」とし、そのワーカーが静止する（`StragglersInFlight() == 0`）まで、どのシステムもそのボイスの描画状態を読み書きせず、レンダーリストに入れない。保留中の出力は前ブロック末尾から短いフェードで無音にする（2.3）。保留が解けたら、描画状態は「遅れたチャンクが書き終えた値」として扱い、次のブロックから通常どおり鳴らす。
   - スキーマに、保留を表す列（Render が書き、SelectVoices / BuildRenderList が前ブロック値として読む）を足す。
2. **リサンプラの状態の置き場所（D-44、A1-2）**: `RealVoice.resamplerState`（64 バイトの列）を廃止し、core が持つ固定スロットのプールに置く。スロット数は実ボイス上限＋フェード予備、スロットの大きさは `dsp::ResamplerStateBytes(Sinc, Config の最大ボイスチャンネル数, Config の最大ピッチ比)`。プールの添字は RealVoice のスロット番号そのもの（列は不要）。`Config` に `maxVoiceChannels`（既定 8）と `maxPitchRatio`（既定 4.0、上限 32.0）を足す（X マクロ、C と C++、CONTRACTS の Config 表）。ピッチ比はこの上限で頭打ちにする、と `source_if.hpp` / スキーマのコメントに書く。メモリ区分（`MemTag`）が要るなら足す。
3. **Stream テーブルと外部の書き手（D-45、A1-3）**: Stream テーブルを Direct（スロット番号 = 行の位置。行が動かない）にする。オーディオスレッド以外の書き手（`ExternalIo`、`ExternalDecode`）を持つアトミック列は、スロット番号で直接添字する、とスキーマとコメントに明記する。Direct 表の行はハンドルで参照しない（base のヘッダの注意と一致させる）。io の `StreamChannel` が持つスロット単位のアトミック（`ReadPos()` など）との対応付けは段階2の Stream ソースの担当なので、契約には「IO 側の公開値を source が Stream の列へ写す、または列の実体として io のチャンネルの値を参照する」のどちらかを source が選べる、と書くに留める。
4. **`snd_effect_desc.sample_rate`（A1-4）**: 構造体の末尾に `uint32_t sample_rate`（＋必要なパディング）を足し、プラグインの `state_bytes` がレートを知れるようにする。C++ 側の別名・橋渡し、dsp の `EffectDesc` を使う箇所（`EffectStateBytes(desc, sampleRate)` の引数のレートとの関係。引数を残すか desc に寄せるかを決めて統一）、bank / buildtool の該当箇所を追従。
5. **依存表（A1-5）**: ルート `CMakeLists.txt` の `SND_MODULE_DEPS_buildtool` に dsp を加える（buildtool が `dsp::ComputeDspPoolNeed` を呼ぶため）。`CONTRACTS.md` D-02 に追記。
6. **bank-format.md への追記（A1-6）**: 標準エフェクトの種別 ID の表（`dsp/include/snd/dsp/effects.hpp` の `kEffect*` と一致。パラメータ表は effects.hpp を参照と書く）と、組み込みコーデックのバイト配置の節（PCM f32 / s16、IMA ADPCM。`codec/README.md` の内容を正として移す。ADPCM のブロックは 1 チャンネルあたり 1024 バイト以下という制約も）。バンク形式の版は 2.0 のまま（文書の追記のみ）。
7. **公開 API の追加（A1-7）**: `snd_result` に `SND_ERR_BANK_ALREADY_LOADED` を末尾追加（bank の二重読み込みの失敗はこれを返すよう bank を直す）。言語名をランタイムに伝える関数を足す: `snd_result snd_declare_language(snd_system*, const char* name)`（登録スレッド。`snd_set_language` に渡す ID は `snd_hash_id(name)`。言語別バンクのファイル名 `<base>.<name>.sndb` の解決に使う。未宣言の ID を `snd_set_language` に渡したら `SND_ERR_NOT_FOUND`）。C++ ラッパー、`api/src/c_api_stub.cpp`、コマンド（必達）、`CONTRACTS.md` D-28 の追加関数一覧を追従。
8. **`ext.h` の `snd_codec.decode` のコメント（A1-8）**: `src == NULL` の吐き出しは終端を意味しない（終端は `total_frames` で決まる）、状態が保持できる未消費データには上限があり（組み込みは 16 KiB ＋ 1 パケット）、超えると `SND_DECODE_ERROR`、呼び出し側は `max_frames` が返ったら未満が返るまで吐き出してから次を渡す、を明記。
9. **ハンドルの払い出し（D-46、A1-9、A0-1 の解消）**: Playback / Emitter / Transport / UserBuffer / Bank のスロットは `base::HandleAllocator`（ロックフリー）でゲーム側の登録スレッドのどれからでも払い出し、返却は所有スレッドの `Update()` が終了・解放フラグを消費したときに行う。世代は 2^31 未満（最上位ビットは使わない）。`types.h` / `sonus.h` のコメントと D-05 を合わせる。
10. **RT スレッドのスタック（D-47、A1-10）**: オーディオスレッドと RT ワーカーのスタックは 256 KiB 以上を保証する（sinc リサンプラが比率 4 超で約 32 KiB、エフェクトとソースの一時領域を見込む）。`platform::ThreadDesc` にスタックの大きさの指定が無ければ足し、RT ワーカーの生成で指定する（この件に限り platform / jobs の編集可）。プラグイン（`IDspEffect`、Callback）が使ってよいスタックの目安（64 KiB）を `ext.h` に書く。
11. **`notify.hpp` の注記**: トリプルバッファのフレームへの書き込みは `base::notify::SnapshotStore` / `SetFrameHeader` を通すこと、`SnapshotFrame::reserved` は base が「front になったときの seq」に使うことを冒頭コメントに明記。
12. `snd::CCodec`（契約の C⇄C++ 橋渡し）を既定構築できるようにし、codec の `detail::TableCodec` を置き換えられるなら置き換える。

決定に実装上の不都合（矛盾、他の契約との衝突）を見つけたら、勝手に変えずに、どこがどう困るかと代案を最終報告に書いてください。

## 第2部: 統合

13. **buildtool ⇄ codec**（A1-11）: `sonus-build` は既定で codec のエンコーダ（PCM f32 / s16、IMA ADPCM）を `EncoderRegistry` に登録する。ADPCM のブロックは 1 チャンネルあたり 1024 バイト以下。buildtool が自前で持っている PCM の詰め込みは codec のものに置き換える。シーク表は `codec::BuildSeekTable` を使う。buildtool が出した ADPCM のバンクを bank で読み、codec でデコードして、元の波形との誤差が ADPCM の範囲に収まることをテストする。
14. **buildtool ⇄ dsp**: `DspSizer` の既定を `dsp::ComputeDspPoolNeed` にし、暫定値（1 エフェクト 16 KiB と警告）をやめる。ランタイム側（`BankManagerDesc.dsp` の予約）と同じ値になることをテストする。エフェクトの種別 ID・パラメータ数・範囲を、プロジェクトのビルド時に dsp の表（`GetEffectInfo` / `FindEffect`）で検証し、未知の種別や範囲外の既定値をビルドエラー／警告にする。
15. **bank ⇄ io**: `BankManager` の読み込みを io の `IoScheduler`（`FileRequest`：サイズ問い合わせと範囲読み、クラスは Bank）に載せ替える。`bank/src/bank_io.*` の直呼びは、スケジューラ越しの実装に置き換える（テストのために `IFileSystem` 直呼びの経路を残すなら、どちらを使うかを `BankManagerDesc` で選べる形にし、既定はスケジューラ）。検証ジョブは jobs の `IJobSystem`（背景レーン）。取り消し（`FileRequest` の `Cancel`）と Shutdown の順序、スケジューラの Shutdown 中に読み込みが残っているバンクの扱いを決めて、テストする。実ワーカー（`jobs::WorkerPool`）＋実 IO スレッド＋メモリ FS（遅延と失敗の注入）での並行テストを TSan で回す。
16. **汎用部品を base に寄せる**（A1-12）: 各モジュールが `detail` に持った汎用部品のうち、複数モジュールで重複しているもの（整列、LE の読み書き、ミューテックス、CRC-32C、FNV-1a 64 など）を base に移し、各モジュールは base のものを使う。buildtool から base が見えるか依存表を確認し、見えないものは buildtool に残す。DSP 固有のもの（デノーマル除去、dB 変換、ベッセル関数など）は dsp に残す。codec のデコーダプールの空きリストは、世代つきの語と一体なのでそのままでよい。挙動を変えない移動であること。
17. **bank の検証器と buildtool の自己検査の二重持ち**（A1-13）: 現状（`bank/src/validate.cpp` からの生成コピー＋同期テスト）のままにするか、検証器をヘッダだけの共有部品として契約側（`contract/internal/include/snd/format/`）に置いて両方から使うかを検討し、後者が素直にできるならそうする（生成スクリプトと同期テストは削除）。難しければ現状維持で理由を報告。
18. **端から端までの統合テスト**（`tests/integration/`）: サンプルプロジェクト（JSON と、テスト内で生成した WAV。PCM と ADPCM のアセット、エフェクト付きのバス）を buildtool でバンクにする → メモリ FS に置く → io のスケジューラ越しに `BankManager` で読み込む（実ワーカー）→ `PreparedBank` からアセットを引き、codec でデコード → dsp でリサンプル（別のレートのアセットを内部レートへ）→ バスのエフェクトチェーンを `ComputeDspPoolNeed` で確保したプールに初期化して処理 → 出力をゴールデン音声（`tests/integration/golden/`）と比較し、解析的な検査（レベル、周波数）も付ける。RT で呼ぶ部分（デコード以外の描画側）は `SND_RT_SCOPE` 下で違反ゼロ。
19. 構成ルート `sonus`（`api/compose/`）と `api` のスモークテストが、全モジュールの実装をリンクした状態で通ること（`snd_create` はまだ `NotImplemented` でよい）。

## やらないこと

- core / source / music / api 本体（段階2以降）、Opus、実デバイス。
- 契約の、上に挙げた以外の変更（必要だと考えたら提案として報告）。
- 設計書の書き換え。

## 完了条件

- gcc Debug / clang Debug（`-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`）/ gcc `-DSND_SANITIZE=address,undefined` / gcc `-DSND_SANITIZE=thread` / gcc Release（テストあり）/ `-DSND_BUILD_TESTS=OFF` Release の全構成で警告ゼロ・`ctest` 全件通過。既存のテスト（独立テスト担当が書いた `indep_*`、`adv_*`、`defect_*`、`Defect_*` などを含む）は、契約 1.1 の変更に伴う機械的な追従（構造体のフィールド追加、関数名、列の型）以外では変更しない。追従した箇所は一覧で報告。ビルドディレクトリはリポジトリの外（例 `/tmp/snd-int-gcc`）。
- 並行性のテスト（bank ⇄ io ⇄ jobs）は TSan と Release でそれぞれ 20 回繰り返して通す。
- 例外・RTTI なし、サードパーティ依存なし。

## 終わり方

- 区切り（契約 1.1、各統合項目）ごとに、ビルドが通る状態でローカルコミットを作る（push はしない）。利用上限で中断する可能性があるので、こまめにコミットすること。
- 最終報告: 作業場所・ブランチ名・コミットの一覧、契約 1.1 の各項目をどう反映したか（決定と違えた点があれば理由）、統合の各項目の結果、既存テストを追従させた箇所の一覧、各構成の結果、未実装・未検証の箇所、段階2（core）の担当者への注意。あなたの後に、別のエージェントによる独立テストと受け入れチェックがあります。
