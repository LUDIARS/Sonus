# D-04 実デバイスの入力（platform / 構成ルートへの依頼）

依頼元: セッション D（ソース組 1、D-194）。宛先: platform の担当（本社）、api（構成ルート）。

## 背景

設計書 5 章の Input（マイク等の外部入力）は、platform に入力デバイスの IF が無いので、今はゲームが PCM を書く入力ポート
（`source/include/snd/source/input_port.hpp`、公開 API `snd_write_input`、D-194 / D-195）で作った。実デバイスから鳴らすには、
platform が取り込み（キャプチャ）デバイスを開き、そのコールバックでポートに書けばよい。source 側の口はそのまま使える。

## 依頼の中身

1. **platform の IF**（出力の `IOutputDevice` に並べる形）:
   ```cpp
   struct IInputDevice {
     virtual ~IInputDevice() = default;
     // cb は取り込みスレッドから、非インターリーブ float の 1 周期ごとに呼ばれる。
     virtual bool Open(const InputDeviceDesc&, InputCallback cb, void* user) = 0;
     virtual bool Start() = 0;
     virtual void Stop() = 0;
     virtual void Close() = 0;
     virtual std::uint32_t SampleRate() const = 0;
     virtual std::uint32_t Channels() const = 0;
     virtual void SetEventListener(DeviceEventFn fn, void* user) = 0;  // 変更・喪失
   };
   using InputCallback = void (*)(void* user, const float* const* in, std::uint32_t channels, std::uint32_t frames);
   ```
   バックエンドは出力と同じ並び（ALSA の capture、WASAPI の capture client、Core Audio の input unit / AVAudioSession の
   record カテゴリ）。iOS / Android はマイクの許可が要る。
2. **構成ルート（api）**: `snd_config` に入力デバイスの指定（使うデバイスの数と、`device_index` → OS のデバイスの対応）を足し、
   `snd_create` で開いて、コールバックで `source::Runtime::InputPortOf(device_index)->Write(in, channels, frames)` を呼ぶ。
   開いたデバイスのレートは `InputPort::Configure(rate, 0)` でポートに伝える（core がそのレートでリサンプルするのは依頼 D-03）。
   書き手はポートごとに 1 つ（取り込みスレッド）なので、ゲームが同じポートに `snd_write_input` で書かないことを決まりにする
   （デバイスに割り当てたポートとゲームが書くポートを分ける）。
3. **デバイスの喪失・変更**: 取り込みが止まるとポートが空になり、Input のボイスは無音（`starved`）で鳴り続ける。
   再び開いたら書き始めればよく、source 側の扱いは要らない（読む側は溜まり過ぎたら古い方を捨てて追い付く）。

## source 側で用意済みのもの

- ポートは書き手 1・読み手 1 のロックフリーのリングで、取り込みスレッドから直接 `Write` を呼べる（確保もロックも無い）。
- 並行のテスト `SourceInput.WriterAndReaderThreads`（TSan で繰り返し）と、C API を通した `SourceInputApi.WriteInputFeedsAnInputPlayback`。
