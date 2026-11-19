// Compiled as C++17 with exceptions (the parent's settings): snd/sonus.h must be clean here.
static_assert(__cplusplus == 201703L, "the embedding test's game must be compiled as C++17");
#if !defined(__cpp_exceptions)
#error "the parent compiles with exceptions; Sonus' -fno-exceptions must not reach it"
#endif

#include "embed_game.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include "snd/sonus.h"

namespace embed {
namespace {

// A silent output device written against the C table of snd/ext.h (what an engine
// with its own audio output would do). The host pumps it from its thread.
struct NullDevice {
  snd_device_callback cb = nullptr;
  void* cbUser = nullptr;
  std::uint32_t rate = 48000, channels = 2, period = 256;
  bool started = false;
  bool silent = true;
  std::uint64_t pumped = 0;
  std::vector<float> buffer;
};

std::int32_t SND_CALL Open(void* u, const snd_device_desc* desc, snd_device_callback cb, void* cbUser) {
  NullDevice* d = static_cast<NullDevice*>(u);
  if (cb == nullptr) return 0;
  if (desc->sample_rate != 0u) d->rate = desc->sample_rate;
  if (desc->channels != 0u) d->channels = desc->channels;
  if (desc->period_frames != 0u) d->period = desc->period_frames;
  d->cb = cb;
  d->cbUser = cbUser;
  d->buffer.assign(std::size_t{d->period} * d->channels, 0.0f);
  return 1;
}
std::int32_t SND_CALL Start(void* u) {
  static_cast<NullDevice*>(u)->started = true;
  return 1;
}
void SND_CALL Stop(void* u) { static_cast<NullDevice*>(u)->started = false; }
void SND_CALL Close(void* u) {
  NullDevice* d = static_cast<NullDevice*>(u);
  d->started = false;
  d->cb = nullptr;
  d->cbUser = nullptr;
}
std::uint32_t SND_CALL PeriodFrames(void* u) { return static_cast<NullDevice*>(u)->period; }
std::uint32_t SND_CALL SampleRate(void* u) { return static_cast<NullDevice*>(u)->rate; }
std::uint32_t SND_CALL Channels(void* u) { return static_cast<NullDevice*>(u)->channels; }
double SND_CALL Latency(void*) { return 0.01; }
void SND_CALL SetListener(void*, snd_device_event_fn, void*) {}

void Pump(NullDevice* d) {
  if (!d->started || d->cb == nullptr) return;
  d->buffer.assign(d->buffer.size(), 1.0f);  // the callback must overwrite all of it
  d->cb(d->cbUser, d->buffer.data(), d->period, d->channels);
  for (float v : d->buffer) {
    if (v != 0.0f) d->silent = false;
  }
  ++d->pumped;
}

}  // namespace

struct Game {
  NullDevice device;
  snd_output_device table{};
  snd_system* sys = nullptr;
};

int StartGame(std::uint16_t livelinkPort, Game** out) {
  *out = nullptr;
  Game* g = new Game;
  g->table.user = &g->device;
  g->table.open = &Open;
  g->table.start = &Start;
  g->table.stop = &Stop;
  g->table.close = &Close;
  g->table.period_frames = &PeriodFrames;
  g->table.sample_rate = &SampleRate;
  g->table.channels = &Channels;
  g->table.output_latency_seconds = &Latency;
  g->table.set_event_listener = &SetListener;

  snd_config cfg;
  snd_config_init(&cfg);
  cfg.output.device = &g->table;
  cfg.livelink.enabled = livelinkPort != 0u ? 1u : 0u;
  cfg.livelink.port = livelinkPort;
  const snd_result r = snd_create(&cfg, nullptr, nullptr, &g->sys);
  if (r != SND_OK) {
    delete g;
    return static_cast<int>(r);
  }
  *out = g;
  return 0;
}

std::uint64_t RunGame(Game* g, std::uint64_t blocks) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  snd_stats stats{};
  for (;;) {
    snd_update(g->sys);
    Pump(&g->device);
    if (snd_get_stats(g->sys, &stats) != SND_OK) return 0;
    if (stats.block_index >= blocks || std::chrono::steady_clock::now() > end) return stats.block_index;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool OutputWasSilent(const Game* g) { return g->device.silent; }
std::uint64_t PeriodsPumped(const Game* g) { return g->device.pumped; }

void StopGame(Game* g) {
  if (g == nullptr) return;
  snd_destroy(g->sys);
  delete g;
}

}  // namespace embed
