// The preview engine (design.md 11.1 試聴), group P.
#include "snd/tool/preview/preview_engine.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "runtime_host.hpp"
#include "snd/tool/doc_build.hpp"

namespace snd::tool::preview {
namespace ll = livelink;

snd_config PreviewConfig() {
  snd_config c;
  snd_config_init(&c);
  c.livelink.enabled = 1;
  c.jobs.realtime_workers = 0;
  c.jobs.normal_workers = 1;
  c.max_real_voices = 64;
  c.max_virtual_voices = 256;
  c.max_playbacks = 256;
  c.max_emitters = 64;
  c.max_streams = 8;
  c.max_banks = 32;
  c.max_registered_threads = 4;
  c.max_voice_channels = 2;
  c.memory.stream_pool_bytes = 2ull << 20;
  c.memory.pcm_cache_bytes = 4ull << 20;
  c.tuning.lod_down_blocks = 0;  // 2.5: the preview renders what the project says, no load LOD
  return c;
}

std::unique_ptr<PreviewEngine> PreviewEngine::Create(const PreviewEngineDesc& desc, std::string* error) {
  std::unique_ptr<PreviewEngine> e(new PreviewEngine());
  e->desc_ = desc;
  e->host_ = std::make_unique<RuntimeHost>();
  RuntimeHostDesc hd;
  hd.config = desc.config;
  hd.hooks = desc.liveLink ? HostHooks::Live : HostHooks::None;
  hd.structEditor = desc.structEditor;
  hd.chunkSlots = desc.chunkSlots;
  hd.output = desc.output == PreviewOutput::Device   ? HostOutput::Device
              : desc.output == PreviewOutput::Silent ? HostOutput::Silent
                                                     : HostOutput::Manual;
  if (e->host_->Create(hd, error) != SND_OK) return nullptr;
  if (desc.liveLink) {
    ll::ServerDesc sd;
    sd.revertOnDisconnect = desc.revertOnDisconnect;
    sd.heartbeatMs = 1000;
    const snd_result r = e->host_->StartServer(sd);
    if (r != SND_OK) {
      if (error != nullptr) *error = "the live link server did not start";
      return nullptr;
    }
  }
  return e;
}

PreviewEngine::~PreviewEngine() {
  if (host_ != nullptr) host_->Destroy();
}

const snd_config& PreviewEngine::config() const { return host_->config(); }
core::System* PreviewEngine::system() { return host_->system(); }
ll::Server* PreviewEngine::server() { return host_->server(); }
ll::IConnector* PreviewEngine::connector() { return desc_.liveLink ? host_->connector() : nullptr; }

bool PreviewEngine::PutFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  if (path.empty()) return false;
  host_->fs().Put(path, bytes);
  return true;
}

std::string PreviewEngine::PutRebuiltBank(const std::string& file, const std::vector<std::uint8_t>& bytes) {
  const std::string path = "live/" + std::to_string(++reloadSerial_) + "/" + file;
  host_->fs().Put(path, bytes);
  return path;
}

