# tool — オーサリングツール（段階2: ミキサーとプロファイラ）

設計書 11 章のスタンドアロンのツール `sonus-tool`。実行中のゲームにライブリンク（`spec/interface/livelink-protocol.md`）で
接続し、バスの音量・ミュート・ソロ・エフェクトのパラメータを変え（11.2 上書き層）、ゲームが送るボイス・処理時間・メモリ・
メータ・イベントログを表示し、記録（11.3）を保存・読み込みする。UI は LUDIARS **Tela**（CONTRACTS.md D-67）。

- 依存（凍結、ルート `CMakeLists.txt` の `SND_MODULE_DEPS_tool`）: buildtool, dsp, core, livelink。UI だけが加えて Tela（`find_package`）。
- `-DSND_BUILD_TOOL=ON` で中核・UI・`sonus-tool` をビルドする（Tela が要る）。
- `-DSND_BUILD_TOOL_CORE=ON` は UI に依存しない中核 `sonus_tool`（別名 `Sonus::tool_core`）だけをビルドする。Tela は要らない
  （CONTRACTS.md D-101）。ゲームエンジンのエディタなど、自前の UI を持つホストが使う。既定（両方 OFF）ではどちらも作らない。

## 外のホストから使う（D-101）

ホストは `snd/tool/session.hpp` と `Sonus::tool_core` だけで使える（ライブリンクのクライアントのヘッダは要らない）。
公開ヘッダは C++20（`Sonus::tool_core` が `cxx_std_20` を伝える）。

- ポーリング型: ホストは自分のループから `Session::Tick()` を呼び、その間にモデルを読む。ネットワークはライブリンクの
  クライアントのスレッドで動いて受信をためるだけで、ホストへのコールバックは無い。
- スレッド: 1 つの `Session` への呼び出しは同時に 1 つのスレッドから（作ったスレッドと同じでなくてよい）。
- 時刻: `Tick(nowNs)` の `nowNs` は `Session::NowNs()` と同じ時計（クライアントの受信時刻と比べる）。引数なしの `Tick()` が使う。
- 接続: `Connect(ConnectionDesc{address, port, ...})`、状態は `connection().state()` / `hello()` / `roundTripUs()`。
- ミキサー: `mixer().Set(Target{Control::BusFader / BusMute / BusSolo, bus}, value)`、`Undo` / `Redo`、
  上書きの一覧 `Overrides()` と戻し `Revert` / `RevertAll`。バスの番号はプロジェクト（`LoadProject` / `UseProject`）の並び。
- プロファイラ: `profiler().FindMeter(busId)`（0 = 出力）、`voices()`、`Summary()`（処理時間）、`memory()`、`log()`。
- 録画: `StartRecording()` / `StopRecording()`、`profiler().recordingState()` が Ready になったら `SaveRecording(path)`。

## ビルド

Tela は同梱も自動取得もしない。先にビルドしてインストールし、その場所を渡す。

```sh
# Tela（Core と Raster。Linux ではこれだけ）
cmake -S <tela> -B /tmp/tela-build -G Ninja -DCMAKE_INSTALL_PREFIX=/tmp/tela-prefix
cmake --build /tmp/tela-build && cmake --install /tmp/tela-build

# Sonus + ツール
cmake -S . -B build-tool -G Ninja -DSND_BUILD_TOOL=ON -DCMAKE_PREFIX_PATH=/tmp/tela-prefix
cmake --build build-tool && ctest --test-dir build-tool --output-on-failure -R tool_

# 中核だけ（Tela なし）。モデルのテスト（model / live_mixer / live_profiler / indep_sync / core_host と、段階5 のエディタのモデル）だけが入る
cmake -S . -B build-core -G Ninja -DSND_BUILD_TOOL_CORE=ON
```

- **Windows**: Tela を `-DTELA_BUILD_WINDOWS=ON -DTELA_PICTOR_INCLUDE_DIR=... -DTELA_PICTOR_LIBRARY=...` でビルドする
  （`Tela::Windows` が無いと構成時にエラー）。`sonus-tool --font C:\Windows\Fonts\segoeui.ttf --project <dir>`。
- **macOS**: Tela を `-DTELA_BUILD_MACOS=ON` と Pictor でビルドする（`Tela::MacOS`）。`sonus-tool --font <ttf> --project <dir>`。
- **Linux**: Tela にウィンドウのホストが無い。`--headless` で画像（PPM）を出す。Tela を `-DTELA_BUILD_PICTOR=ON` でビルド
  していれば（`Tela::Pictor`）、`--font` で文字も描く。無ければ文字以外を描く（`--text-placeholders` で文字の位置に帯を出す）。

```sh
sonus-tool --headless --project game/audio --connect 127.0.0.1:27460 --frames 60 --screen mixer --snapshot mixer.ppm
```

| オプション | 内容 |
| --- | --- |
| `--project <dir>` | 編集するプロジェクト（`spec/interface/project-format.md`）。読み込んだ後に `--build-dir`（既定 `<project>/.sonus-tool-build`）へビルドし、`Hello.projectBuildHash` にする |
| `--connect <host:port>` / `--no-connect` | 接続先（既定 127.0.0.1:27460）。切断後は自動で再接続する |
| `--refuse-mismatch` | ビルドハッシュが違えば接続を断る（既定はツールの値を全部送る） |
| `--font <ttf>` | 文字の TrueType フォント。ウィンドウのホストでは必須（Tela はフォントを同梱しない） |
| `--recording <path>` | プロファイラの Save / Load が使うファイル（既定 `sonus-recording.snrec`） |
| `--send-diffs` | 文書があるとき、ビルドの不一致に「差分をすべて送る」で即答する（既定は利用者に聞く。文書が無いときは従来どおりミキサーの値を送る） |
| `--preview silent\|device` | プレビューエンジン（ツールの中のランタイム）を動かし、それに接続する（試聴・ホットリロードの書き出し先もそれ） |
| `--reload-dir <dir>` / `--reload-prefix <p>` | ホットリロードの再ビルドしたバンクの書き出し先と、ゲームから見たその場所 |
| `--report-dir <dir>` | 記録と再生の画面の不具合報告のフォルダ（既定 `sonus-report`） |
| `--headless`, `--snapshot`, `--screen <名前>`, `--frames`, `--frame-ms`, `--size WxH`, `--dpi 0.25..4`, `--text-placeholders` | ヘッドレス実行（上記）。画面の名前は mixer buses event params spatial music material patch profiler replay link problems |

