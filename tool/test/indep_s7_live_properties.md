# 組 P（ツール側のライブ機能・プレビュー・記録と再生）の独立テストの性質

独立テスト担当が、実装（`tool/src`、`tool/preview/src`）と実装担当のテストを読む前に、仕様と公開ヘッダだけから書き出した性質。
根拠の略記: 設計 = `spec/architecture/design.md`、LL = `spec/interface/livelink-protocol.md`（1.1）、
P-impl = 依頼書 `_briefs/P-impl.md`、ヘッダ = `tool/include/snd/tool/*.hpp`・`tool/preview/include/snd/tool/preview/*.hpp`、
案 n = 実装担当のメモ `C-notes/live.md` の「決定の案」n。
「確かめ方」の期待値は解析的に決める（ゴールデンの出力は使わない）。ゲーム側の値は ReadCell（LL §8.2）か出力のレベルで読む。

## A. 接続と照合（設計 11.2「接続時の照合」、LL §4、P-impl 1）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| A1 | ツールの文書のビルドとゲームのバンクが同じなら（Accepted）、質問なしで phase が Live になり、check() は valid・helloStatus = Accepted・全バンク Same。送る編集は 0 | LL §4、live_sync.hpp 1 | 本物の core のプレビューエンジンに同じビルドを読ませて接続し、`stats().values + chunks + structEdits + reloads == 0` |
| A2 | 不一致（BuildMismatch）で Ask なら、決めるまで phase = AwaitingDecision、pending() = true、**編集を送らない**: 待つ間に文書を編集しても、ゲームのセルは元のまま | 設計 11.2、P-impl 1「選ぶまで編集を送らない」 | 文書を 1 か所変えたビルドとゲームのビルドを食い違わせ、待つ間に別の値も編集 → ReadCell でゲームの値が元のビルドの値、`stats().sent` の SetValue / ReplaceChunk / StructEdit / ReloadBank が 0 |
| A3 | バンクごとの判定: 両方にあり内容が同じ → Same、違う → Differs、ツールだけ → ToolOnly、ゲームだけ → GameOnly。ツールとゲームのビルドハッシュが check() に入る | live_sync.hpp BankVerdict、LL §4 の 1.1 | 2 バンクの文書を作り、ゲームに (a) 片方だけ、(b) 片方を変えたもの、(c) ツールに無いバンク を読ませ、各判定を比べる |
| A4 | 「差分をすべて送る」の後、ゲームの全セル（ツールのビルドの全 live な値のセル）がツールのビルドと同じ値になる（ReadCell の current が一致）。phase が Live | 設計 11.2、P-impl 1・9 | 複数の値（バスの音量、イベントの音量など）を変えたビルドで不一致 → SendDiffs → 全セルを ReadCell して一致 |
| A5 | 「拒否」で何も変わらない: ゲームの全セルが元のまま、phase = Refused、以後の編集も送らない | 設計 11.2、live_sync.hpp | Refuse の後に編集・Tick を繰り返す → ゲーム（別の接続で読む / 出力のレベル）が元のまま |
| A6 | 1.0 の相手（`peerMinor() == 0`）: 落ちない・待ち続けない。features() で 1.1 の機能（読み返し、Revert、ホットリロード、構造の変更、全停止、バンクの一覧）が理由つきで無効。SetValue は届く | LL §3、P-impl 1、live_sync.hpp | 1.0 を名乗る手作りのゲームにつなぎ、有限の Tick で phase が決まること、features の available=false と reason 非空 |
| A7 | 再接続（D-87）: 切れてつなぎ直すと照合からやり直し、文書の未保存の編集がまた送られて、ゲームのセルが文書のビルドと一致する | P-impl 1、live_sync.hpp 1 の Reconnect | 切断中に編集 → 再接続 → ReadCell で一致 |
| A8 | 不一致の答えは再接続でどう扱われるか（案 3: 同じ LiveSync のうちは覚える） | P-impl 1「同じ選択で決める」 | 今の振る舞いを記録（仕様の穴の候補） |

## B. 編集のライブ反映（P-impl 2）

編集の種類 × 経路の表（自作）:

