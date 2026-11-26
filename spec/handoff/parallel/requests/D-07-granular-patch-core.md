# D-07 合成クリップのパラメータ配線（core・bank・buildtool）

現在 core/src/sys_event.cpp の SynthKind::Granular / Patch は params を nullptr にする。種別はこの呼び出しを invalid slot / NONE で拒否する。

1. SYNP の既存列 0〜8 を保存し、任意列案 9 density (F32、既定 20 粒/秒)、10 lengthMs (F32、100)、11 position (F32、0)、12 pitchJitterCents (F32、0)、13 window (U32、0 Hann/1 triangle/2 rectangle)、14 positionJitter (F32、0)、15 pitchCents (F32、0)、16 gateFrames (U64、0) を追加する。古い列の無いバンクでは既定値を使う。buildtool は synth の同名の JSON 属性を検査して書く。Granular の asset は既存の SYNP.asset を用いる。
2. core/src/bank_registry.cpp の oNoise の後に、SYNP 行数の GranularParams / PatchParams を追加して 64 バイト境界に揃える。登録中のバンクレコードにポインタを保管する。Granular の asset は ASST の範囲内、常駐 PCMF/PC16、適正チャンネル・レート・バイト数に限る。Patch は PTCH の存在とスライス範囲を減算で検査し、読み込み時に形式を検証する。依存方向は D-06 の規則を契約ヘッダに移す等で保つ。プログラムと素材を声の終了までバンクのエポックで保護する。遅れた描画が残る声は解放しない。
3. core/src/sys_event.cpp の SYNP switch で検証済みの記述子を渡す。Granular はレート取得用に ASST の sample_rate を VoiceStatic.sourceRate に設定する（SourceStartResult.reserved にも内部レートとの差が返る）。Render 側の元レート種別として既存のリサンプラで処理し、仮想にも素材/内部レート込みの比率を一度だけ渡す。Patch は内部レート。現在の core の Start 失敗の通知を保つ。
4. D-01 の CurveTarget 案に GrainDensity、GrainLengthMs、GrainPosition、GrainPitchJitterCents を加える。core/src/sys_params.cpp で算出したブロック両端の加算量を、core/src/sys_render.cpp の直列リスト組立で grainDensityOffset、grainLengthMsOffset、grainPositionOffset、grainPitchJitterCentsOffset へ写す。未指定は 0。ワーカーはテーブルを参照しない。単位は各 SYNP 列と同じ。
5. 統合試験で 24 kHz 素材の 48 kHz 出力（仮想の復帰を含む）、各曲線、バンク解放・再登録、壊れた PTCH 範囲、D-06 の JSON からのオシレーターとゲインを検査する。