## 構成

```
tool/
  include/snd/tool/, src/   sonus_tool: UI に依存しない中核（モデル）
    project          プロジェクトの読み込み（バスの木、カテゴリ、パラメータ、エフェクトのパラメータ表）とビルドハッシュ
    connection       ライブリンクのクライアント: 接続先、状態、再接続、ビルドハッシュの不一致の扱い
    mixer            ミキサーの状態、編集の規則、取り消し・やり直し、上書きの一覧と元に戻す
    edit_history     取り消し・やり直しの履歴（まとめ・併合）
    cell_sync        値を SetValue / OverrideParam で送り、Ack で確定する
    profiler         処理時間・ボイス・メータ（ピークホールド）・メモリ・イベントログ・記録
    meter_ballistics ピークホールドとクリップ
    recording_file   記録ファイルの形式と保存・読み込み
    session          上をまとめたもの（UI のフレームごとに Tick）
    document         編集できるプロジェクト（段階5）: 全種類のオブジェクト、保存の規則、GUID、参照、リネーム、Undo / Redo
    doc_build        文書の検査とメモリ上のビルド（buildtool と同じ診断）、ビルドの差分（値の編集 / 構造の変更）、メンバ → セルの対応表
    event_editor     Event エディタ: トラックとクリップ、シーケンス種別、ランダム幅、アクション、タイムラインの計算
    mixer_editor     ミキサーの構造: バスの木と graph order、センド、エフェクト、スナップショット、ダッキング、カテゴリ、
                     ボイスリミットグループ、ライブの値の文書への書き戻し
    curve_editor     パラメータとカーブ: 点の編集、合成の規則の計算、スコープの優先、ランプ
    spatial_preview  3D 減衰のプレビュー: 上から見た図の状態とランタイムと同じ減衰・パンの計算、試聴の要求
    music_editor     音楽: テンポマップ、タップテンポとグリッド、Segment / Playlist / State / Stinger、遷移ルール表、
                     最小先読み時間、遷移の試聴の計算
    material_editor  マテリアル表: 材質と番号、単体音・組・足音・既定、代替の解決と一覧、強さのカーブ
    patch_editor     波形生成パッチ: ノード、接続の規則、検査と依存順の配列（表は buildtool の patch.hpp）
  ui/                sonus_tool_ui: Tela の宣言を作る層（OS 非依存）
    app              Tela の Runtime、画面の宣言、ポインタの振り分け
    mixer_view, profiler_view, status_view   画面の宣言（段階2）
    buses_view, event_view, params_view, spatial_view, music_view, material_view, patch_view,
    replay_view, link_view, common_view       段階5 の画面（common_view: 候補の一覧と Problems）
    kit, text_entry  画面の部品（名前・数値・候補・トグル・ページ送り・ドラッグの受け口）とツールの文字の入力
    editor_draw      タイムライン、カーブ、上から見た図、印のタイムライン、パッチ、系列の図形
    services, replay_service   再生のサービス（実装は sonus_tool_ui_replay）
    widgets          フェーダー・ノブ・メータ・グラフの図形（canvas に描く）
    control_math     値と画素の対応（下記の規則）
    pointer_router   ドラッグ・ダブルクリック・ホイール
    safe_drawing     Tela の上限を先に検査する図形ビルダ（Tela は検査に失敗すると例外を投げる）
    raster_compositor, image_file   Tela::Raster で画面を画像にする（ヘッドレス・テスト）
  app/               sonus-tool（main、オプション、ホスト: headless / Windows / macOS）
  test/              テスト（下記）
```

スレッド: UI のスレッドだけがモデルと Tela の Runtime を触る。ライブリンクのクライアントは自分のネットワークスレッドで
受信し、UI はクライアントのスレッド安全な関数（`Copy*`、`TakeAck` …）で写しを取るだけ。

## 画面と操作の仕様

**ヘッダ**: 画面の切り替え（12 面のタブ。下の「段階5 の画面」）、接続の状態（ランプ: 緑 = 接続、黄 = 接続中・ビルド不一致で
ツールの値を送った、赤 = 拒否）、レート・ブロック長・往復時間・ライブ反映の段階、ビルド不一致の質問（Send diffs / Refuse）、
Connect / Disconnect。**フッタ**: 最新のメッセージ、プロジェクト名と未保存の印・文書の Undo / Redo / Save（文書があるとき）、
問題の数（Problems 画面へ）、送信待ち・Ack 待ち・拒否された値の数。

**ミキサー**（左にストリップ、右に上書きの一覧とパラメータ）

- ストリップはバス（マスターから木の順、子は名前順）、続いてカテゴリ。入りきらなければページ（`<` `>`）。
- バスのストリップ: 名前、送信状態のランプ（黄 = Ack 待ち、赤 = 拒否）、エフェクトのパラメータのノブ（全エフェクトを通して
  先頭 6 個。超えた分は「+N params」。初期化時だけ読むパラメータ（dsp の kInit）は灰色で操作不可）、M（ミュート）と
  S（ソロ）とそのランプ、フェーダー、メータ（チャンネルごと: peak の棒、rms の棒、ピークホールドの線、クリップ）、値の表示。
- カテゴリのストリップ: フェーダーと M だけ（メータは無い）。
- 規則（`mixer.hpp`）: フェーダーの範囲は −∞（−144 dB として送る = 無音）〜 +12 dB。ミュートはバスの音量を −∞ にして送り、
  フェーダーの値は残す。ソロは「ソロのバス、その上（祖先）、その下（子孫）」以外を消す（センドは追わない）。カテゴリの
  ミュートも −∞。エフェクトのパラメータは dsp の範囲に切り詰め、整数のものは丸める。