bool PreviewEngine::LoadBuild(const DocBuild& build, std::string* error) {
  auto fail = [&](const std::string& m) {
    error_ = m;
    if (error != nullptr) *error = m;
    return false;
  };
  if (!build.ok) return fail("the build has errors");
  // The previous banks go first (their playbacks stop with a short fade).
  StopAll();
  for (auto it = banks_.rbegin(); it != banks_.rend(); ++it) host_->UnloadBank(snd_bank_handle{*it}, desc_.bankTimeoutMs);
  banks_.clear();
  ++buildSerial_;
  // Dependencies first.
  std::vector<const BankImage*> order;
  std::set<Id> done;
  std::string skipped;
  std::vector<const BankImage*> left;
  for (const BankImage& b : build.banks) {
    if (!b.language.empty()) {
      skipped += (skipped.empty() ? "" : ", ") + b.file;
      continue;
    }
    left.push_back(&b);
  }
  while (!left.empty()) {
    bool progress = false;
    for (std::size_t i = 0; i < left.size();) {
      const ParsedBank* v = left[i]->view();
      bool ready = true;
      for (std::uint32_t d = 0; v != nullptr && d < v->dependencyCount(); ++d) ready = ready && done.count(v->dependencyBankId(d)) != 0u;
      if (ready) {
        order.push_back(left[i]);
        done.insert(left[i]->bankId);
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
        progress = true;
      } else {
        ++i;
      }
    }
    if (!progress) {  // a dependency the build does not have: load in order and let the runtime say
      for (const BankImage* b : left) order.push_back(b);
      left.clear();
    }
  }
  for (const BankImage* b : order) {
    const std::string path = "build/" + std::to_string(buildSerial_) + "/" + b->file;
    host_->fs().Put(path, b->bytes);
    std::string err;
    const snd_bank_handle h = host_->LoadBank(path, desc_.bankTimeoutMs, &err);
    if (h.value == 0u) return fail(err);
    banks_.push_back(h.value);
  }
  buildHash_ = build.buildHash;
  error_.clear();
  if (!skipped.empty()) {
    error_ = "language banks not loaded: " + skipped;
    if (error != nullptr) *error = error_;
  }
  return true;
}

std::uint64_t PreviewEngine::Play(Id event, const EmitterPose* emitter) {
  core::System* s = host_->system();
  snd_emitter_handle em{0};
  if (emitter != nullptr) {
    em = s->CreateEmitter();
    if (em.value == 0u) {
      error_ = "no emitter slot";
      return 0;
    }
    const snd_vec3 p{emitter->position.x, emitter->position.y, emitter->position.z};
    const snd_vec3 f{emitter->forward.x, emitter->forward.y, emitter->forward.z};
    const snd_vec3 v{emitter->velocity.x, emitter->velocity.y, emitter->velocity.z};
    s->SetTransform(em, &p, &f, &v);
  }
  const snd_playback_handle h = s->Play(event, em, nullptr);
  if (h.value == 0u) {
    if (em.value != 0u) s->DestroyEmitter(em);
    error_ = "the event did not start";
    return 0;
  }
  playing_.push_back(Playing{h.value, em.value});
  return h.value;
}

bool PreviewEngine::MoveEmitter(std::uint64_t playback, const EmitterPose& pose) {
  for (const Playing& p : playing_) {
    if (p.playback != playback || p.emitter == 0u) continue;
    const snd_vec3 pos{pose.position.x, pose.position.y, pose.position.z};
    const snd_vec3 f{pose.forward.x, pose.forward.y, pose.forward.z};
    const snd_vec3 v{pose.velocity.x, pose.velocity.y, pose.velocity.z};
    return host_->system()->SetTransform(snd_emitter_handle{p.emitter}, &pos, &f, &v) == SND_OK;
  }
  return false;
}

bool PreviewEngine::SetListener(const ListenerPose& pose) {
  snd_listener_desc d{};
  d.position = snd_vec3{pose.position.x, pose.position.y, pose.position.z};
  d.forward = snd_vec3{pose.forward.x, pose.forward.y, pose.forward.z};
  d.up = snd_vec3{pose.up.x, pose.up.y, pose.up.z};
  d.weight = 1.0f;
  d.enabled = 1;
  return host_->system()->SetListener(0, &d) == SND_OK;
}

bool PreviewEngine::SetParam(Id param, float value) { return host_->system()->SetParamGlobal(param, value) == SND_OK; }

bool PreviewEngine::SetMusicState(std::uint64_t playback, Id state, std::uint64_t atBlock) {
  if (playback == 0u || state == 0u) return false;
  if (atBlock <= NextBlock()) {
    return host_->system()->SetMusicState(snd_playback_handle{playback}, state, nullptr) == SND_OK;
  }
  scheduled_.push_back(Scheduled{atBlock, playback, state});
  return true;
}

