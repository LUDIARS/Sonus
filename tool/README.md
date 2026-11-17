# tool — オーサリングツール（段階2: ミキサーとプロファイラ）

設計書 11 章のスタンドアロンのツール `sonus-tool`。実行中のゲームにライブリンク（`spec/interface/livelink-protocol.md`）で
接続し、バスの音量・ミュート・ソロ・エフェクトのパラメータを変え（11.2 上書き層）、ゲームが送るボイス・処理時間・メモリ・
メータ・イベントログを表示し、記録（11.3）を保存・読み込みする。UI は LUDIARS **Tela**（CONTRACTS.md D-56）。

- 依存（凍結、ルート `CMakeLists.txt` の `SND_MODULE_DEPS_tool`）: buildtool, dsp, core, livelink。加えて Tela（`find_package`）。
- `-DSND_BUILD_TOOL=ON` のときだけビルドする。既定（OFF）では Tela は要らない。

## ビルド

Tela は同梱も自動取得もしない。先にビルドしてインストールし、その場所を渡す。

```sh
# Tela（Core と Raster。Linux ではこれだけ）
cmake -S <tela> -B /tmp/tela-build -G Ninja -DCMAKE_INSTALL_PREFIX=/tmp/tela-prefix
cmake --build /tmp/tela-build && cmake --install /tmp/tela-build

# Sonus + ツール
cmake -S . -B build-tool -G Ninja -DSND_BUILD_TOOL=ON -DCMAKE_PREFIX_PATH=/tmp/tela-prefix
cmake --build build-tool && ctest --test-dir build-tool --output-on-failure -R tool_
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
| `--headless`, `--snapshot`, `--screen`, `--frames`, `--frame-ms`, `--size WxH`, `--dpi 0.25..4`, `--text-placeholders` | ヘッドレス実行（上記） |

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
  ui/                sonus_tool_ui: Tela の宣言を作る層（OS 非依存）
    app              Tela の Runtime、画面の宣言、ポインタの振り分け
    mixer_view, profiler_view, status_view   画面の宣言
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

**ヘッダ**: 画面の切り替え（Mixer / Profiler）、プロジェクト名、接続の状態（ランプ: 緑 = 接続、黄 = 接続中・ビルド不一致で
ツールの値を送った、赤 = 拒否）、レート・ブロック長・往復時間、Connect / Disconnect。**フッタ**: 最新のメッセージと、
送信待ち・Ack 待ち・拒否された値の数。

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
- ショートカット: Ctrl+Z（macOS は Cmd+Z）取り消し、Ctrl+Y / Ctrl+Shift+Z やり直し、F2 ミキサー、F3 プロファイラ。

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
重複していれば `g` + GUID、GUID も重複していれば `n` + 通し番号）から作る。名前が重複したプロジェクトはビルドできないが、
ツールは開き、ヘッダにエラー数、フッタにメッセージを出す。

**その他**: `VoiceInfo.isVirtual` は 0 以外を「仮想」とする真偽値。パラメータ上書きの Revert は上書きを無効にし、ツール上の値も
バンクの既定に戻す（再度 Override すると既定値から始まる）。受け付ける DPI は 0.25〜4（`--dpi` と `App::SetViewport` で同じ）。
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
  コンストラクタと `synchronize()` の例外を境界で捕まえる（D-56）。
- テーマ: Tela の `Theme`（panel / text / button / pressed）から色を作る（`palette.hpp`）。区分の色（メータ、ミュート、ソロ、
  クリップ）だけ固定の色相をテーマのパネル色に寄せて使う。

## テスト

| テスト | 内容 |
| --- | --- |
| `tool_model_test` | プロジェクトの読み込み（木の順、GUID、dsp の表、エラー）、ビルドハッシュ、フェーダー・ミュート・ソロの規則、セルの宛先、取り消し・やり直し・併合、上書きの一覧と Revert、オフライン、記録ファイル、ピークホールド |
| `tool_live_mixer_test` | 本物の core とライブリンクのサーバ（メモリ転送）。フェーダー・マスター・ミュート（完全な無音）・ソロ・取り消し・エフェクトのパラメータ・カテゴリが出力のレベルを解析どおりに変える、切断・再接続・`revertOnDisconnect` の後もツールの状態に戻る、ビルド不一致（送る / 断る / 知らないバスは拒否として報告）、パラメータの上書き |
| `tool_live_profiler_test` | メータ（マスター = 出力、バス別）が描いた出力と一致、処理時間・ボイス・メモリ・イベントログ、記録の保存と読み込みの往復（壊れたファイルは拒否） |
| `tool_ui_declaration_test` | ストリップの数と並び、ID の安定（フレーム・値の変更・画面の往復）、ボタンがモデルを動かす、ページ |
| `tool_ui_gesture_test` | ドラッグ量と値（端で飽和）、宣言の差し替えをまたぐドラッグ、Shift、ダブルクリック、ホイールの併合、ノブ（線形・対数）、取り消し・フォーカス喪失 |
| `tool_ui_raster_test` | Tela::Raster で描いた画素: メータの棒の高さ（DPI 1 と 2）、グラフの点と予算の線、フェーダーのつまみ、図形の上限 |
| `tool_ui_concurrency_test` | ゲームのスレッドとクライアントの受信スレッドが動く中で UI のスレッドがフレーム・ドラッグ・記録・切断を繰り返す（TSan 用） |
| `tool_ui_snapshot_test` | 接続中のツール全体を描き、マスターのメータの棒が出力のレベルの高さにあること。`SND_TOOL_SNAPSHOT_DIR` を設定すると画像を書く |

## 段階2で入れていないもの

Event / パラメータとカーブ / 3D / 音楽 / マテリアル / パッチの各エディタ、センド・スナップショット・ダッキングの編集、
構造の変更（StructEdit）とチャンクの差し替え（ReplaceChunk）、記録の再生（Replayer で同じ結果を出すのは段階5）、
接続先やファイル名の画面上での入力（Tela に文字入力が無い）。