- 上書きの一覧: バンク（プロジェクト）の値と違う値すべてと、有効なパラメータの上書き。行ごとに Revert（その項目をバンクの
  値に戻す。バスの音量ならミュートとソロも外し、まだ他のソロで消えていれば全部のソロを外す）。Undo / Redo / Revert all。
- パラメータ: グローバルのパラメータごとにノブと Override / Release。ノブを回すと上書きが有効になる（同じ取り消し 1 回）。

**操作**（`control_math.hpp`、`pointer_router.hpp`）

| 対象 | ドラッグ | Shift + ドラッグ | ダブルクリック | ホイール |
| --- | --- | --- | --- | --- |
| フェーダー | 押した位置からの縦の移動量。行程（高さ − つまみ 18 px）を動かすと位置 p が 1 変わる | 1/10 | バンクの値 | 1 ノッチ ±1 dB（Shift で 0.1）。−∞ から上げると −90 dB、−90 未満は −∞ |
| ノブ | 縦 150 px で全範囲 | 1/10 | バンクの値 | 1 ノッチ = 範囲の 1/100 |

- フェーダーの位置 p と dB: p 0 = −∞、0+ = −90、0.05 = −60、0.25 = −30、0.5 = −12、0.75 = 0、1 = +12 の折れ線。端で飽和する
  （上に行き過ぎても +12、下は −∞）。ノブは範囲に線形、`Hz` で最小が正のものは対数。
- ダブルクリック: 同じコントロールを 400 ms 以内・4 px（論理画素）以内でもう一度押す。パラメータのノブのダブルクリックは
  上書きの解除（Revert と同じ）。ドラッグは 1 回が取り消し 1 回。ホイールは同じコントロールに続けて回した分が取り消し
  1 回にまとまる（パラメータのノブで上書きを有効にした分も含む）。元の値に戻ったドラッグ・ホイールの連続は取り消しの手を
  残さない。Shift を途中で押し離ししても値は跳ばない。
- 縦の移動量が 0 のドラッグ（押して離す、横にだけ動かす）は値も履歴も変えない。−90 dB 以下（位置 0）から始めたドラッグは
  位置 0+ から相対で動き、縦に動いたときだけ折れ線の値になる。
- メータ: −60〜0 dBFS に線形、棒の高さ = 高さ ×（dB + 60）/ 60。区分の色は −18 dB 未満・−18〜−6・−6 以上。ピークホールドは
  新しい BusMeters が届いたときだけ更新し、1.5 秒保持してから 20 dB/秒で落ちる。0 dBFS に達するとクリップを点け、
  Profiler の「Reset clips」まで残る。新しいメータが 500 ms 来なければ（切断・ゲーム停止）、peak / rms も 20 dB/秒で下がり、
  最後は −∞ になる（凍結しない）。
- ショートカット: Ctrl+Z（macOS は Cmd+Z）取り消し、Ctrl+Y / Ctrl+Shift+Z やり直し（ミキサー画面ではライブの値の履歴、
  ほかの画面では文書の履歴）、Ctrl+S 文書の保存、F2 ミキサー、F3 プロファイラ、F4 Buses、F5 Event、F6 Params、F7 3D、
  F8 Music、F9 Material、F10 Patch、Ctrl+PageDown / Ctrl+PageUp（Ctrl+Tab / Ctrl+Shift+Tab）でタブを順に、Delete / Backspace
  で選んだもの（クリップ、カーブの点、パッチのノード）を消す。

**プロファイラ**

- 処理時間のグラフ: ブロックごとの logic + render（強調色）と render（淡色）、予算の破線。縦軸は max(1.25 × 予算,
  1.1 × 最大値)、直近 240 個。右に要約（render の最新・平均・最大、logic、予算と負荷 %、予算超えの数、LOD、遅れたチャンク、
  アンダーラン、ボイス数、ストリームのストール、LOD の変化）。
- ボイス一覧（実ボイスが先、可聴度の大きい順）: Event、状態、バス、可聴度、ゲイン、ソースの種類。イベントログ（新しい順）。
- メモリ: 区分（`snd_mem_tag`）ごとの使用量と予算、棒。
- 記録: Record / Stop（RecordControl。Ack で状態を進め、停止の Ack の後にデータが 100 ms 増えなければ Ready）、Save
  （`--recording` のファイルへ）、Load（読み込んで要約: アリーナ・LOD・ストール・バンクの数とブロックの範囲。壊れていれば拒否）。

**接続**（`connection.hpp`、`cell_sync.hpp`）

- 値は変わったときだけ送る（同じフレーム内のドラッグは 1 通）。Ack の `SND_OK` で確定、`SND_ERR_COMMAND_BUFFER_FULL` は
  再送、それ以外は「拒否」として残し、同じセッションでは値がまた変わるまで送らない（新しいセッションでは送り直す）。
- Ack が 2 秒届かない値は「応答なし」と表示する（ストリップのランプが橙、フッタの数）。送り直しは再接続時の規則に任せる。
- 接続（再接続を含む）のたびに、バンクと違う値と一度でも送った値を送り直す（ゲームが再起動しても、
  `revertOnDisconnect` で戻されても、ツールの状態に戻る）。切断中の編集は「送信待ち」で残り、接続したら送る。
- ビルドハッシュの不一致: 既定はツールの値（ミキサーが管理する全部）を送る。`--refuse-mismatch` なら切断し再試行しない。

**記録ファイル**（`recording_file.hpp`）: 64 バイトのヘッダ（`SNRC`、版 2、フラグ（クライアントのバッファあふれ）、ツールと
ゲームのビルドハッシュ、レート、ブロック長、機種、契約の版、長さ、FNV-1a 64）＋ゲームが送った記録そのまま（protocol §7）。
チェックサムはヘッダ（チェックサムの欄を除く 56 バイト）とレコードの両方を覆う。読めないレコード（未知の kind など）が
あれば、チェックサムが合っていても読み込みは失敗し、前に読み込んだものは変わらない。版 1 は読まない。