void PreviewEngine::RunScheduled(std::uint64_t upTo) {
  for (std::size_t i = 0; i < scheduled_.size();) {
    if (scheduled_[i].block <= upTo) {
      host_->system()->SetMusicState(snd_playback_handle{scheduled_[i].playback}, scheduled_[i].state, nullptr);
      scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void PreviewEngine::Forget(std::uint64_t playback) {
  for (std::size_t i = 0; i < playing_.size(); ++i) {
    if (playing_[i].playback != playback) continue;
    if (playing_[i].emitter != 0u) host_->system()->DestroyEmitter(snd_emitter_handle{playing_[i].emitter});
    playing_.erase(playing_.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

bool PreviewEngine::Stop(std::uint64_t playback) {
  const float fade = host_->config().tuning.short_fade_ms / 1000.0f;
  const bool ok = host_->system()->Stop(snd_playback_handle{playback}, fade, nullptr) == SND_OK;
  for (std::size_t i = 0; i < scheduled_.size();) {
    if (scheduled_[i].playback == playback) {
      scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  Forget(playback);
  return ok;
}

std::uint32_t PreviewEngine::StopAll() {
  std::uint32_t n = 0;
  const std::vector<Playing> all = playing_;
  for (const Playing& p : all) n += Stop(p.playback) ? 1u : 0u;
  scheduled_.clear();
  return n;
}

std::uint64_t PreviewEngine::NextBlock() const { return host_->BlocksRendered(); }

std::uint32_t PreviewEngine::Render(std::uint32_t blocks, std::vector<float>* out) {
  if (host_->output() != HostOutput::Manual) return 0;
  const std::size_t n = std::size_t{host_->BlockFrames()} * host_->Channels();
  std::vector<float> block(n);
  for (std::uint32_t i = 0; i < blocks; ++i) {
    RunScheduled(NextBlock());
    host_->Step(block.data());
    if (out != nullptr) out->insert(out->end(), block.begin(), block.end());
  }
  // Playbacks that ended are forgotten (their emitters go back).
  for (std::size_t i = 0; i < playing_.size();) {
    const snd_playback_state st = host_->system()->GetPlaybackState(snd_playback_handle{playing_[i].playback});
    if (st == SND_PLAYBACK_STATE_ENDED || st == SND_PLAYBACK_STATE_NONE) {
      Forget(playing_[i].playback);
    } else {
      ++i;
    }
  }
  return blocks;
}

void PreviewEngine::Pump() {
  if (host_->output() == HostOutput::Manual) return;
  // Device / Silent: a scheduled state goes out once its block is near (render-ahead blocks are
  // already rendered: the switch is not block exact in real time).
  RunScheduled(NextBlock() + host_->config().render_ahead_blocks + 1u);
  host_->Pump();
}

std::vector<float> PreviewEngine::RecentOutput(std::uint32_t blocks) const { return host_->RecentOutput(blocks); }

std::vector<PreviewTransition> PreviewEngine::Transitions() const {
  std::vector<PreviewTransition> out;
  music::MusicTransitionRecord recs[64];
  const std::uint32_t n = host_->music().TransitionLog(recs, 64u);
  for (std::uint32_t i = 0; i < n && i < 64u; ++i) {
    PreviewTransition t;
    t.from = recs[i].from;
    t.to = recs[i].to;
    t.clock = recs[i].clock;
    t.tick = static_cast<std::int64_t>(recs[i].tick);
    out.push_back(t);
  }
  return out;
}

PreviewStatus PreviewEngine::Status() const {
  PreviewStatus s;
  s.running = host_->created();
  s.output = desc_.output;
  s.sampleRate = host_->SampleRate();
  s.blockFrames = host_->BlockFrames();
  s.channels = host_->Channels();
  s.block = NextBlock();
  s.banks = static_cast<std::uint32_t>(banks_.size());
  s.buildHash = buildHash_;
  s.activePlaybacks = static_cast<std::uint32_t>(playing_.size());
  s.underruns = host_->Underruns();
  s.error = error_;
  return s;
}

}  // namespace snd::tool::preview