| 編集 | 期待する経路 | ゲームで確かめるもの |
| --- | --- | --- |
| バスの volumeDb（値） | SetValue | ReadCell と出力レベル |
| イベントの volumeDb など列の値（行の形は同じ） | SetValue（live な列）/ ReplaceChunk（live でない列） | ReadCell |
| イベントを足す（行が増える） | ホットリロード（ReplaceChunk は行数違いで UNSUPPORTED） | BankList の contentHash がツールのビルドと一致、足したイベントが鳴る |
| バスを足す・外す、エフェクトを足す・外す、センドを足す | StructEdit（予備容量の範囲）、編集器が無ければ UNSUPPORTED → ホットリロード | 最終的に BankList の contentHash が一致 |
| notes など（バンクに行かない） | 何も送らない | stats の送信が増えない |
| マテリアル表（PHYS）、パッチ（XPCH） | 未対応（blocking、理由つき） | unsupported() に理由、落ちない |

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| B1 | 値の編集が SetValue で届き、出力のレベルが dB どおり（10^(dB/20)）に変わる | 設計 11.2、P-impl 2 | DC の素材を鳴らし、バスの音量を −6.0206 dB に → 出力が 0.5 倍（1e-4） |
| B2 | Undo でゲームが元の値に、Redo でまた新しい値に戻る（ReadCell） | P-impl 2 | 編集 → Undo → Redo のそれぞれで同期を待って ReadCell |
| B3 | グループ（BeginGroup〜EndGroup）の中の多数の手は最後の値が届き、1 回の Undo で元に戻る | document.hpp、P-impl 2 | 50 手のドラッグ → 最後の値、Undo → 最初の値 |
| B4 | 間引き: 連続した N 手の送信は N より少なく、最後の値は必ず届き、届いた最終値は最後の手（順序が入れ替わらない） | P-impl 2「最後の値は必ず届く」 | 200 手を Tick ごとに 1 手 → ReadCell が最後の値、送った SetValue < 200 |
| B5 | 行の増える編集（イベントの追加）は値の経路では送れず、ホットリロード（など）で最終的にゲームのバンクの contentHash が文書のビルドと一致する | P-impl 2・3、LL §8.3・8.5 | QueryBanks（Client）で contentHash を読み、ビルドの `BankImage::contentHash` と比べる |
| B6 | 構造の変更（バスの追加）は、編集器が無い（core は UNSUPPORTED）ときホットリロードに回り、最終的に contentHash が一致。問題には出さない（ツールの次の手として正常） | LL §8.4 表、P-impl 2 | problems() が空、reloads() の最後が Done |
| B7 | 応答の失敗（ゲームが断る SetValue）は problems() に、編集の revision・オブジェクトの GUID・メンバ付きで出る | P-impl 2 | ゲームに無いセルへの編集（ゲームのバンクを書き込み不可・別ビルドにする等）→ problems() の項目 |
| B8 | 切断の最中の編集は失われず、再接続のあとゲームが文書のビルドと一致する | P-impl 1・2 | A7 と同じ。複数の手と Undo を切断中に |
| B9 | ミキサーの監視の値と文書の編集が同じセルを触るとき、ミキサーが持っている間は文書の値を送らず HeldByMixer（案 8） | P-impl 2「優先を決めて書く」 | heldByMixer が true を返すセル → 送られない、items() の状態 |
| B10 | 乱数の編集列（固定のシード、値の編集・Undo・Redo・グループ）の後、FlushNow と同期のあとで「ゲーム側の全セル = 文書のビルド」 | P-impl 2、テスト担当の重点 | 200 手の乱数列 → 全 live セルを ReadCell |
| B11 | 文書を保存してもライブの状態は変わらない（送信が増えない、phase が変わらない） | P-impl 2 | MarkSaved / Save の前後で stats が同じ |
| B12 | 端の値: 非有限の値は文書が断る（D-176c）ので送られない。同じ値の Set は送信を増やさない。巨大な dB（+1000）はゲームに届くか、届かなければ problems に出る（黙って捨てない） | D-176c、D-177b、P-impl 2 | stats と ReadCell |
| B13 | 未対応の変更（PHYS / XPCH）は unsupported() に blocking と理由つきで出て、ほかの編集の送信を止めない | P-impl 8 | マテリアル表を足した文書 → unsupported の項目、同時の値の編集は届く |
| B14 | `LiveSupportOf` は仕様の表どおり: PHYS・XPCH・PTCH は blocking、送れる値の列は supported | P-impl 8、live_sync.hpp | 直接呼ぶ |

## C. ホットリロードと予備容量（P-impl 3、LL §8.4・8.5）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| C1 | ホットリロードの最中の値の編集は失われず、順序が保たれる（最後の値が残る）。NOT_READY を problems() に出さない | P-impl 3 | リロードを起こす編集の直後に同じセルへ 3 回値の編集 → 終わった後 ReadCell が最後の値、problems 空 |
| C2 | リロードの失敗（書き出し先が失敗、ゲームが読めない）: reloads() の phase Failed、reason 非空、ゲームが読めなかったときは detached = true、problems() に出る。落ちない | P-impl 3 | 壊れたバンクを書く writer → Failed |
| C3 | reserve(): OK を受けた StructEdit だけを数える（buses 1、bytes 64 + 8 × argCount）。UndoStructEdit で戻すと 0 に戻る。限度の既定は buses 16 / effects 32 / bytes 1 MiB | LL §8.4 表、live_sync.hpp | 常に OK の編集器でバスを足し、使用量の値 |
| C4 | 書き出し先が無い（writer が null）ときに要るリロードは unsupported() に理由つき（黙って捨てない） | live_sync.hpp | writer なしで行の増える編集 |