**名前の重複・ID**: Tela の要素 ID と上書き一覧のキーは、オブジェクトのキー（種類の中でランタイム ID が一意ならその 16 進、
重複していれば `g` + GUID、GUID も重複していれば `n` + 通し番号）から作る。`n` + 通し番号はプロジェクトの並び順で決まるので、
ファイルを足したり消したりすると変わる。キーはそのプロジェクトを読み込んでいる間だけ有効で、保存や共有には使わない。名前が重複したプロジェクトはビルドできないが、
ツールは開き、ヘッダにエラー数、フッタにメッセージを出す。

**その他**: `VoiceInfo.isVirtual` は 0 以外を「仮想」とする真偽値。パラメータ上書きの Revert は上書きを無効にし、ツール上の値も
バンクの既定に戻す（再度 Override すると既定値から始まる）。DPI の範囲は 0.25〜4。`--dpi` は範囲外の値を拒否し（起動しない）、`App::SetViewport` は範囲内に切り詰める。
図形の作業量は実際の DPI で見積もり、Tela の予算を超えるときは線を細かく分け、それでも足りなければ点を間引く。

## Tela の使い方

- 要素は panel / text / button / canvas だけなので、フェーダー・ノブ・メータ・グラフは canvas に図形で描く。すべての要素を
  位置指定（`Layout::positioned`）で置き、ID はランタイム ID から作る（`bus.<id>.fader` など。リストの行だけは行番号）。
- 操作の受け方: Tela はボタンの押下・解放・発火しかアプリに渡さず、canvas の押下は宣言を差し替えると取り消される。そこで
  各コントロールを「排他入力のボタン（`.hit`、文字なし）＋その上に入力を通す canvas」で宣言する。Tela がボタンとして
  ジェスチャーを所有し（宣言の差し替えをまたいで保持、マウスのキャプチャ、フォーカスを失えば取り消し）、ツールは Tela が
  そのボタンを押下中と言っている間だけドラッグとして位置を追う（`pointer_router.hpp`）。位置・ホイール・修飾キーはホストが
  OS から観測して渡す（Windows はウィンドウのサブクラス、macOS は NSEvent のローカルモニタ）。
- 例外: Tela は検証に失敗すると例外を投げる。Sonus 側は投げない呼び方だけをする（図形は `SafeDrawing` が座標・数・作業量を
  先に検査、viewport・テーマも検査してから渡す）。ホストの 3 ファイルだけを `-fexceptions` でコンパイルし、Tela の
  コンストラクタと `synchronize()` の例外を境界で捕まえる（D-67）。
- テーマ: Tela の `Theme`（panel / text / button / pressed）から色を作る（`palette.hpp`）。区分の色（メータ、ミュート、ソロ、
  クリップ）だけ固定の色相をテーマのパネル色に寄せて使う。

## テスト

| テスト | 内容 |
| --- | --- |
| `tool_model_test` | プロジェクトの読み込み（木の順、GUID、dsp の表、エラー）、ビルドハッシュ、フェーダー・ミュート・ソロの規則、セルの宛先、取り消し・やり直し・併合、上書きの一覧と Revert、オフライン、記録ファイル、ピークホールド |
| `tool_live_mixer_test` | 本物の core とライブリンクのサーバ（メモリ転送）。フェーダー・マスター・ミュート（完全な無音）・ソロ・取り消し・エフェクトのパラメータ・カテゴリが出力のレベルを解析どおりに変える、切断・再接続・`revertOnDisconnect` の後もツールの状態に戻る、ビルド不一致（送る / 断る / 知らないバスは拒否として報告）、パラメータの上書き |
| `tool_live_profiler_test` | メータ（マスター = 出力、バス別）が描いた出力と一致、処理時間・ボイス・メモリ・イベントログ、記録の保存と読み込みの往復（壊れたファイルは拒否） |
| `tool_document_test` | 文書: 数値の表記、読み込み → 保存で何も書かない、編集 → 取り消し → 保存で何も書かない、知らないメンバと notes の保持、仕様の順、Undo / Redo（グループ・併合）、GUID とファイル名、リネームと別名、参照と削除、壊れた入力、buildtool と同じ診断（文言・位置）、メモリ上のビルド、対応表の全項目、値の編集と構造の変更の分類 |
| `tool_event_editor_test` | Event エディタ: タイムライン（WAV から求める長さ、ループ区間、ピッチ、入れ子、無限、重なり）、規則の拒否、全編集・Undo / Redo・保存の往復・バンクの列の値、ドラッグの併合、パッチと synth のクリップ |
| `tool_mixer_editor_test` | ミキサーの構造: 木と循環、graph order がビルドの `BUSS` の並びと一致、センドとエフェクト（dsp の範囲、MixerModel と同じ）、スナップショット（取り込み・重み付きの結果）、ダッキング・カテゴリ・ボイスリミットグループ、書き戻し |
| `tool_curve_editor_test` | カーブ: 点の順序・最小数・shape、全カーブの編集、パラメータの規則、合成（dB・セントの加算、最小・最大、パンの加算、乗算）とスコープの優先、ランプ |
| `tool_spatial_preview_test` | 3D: 逆距離と maxDistance、コーン、距離カーブ、方位とパン、外挿、2D、表示の変換、試聴の要求 |
| `tool_music_editor_test` | 音楽: テンポマップ（解析値）、タップテンポとグリッド、Segment / Playlist の編集と規則、State・ルール・Stinger（照合、到達できない行がビルドの警告と一致、自動の並べ替え）、最小先読み、遷移の試聴、保存の往復 |
| `tool_material_editor_test` | マテリアル表: 番号の付け替え、対称な組、9.3 の代替順と代替の一覧、強さのカーブ、`PHYS` の中身 |
| `tool_patch_editor_test` | パッチ: ノードと接続の規則、上限、ビルドと同じ検査、依存順、`XPCH` の中身、位置はバンクに届かない |
| `tool_decisions_test` | 差し戻しの決定（D-176〜D-180 と 2 回目の追記: 別の綴りの同じディレクトリ、SaveTo の宛先の規則、明示した既定値と同じ位置への移動）: 負のゼロ、非有限の拒否、値が変わらない設定は成功、GUID の諦め、予約名、保存の失敗と保留・やり直し、名前を付けて保存、スキーマの全メンバの分類、LocateField と DiffBuilds の一致（無い列は live でない）、ビルド結果を複数スレッドで読む、最後の材質、stem の範囲、拍子は小節線、AutoSort の拒否、近似の位置、効かない compose、強さの有限性 |
| `tool_runtime_compare_test` | ランタイムとの一致: カーブの評価（core の `EvaluateCurve` とビット一致）、3D のプレビューとパラメータの合成（本物の core の出力と比べる）、テンポマップ（music の `Timeline`）、ルールの照合（music の `FindRule`） |
| `tool_ui_declaration_test` | ストリップの数と並び、ID の安定（フレーム・値の変更・画面の往復）、ボタンがモデルを動かす、ページ |
| `tool_ui_gesture_test` | ドラッグ量と値（端で飽和）、宣言の差し替えをまたぐドラッグ、Shift、ダブルクリック、ホイールの併合、ノブ（線形・対数）、取り消し・フォーカス喪失 |
| `tool_ui_raster_test` | Tela::Raster で描いた画素: メータの棒の高さ（DPI 1 と 2）、グラフの点と予算の線、フェーダーのつまみ、図形の上限 |
| `tool_ui_concurrency_test` | ゲームのスレッドとクライアントの受信スレッドが動く中で UI のスレッドがフレーム・ドラッグ・記録・切断を繰り返す（TSan 用） |
| `tool_ui_snapshot_test` | 接続中のツール全体を描き、マスターのメータの棒が出力のレベルの高さにあること。`SND_TOOL_SNAPSHOT_DIR` を設定すると画像を書く |

