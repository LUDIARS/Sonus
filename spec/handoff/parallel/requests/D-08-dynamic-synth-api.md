# D-08 バンククリップを経由しない再生（api・core）

snd_play_synth の既存記述子は callback と user を保存し、core/src/sys_apply.cpp の PlaySynth は Callback ボイスを生成する。c_api.cpp 末尾の関数追加だけでは Granular/Patch の型と所有権を渡せない。以下を統合時の API 案とする。

- snd_play_granular(sys, const snd_granular_desc*, emitter, when)。desc は struct_size、常駐 PCM のバンク/資産ハンドル、density、length_ms、position、position_jitter、pitch_cents、pitch_jitter_cents、window、channels、loop を持つ。資産は記録から終了まで世代と参照を保持する。
- snd_play_patch(sys, const snd_patch_desc*, emitter, when)。desc は struct_size、program、program_bytes、gate_frames、channels を持つ。記録時に最大 332 バイトを検証し、システム初期化時に確保した記述子スロットへコピーしてから返す。呼出側バッファをオーディオスレッドで借りない。
- 新しいコマンド種別は追加しない。既存 PlaySynth の予約 variant に種別＋所有スロットの世代つき参照を載せ、サイズ・再生ファイルの既存解釈は保持する。ApplyCommands と RunEventLogic は variant を処理する。失敗・取消・記録破棄・終了で資産とスロットを一度だけ解放する。上限時・不正引数は無効ハンドル。生ポインタの再生ファイルへの保存は禁止。寿命・飽和・同時呼出を統合試験に含める。