## D. プレビューエンジンと試聴（P-impl 4）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| D1 | フェーダー: バスの volumeDb = g dB で、DC 1.0 の素材の出力 = 10^(g/20)（パンなしの 2D、中央） | 設計 11、P-impl 4 | Manual 出力で数ブロック描き、定常値 |
| D2 | 距離・パン: 3D の試聴の出力が `SpatialPreview` の計算（左右のゲイン）と一致。位置を動かすと（MoveEmitter / Update3D）出力が新しい位置の計算に追従する | P-impl 4、spatial_preview.hpp | 2 点で比べる |
| D3 | 全停止で、エンジンが始めた試聴がすべて止まり、フェードの後は無音、Status().activePlaybacks = 0 | P-impl 4 | StopAll の後に数ブロック描いて最大値 0 |
| D4 | 遷移の試聴: To への切り替えが `MusicEditor::Preview` が計算するブロック・サンプルで起きる | P-impl 4・9 | Transitions() の clock と出力のレベルの変わり目 |
| D5 | 作って壊すの繰り返し（20 回、Play したまま壊すを含む）で落ちない・漏れない（ASan） | P-impl 4、テスト担当の重点 | ループ |
| D6 | 端の値: Render(0) は 0、知らないハンドルの MoveEmitter / Stop / SetMusicState は false、2D の Play の MoveEmitter は false、NaN の SetParam / 位置で落ちず出力が有限 | preview.hpp | 直接 |
| D7 | 文書の編集が LiveSync を通ってプレビューの音に効く（別の特別扱いなし） | P-impl 4 | D1 の状態から文書を編集 → レベルが変わる |
| D8 | AuditionRouter: 宛先が無いと Ready() = false と理由、Play は ok = false で error 非空（落ちない）。ゲームへの 3D の移動は unsupported に理由 | preview.hpp | 直接 |

## E. 記録と再生（設計 2.5・11.3、P-impl 7）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| E1 | ツールの経路で記録 → 保存 → 読み込みが往復し（バイト一致）、2 回の再生で決定のログが一致、2.5 の条件（BitExactExpected）ならビットも一致し `ok` | 設計 2.5、P-impl 7 | プレビューエンジンで記録 |
| E2 | 2 回の再生が別スレッドで同時でも同じ結果（digest 一致） | replay_host.hpp「Threads」 | 2 スレッド |
| E3 | CompareRuns: 決定が違えば decisionsMatch = false と firstDecisionDiff、bitExactExpected = false なら bits を求めずに理由（ConditionReasons）を出す | recording_report.hpp | ReplayRun を手で作る |
| E4 | 期待のファイルの往復（Encode → Decode が同じ）。1 項目を変えた期待で CheckExpectation が失敗し、どこが違うかを言う | recording_report.hpp | 直接 |
| E5 | ConfigText → ParseConfigLine の往復で snd_config の全数値メンバが一致。知らない名前・壊れた値は false | recording_report.hpp | 直接 |
| E6 | 不具合報告の往復: WriteBugReport → ReadBugReport で全欄が一致し、読み戻したものを再生できる | P-impl 7 | 直接と ReplayBugReport |
| E7 | 壊れた入力で落ちない: 記録ファイルの切り詰め（全長さ）、長さの嘘（recordBytes を大きく）、巨大な数、不具合報告の report.txt の壊れた行・巨大な数、無いファイル、バンク名の `..` | P-impl 7・9 | 全部 false を返し、ASan で報告なし |
| E8 | DescribeRecording: 切れたレコード列・巨大な長さで落ちず、valid = false か範囲の内側で止まる | recording_report.hpp | 任意のバイト列（乱数のファジング、固定シード） |
| E9 | WriteWav: 44 + 4n バイトの float WAV（RIFF / fmt 3 / data の大きさが正しい） | replay_host.hpp | ファイルを読んで解析 |
| E10 | sonus-replay の終了コード: 0 一致、1 不一致、2 入力の誤り | 案 15、README | CTest で実行ファイルを起こす |

## F. スレッド（P-impl 9、common.md）

| # | 性質 | 根拠 | 確かめ方 |
| --- | --- | --- | --- |
| F1 | 実時間（Silent）のプレビューエンジンとライブリンクが動く中で、UI スレッドが編集・Tick・読み出し・試聴・切断を繰り返しても TSan の報告なし・落ちない | preview.hpp「Threads」、live_sync.hpp | TSan と Release で 20 回 |
| F2 | `RecentOutput()` と `connector()` はどのスレッドからでも呼べる | preview_engine.hpp | 別スレッドから読み続ける |
| F3 | 別々のスレッドで同時に Replay して結果が同じ | replay_host.hpp | E2 |