## 段階5 のモデル（組 M）

全画面の中身は `Sonus::tool_core` にある（Tela に依存しない。Ergo Studio の sonus プラグインも同じものを使える）。
画面（Tela）、ライブへの反映・試聴・再生は後の組が足す。

- 文書（`ProjectDocument`）を 1 つ持ち、各エディタ（`EventEditor`、`MixerEditor`、`CurveEditor` / `ParameterEditor`、
  `SpatialPreview`、`SegmentEditor` / `PlaylistEditor` / `MusicEditor`、`MaterialEditor`、`PatchEditor`）はその上の型のある窓。
  どの編集も 1 つの履歴で取り消せる。ライブの値（`MixerModel`）の履歴は別で、`WriteBackMixer` が文書の 1 手として書き戻す。
- 検査は `CheckDocument`（buildtool を保存前の文書に走らせる）。ビルドは `BuildDocument`（メモリ上、必要ならディスクにも）。
  ライブ用には `DiffBuilds` が編集を値の編集（SetValue の宛先付き）と構造の変更（チャンクの差し替え）に分ける。
- buildtool は `BuildOptions::source`（ProjectSource）と `memoryOutput` で、ディスクを介さずに動く（CLI は変わらない）。

## 段階5 のライブ機能・プレビュー・記録と再生（組 P）

核（`Sonus::tool_core`）にモデル、ランタイムを動かす部分は別のライブラリ `Sonus::tool_preview`（`tool/preview/`、
`SND_BUILD_TOOL_CORE` か `SND_BUILD_TOOL` で作る。Tela は要らない）。12.1 の依存表で tool は buildtool・dsp・core・
livelink だけなので、ソースの種別・音楽・IO を組み立てるプレビューは構成ルート `sonus`（api/compose、D-93）と同じ扱いの
別のターゲットにした（`compose::SourceKinds` を使う）。

| ファイル | 役割 |
| --- | --- |
| `live_sync` | 文書（`ProjectDocument`）のライブ反映。接続時の照合（バンクごとの比較、差分をすべて送る／拒否、決定の保留）、編集・Undo・Redo の反映（値は SetValue、形が同じ表は ReplaceChunk、バス・エフェクト・センドの追加と削除は StructEdit、ほかはホットリロード）、間引き、背圧、項目ごとの状態、問題・未対応の一覧、ホットリロードの経過、予備容量。未対応の判定は `LiveSupportOf` の 1 か所 |
| `preview` | プレビューエンジンの抽象 `IPreviewEngine` と試聴の振り分け `AuditionRouter`（宛先はゲームかプレビュー） |
| `profiler_streams` | プロファイラの残り（CPU と BlockDetail、メモリの区分、ストリーム、LOD、ストール、イベントログの絞り込み）。画面に出すものだけ購読し、更新番号で取りこぼしを数える |
| `recording_report` | 記録の要約（1.1 のレコードを含む）、再生の比較（再生どうし、記録した実行のログ）、テストの期待のファイル、不具合報告のフォルダ、Config のテキスト |
| `cell_sync` / `mixer` / `session`（追記） | 項目単位の Revert（1.1 の相手には RevertValue）、読み返し（ReadCell）、`RevertOnly`、`Session::AttachDocument`（文書のライブ反映）、`EnableProfilerStreams` |
| `connection`（追記） | `deferSyncDecision`、`awaitingSync` / `DecideSync`、`peerMinor` |
| `preview/preview_engine` | ツールの中のランタイム（メモリ上のファイルシステム、IO とストリーム、ソースの種別、音楽、core、プロセス内の転送路のライブリンク）。出力は手動（テスト・WAV）、デバイス、無音（実時間） |
| `preview/replay_host` | 記録の再生（2 回再生して比較、期待と照合、不具合報告から再生、WAV、デバイスで聞く `OutputPlayer`） |
| `preview/cli/sonus_replay` | `sonus-replay`: UI なしで記録と不具合報告を再生する（終了コード 0 一致、1 比較の不一致、2 入力が読めない・再生そのものの失敗（報告からの再生も）、`--help`） |

使い方（ホストの UI スレッド）:

```cpp
ProjectDocument doc; doc.Load(dir);
Session s; Project p; p.Load(doc); s.UseProject(p);   // ミキサーのバンクの値も文書から
LiveSyncOptions o;                                      // 既定: 不一致は利用者に聞く（Ask）
o.writer = DirectoryReloadWriter(gameDir + "/live", "live/");  // ホットリロードの書き出し先
s.AttachDocument(&doc, o);
s.Connect(desc);                                        // Hello のハッシュと決定の保留は LiveSync が入れる
// 毎フレーム: s.Tick(); 画面は s.live()->phase() / check() / items() / problems() / unsupported() /
// reloads() / reserve() を読む。不一致の質問には s.live()->ChooseSendDiffs(s.connection()) か ChooseRefuse。
```

プレビューエンジン（`tool_preview`）: `PreviewEngine::Create(desc)` → `LoadBuild(BuildDocument(doc))` →
`desc.connector = engine->connector()` で接続する。ホットリロードの書き出しは `engine->PutRebuiltBank`。
試聴は `AuditionRouter`（`Bind(&connection, engine)`、`SetTarget(Game / Preview)`）。

差し戻しの決定（D-182〜D-185）で決まった振る舞い:

- 接続のたびに Hello のハッシュは今の文書のビルド。不一致の答えは同じゲーム（バンクの一覧）のときだけ覚える。再接続では、
  これまでに送った全セル・差し替えた表を今のビルドの値で送り直す（ゲームが上書きを残していても文書になる）。
- 送り直しは同じセルの新しい値を追い越さず、諦めるのは時間（`giveUpMs`）。ミキサーが離したセルは文書の値を自動で送る。
  文書の Undo の Remove で予備容量を返し、ホットリロードの成功でそのバンクのライブの分は 0。ライブで足したオブジェクトの値は
  ホットリロード。バスの追加は今のバンク形式ではマスターの行番号を動かすので、実際にはホットリロードになる。
- リロードの失敗・書き出しの失敗は `LivePhase::Failed`（`failedBanks()` に理由）で、次の成功まで Live に戻らない。
- 遷移の試聴の要求は時刻以降で最初のブロックの頭（少なくとも 1 ブロック後）。`MusicEditor::Preview` も同じブロック
  （`applySeconds`）とランタイムの先読み（素材のプライムだけ）で計算する。
- 再生は記録の長さ（`maxBlocks`）と番号合わせの空回し（`maxAlignBlocks`）に上限（既定は実時間 1 時間）。不具合報告は全項目が
  往復する（`notes.txt`・`description.txt`・`comparison.txt`）。Config のテキストは符号・範囲・型を確かめる。

スレッド: モデル（LiveSync、ProfilerStreams、AuditionRouter、Session）は UI のスレッドだけ。プレビューエンジンの関数も
作ったスレッドだけ（`connector()` と `RecentOutput()` はどこからでも）。手動の出力では作ったスレッドが `Render` で描き、
デバイス・無音の出力ではオーディオスレッドが描いて、作ったスレッドが毎フレーム `Pump` する。再生（`Replay`）は呼んだ
スレッドがランタイムを 1 つ作って描く（別々のスレッドで同時に回してよい）。

テスト（`tool/test`、Tela 不要）:

| テスト | 内容 |
| --- | --- |
| `tool_live_preview_test` | プレビューエンジン: 文書から鳴らしたレベル（バスの音量）、3D（SpatialPreview の値と一致、動かす）、ライブの編集が音に効く、全停止、遷移の試聴が MusicEditor::Preview の計算どおりのブロック・サンプルで切り替わる、ゲームへの試聴 |
| `tool_live_sync_test` | 照合の一致・不一致と 2 つの選択、SetValue / ReplaceChunk / StructEdit / ホットリロード、Undo / Redo、間引き（最後の値が届く）、失敗が問題に出る（外れたバンク）、再接続、ミキサーの監視の値の優先、未対応の一覧、Session との組み合わせ |
| `tool_live_revert_test` | 項目単位の Revert（RevertValue、Undo で SetValue）、読み返し、`RevertOnly` |
| `tool_profiler_streams_test` | 購読のマスク、CPU・BlockDetail・メモリ・ストリーム・LOD・イベントログの絞り込み |
| `tool_replay_test` / `tool_replay_cli` | プレビューエンジンで記録 → 保存 → 読み込み → 2 回再生（決定とビットの一致）、記録した実行のログとの比較、期待のファイル、不具合報告の往復、WAV、壊れた入力、`sonus-replay` |
| `tool_live_v10_test` | 1.0 のゲーム（手で動かすゲーム側）: 1.1 の機能は理由つきで無効、1.0 のメッセージは使う、待ち続けない |
| `tool_live_concurrency_test` | 実時間のプレビューエンジンとライブリンクの中で UI スレッドが編集・読み出し・試聴・切断を繰り返す、2 スレッドで同時に再生（TSan と Release で繰り返す） |

## 段階5 の画面（組 U）

設計書 11.1 の全画面を、ミキサー・プロファイラと同じ作り方（位置指定の要素、排他入力のボタン＋入力を通す canvas、
`SafeDrawing`）で作った。画面はモデル（`tool/include/snd/tool/*.hpp`）を読んで宣言を作り、操作はモデルの関数に渡すだけ
（編集の規則も計算もモデルの側。どの編集も文書の 1 つの履歴で取り消せる）。文書（`Workspace::document`）が無いときは
エディタの画面は「`--project` で起動する」と出す。

| タブ（`--screen`） | ファイル | 中身 |
| --- | --- | --- |
| Mixer（`mixer`） | `mixer_view` | 段階2 のストリップ。右の一覧に「Read back」（1.1 の読み返し）と「Write back to project」（ライブの値を文書へ、1 手） |
| Buses（`buses`） | `buses_view` | Buses: バスの木（追加・削除・名前）、音量・親・チャンネル・フラグ・LOD 優先度、センド（宛先・レベル・pre/post・追加・削除）、エフェクトの鎖（追加・並べ替え・削除・全パラメータ）、graph order。Snapshots: 値（種類・バス・値・番号）、ミキサーから取り込み、重みと結果。Ducking: 種類・元・先・量・アタック・リリース・しきい値。Categories: 親・音量・フラグとボイスリミットグループ |
| Event（`event`） | `event_view` | 一覧、Event の項目（シーケンス種別・バス・カテゴリ・数値・フラグ・コーン）、タイムライン（トラックとクリップ、ルーラー、ズームとスクロール、ドラッグで開始位置、上下でトラック、ダブルクリックで素材を足す）、クリップの項目（ランダム幅、フェード、weight、switch、layer、loop、synth）、トラックのバスとアクション、試聴 |
| Params（`params`） | `params_view` | パラメータ（種類・スコープ・範囲・既定値・追従速度と軌跡）、カーブの図（点のドラッグ・ダブルクリックで追加・Delete で削除、shape、対象・合成・入力）、合成の規則の確認（値とスコープ、距離 → 対象ごとの結果と内訳、警告） |
| 3D（`spatial`） | `spatial_view` | 上から見た図（リスナー・エミッタ・向きの取っ手のドラッグ、背景のドラッグで移動・ホイールで拡大）、maxDistance の円とコーン、計算の値、出力の利得、距離カーブと今の距離、試聴（動かすと追従。宛先が無ければ理由つきで無効） |
| Music（`music`） | `music_view` | Segments（素材・小節・入口と出口、拍子と小節のタイムライン、マーカーのドラッグ、テンポ点・マーカー・Stem の表、タップとグリッド）、Playlists、States & rules（State の表、遷移ルール表の編集・並べ替え・自動の並べ替え・到達できない行の印、遷移の試聴の計算をタイムラインに、試聴の要求、最小先読み）、Stingers |
| Material（`material`） | `material_view` | 表、材質と番号（付け替えの表示）、単体音・足音、強さのカーブと入力の結果、組の表（8×8 の窓をスクロール）、既定、接触の解決、代替の一覧 |
| Patch（`patch`） | `patch_view` | ノードの図（ドラッグで移動、出力の端子から入力の端子へドラッグで接続、または端子を選んでつなぐ、つながった入力を押すと切断、背景のドラッグでスクロール）、ノードの追加・削除・名前・パラメータ、接続の一覧、検査と依存順 |
| Profiler（`profiler`） | `profiler_view` | Overview（段階2）、CPU（負荷のグラフと BlockDetail の表）、Memory、Streams（スロットとストール）、LOD（段と履歴）、Event log（絞り込みと取りこぼしの数）。タブを開くとその View を購読する |
| Replay（`replay`） | `replay_view` | 記録・保存・読み込み（ファイル名は入力）、記録の要約、2 回の再生と比較、記録した実行との比較、WAV と試聴、不具合報告の書き出しと読み込み・再生（`ReplayService`） |
| Link（`link`） | `link_view` | 接続先の入力、状態と段階、照合（版・ハッシュ・バンクごとの判定）、不一致の質問、1.0 の相手で無い機能、ホットリロードの経過、予備容量と構造の変更（行ごとに Undo）、数、Resync。Items: オブジェクトごとの経路と状態 |
| Problems（`problems`） | `common_view` | ビルドの検査の問題（エラーが先）、ライブの問題、ライブに出せないもの。行の Go でその編集画面へ |

**文字の入力（Tela に無い。回避は 1 つに揃えた）**: ホストがキーボードを観測して `App::Key(KeyEvent)` に渡し（Windows は
WM_KEYDOWN と WM_CHAR、macOS は NSEvent のローカルモニタ）、ツールの中の `TextEntry` が文字列を組む。
- 名前: 欄を押すと今の名前で入力が始まる。Enter で確定（モデルが拒否すれば入力は開いたままメッセージ）、Esc で取り消し、
  Backspace で 1 文字消す。新しいオブジェクトは「Event 1」のように連番の名前で作り、あとで名前を変える。
- 数値: `[-] 値 [+]`。ボタンで 1 ステップ、値の上を縦にドラッグ（4 px で 1 ステップ、Shift で 1/10、ドラッグ 1 回が取り消し 1 回）、
  ホイール（続けて回した分が取り消し 1 回）、動かさずに押すと数字の入力。
- 参照と選択肢: 欄を押すと候補の一覧（画面の上に重ねる。外を押すか Esc で閉じる。多ければページ送り）。
- Windows では Tela が Esc でウィンドウを閉じるので、入力中・一覧が開いている間の Esc だけは Tela に渡さない。

**差し戻しの決定（D-186〜D-189）**:
- 入力中（`TextEntry` が開いている間）は全部のキーが入力欄に行く（Ctrl+Z / Ctrl+Y、Delete、画面の切り替えのキーは何もしない）。
  入力も一覧も開いていない時の Esc もツールが受け取り、ドラッグ中ならそのドラッグを取り消し（文書の 1 手を戻す）、パッチの
  接続待ちがあれば消す。それ以外は何もしない（Windows でも Tela にウィンドウを閉じさせない）。
- 打った数値は切り詰めずにモデルに渡す（拒否なら入力は開いたまま、文書は変わらない）。± / ドラッグ / ホイールは欄の範囲で止まる。
  ホイールと連打の ± は、宣言の間でもモデルの最新の値に足す（n ノッチは n ステップ）。数値の欄の値は少なくとも 48 px（ラベルが
  先に縮み、24 px を切ればラベルを値の文字に入れる。ボタンの入らない欄はボタン無し）。
- クリップを上下に動かすと、トラックは `round(縦の移動量 / 48 px)` だけ移り、範囲の外は端のトラックへ。
- 選択は対象で持つ（GUID、名前、ノード id）。オブジェクトの中の位置で持つ選択（クリップ、アクション、カーブの点、テンポ点、
  Stem、Playlist の行、ルール）は、Undo / Redo と画面の切り替え（ほかの画面の編集）で外す（選んでいない物を Delete で消さない）。
- 文書の構造が変わってミキサーを結び直すとき、ライブの値は対象（GUID、エフェクトの種類と位置）で引き継ぐ
  （`MixerModel::Rebind`）。引き継げない値があれば結び直しを保留し、本体の上に「書き戻す／捨てる」の質問を出す。答えるまで上書きは残る。
- 名前で一意な物（材質、State、マーカー、Stinger）の要素の ID は名前から作る（`NameKey`）。位置の番号は名前の無い行だけ。
- コーンは `EventEditor::Cone()`（無いときは 0 / 0 / 0 dB = コーンなし）で Event と 3D の両画面に同じ値を出す。3D の既定のエミッタは
  maxDistance の半分（無ければ 5 m）だけ右前に置く。カーブの図は x にも余白（`kCurveXMargin`）。Link は未接続なら相手の版を出さない。
  Problems の一覧は同じ問題を 1 回にする（`DistinctIssues(CheckDocument(doc))`。`CheckDocument` は buildtool と一語一語同じまま）。

**ドラッグの受け口**（`pointer_router.hpp` の `ControlKind::Custom` / `DragHandler`）: 描いた物（クリップ、点、リスナー、ノード、
数値の欄）の下に排他入力のボタンを置き、押下・移動・離す・ダブルクリック・ホイールを渡す。押した時のハンドラを
ジェスチャーの終わりまで使う（宣言が毎フレーム差し替わっても状態を保つ）。要素が消えれば離したことにする。
文書の編集は最初の移動でグループを開き、離した時に閉じる（1 手）。位置のドラッグは押した点からの絶対量、数値の
ドラッグは移動量の積み上げ（Shift で 1/10）。

**ミキサーと文書**: 文書のバス・カテゴリ・パラメータの構成が変われば、セッションのプロジェクトを文書から作り直して
ミキサーを結び直す（`MixerModel::Bind`）。値だけの変更は、ミキサーに上書きが無ければ結び直し、あればメッセージを出して
書き戻すか全部戻すまで待つ（上書きを消さない）。

**記録と再生のサービス**: 再生はランタイムを要るので、UI のライブラリ（`sonus_tool_ui`、核だけに依存）は `ReplayService`
（`services.hpp`）を通す。実装は `sonus_tool_ui_replay`（`Sonus::tool_preview` を使う）。`sonus-tool` は両方をリンクする。

テスト（Tela 入り、`tool/test`）:

| テスト | 内容 |
| --- | --- |
| `tool_ui_event_screen_test` | Event: 宣言と ID の安定、クリップのドラッグ（開始位置・トラック、取り消し 1 回）、ボタンと候補、数値の入力と拒否、名前、ズームとスクロール、クリップの箱の画素（DPI 1・2） |
| `tool_ui_buses_screen_test` | バスの木と項目、新しいバスでミキサーが追従、センド、エフェクト、スナップショット（重みの結果、取り込み）、ダッキング、カテゴリ、グループ、書き戻し |
| `tool_ui_params_screen_test` | パラメータの項目と軌跡、点のドラッグ（値の単位）・追加・削除、対象と合成、合成の確認が `ComposeEvent` と一致、点の画素 |
| `tool_ui_spatial_screen_test` | エミッタと向きのドラッグ、拡大、値が `Compute` と一致、プレビューエンジンへの試聴と追従、宛先が無いときの無効、円と点の画素 |
| `tool_ui_music_screen_test` | Segment のマーカーのドラッグ、テンポ・マーカー・Stem、グリッド、Playlist、ルール表（到達できない行、自動の並べ替えの拒否と実行）、試聴の計算が `Preview` と一致、Stinger |
| `tool_ui_material_screen_test` | 材質と番号の付け替え、単体音・足音・組（対称）・既定、解決、強さ、64 材質の窓 |
| `tool_ui_patch_screen_test` | ノードの追加・移動（取り消し 1 回）・パラメータ・削除、ドラッグとクリックでの接続、切断、検査と順、上限 |
| `tool_ui_live_screens_test` | プレビューエンジンをゲームにして: Link（照合、不一致の質問をヘッダと画面から）、プロファイラのタブ、記録 → 保存 → 読み込み → 2 回の再生 → WAV → 不具合報告 |
| `tool_ui_editor_limits_test` | 大きなプロジェクト（Event 300、40 トラック × 30 クリップ、ルール 100 行、ノード上限、64 材質、バス 60）の全画面を DPI 0.25・1・2・4 で: 要素の数、ID の重複なし、図形の上限、画像。ページ送り |
| `tool_ui_editor_input_test` | 文字の入力（UTF-8、数値、拒否）、キー（画面、文書とミキサーの取り消し、保存）、ドラッグの受け口（差し替えをまたぐ、要素が消える）、数値の欄（ドラッグ、Shift、ホイール）、候補の一覧 |
| `tool_ui_editor_concurrency_test` | 実時間のプレビューエンジンとライブリンクの中で、UI スレッドが画面から編集・試聴・切断を繰り返す（TSan） |
| `tool_ui_editor_fix_test` | 差し戻しの決定: 打った数値の拒否、宣言の間のホイールと連打、縦のドラッグの移動量と端、数値の欄の幅（1280×800 と 640×400）、Pick、入力中のキー、Esc とドラッグの取り消し、位置の選択と Undo、ミキサーの結び直しの引き継ぎと質問、コーン・取っ手・余白・未接続・問題を 1 回、名前から作る ID |
| `tool_ui_editor_limits_<名前>` | 上限のテストを画面ごとに別のプロセスで（320×240 まで） |
| `tool_model_fix_test`（核） | `MixerModel::LiveValues` / `Unmatched` / `Rebind`、`EventEditor::Cone`、`DistinctIssues` |
| `tool_app_screen_<名前>` | `sonus-tool --headless --screen <名前>` が全画面で画像を書く |

## 段階2で入れていないもの

（段階5 の組 M・P・U で、各エディタ、センド・スナップショット・ダッキングの編集、接続先やファイル名の画面上での入力
（ツールの中の文字の入力）、構造の変更・チャンクの差し替え・記録の再生を入れた。）
