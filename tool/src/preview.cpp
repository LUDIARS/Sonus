// The audition router (design.md 11.1 試聴), group P.
#include "snd/tool/preview.hpp"

#include <algorithm>

#include "snd/livelink/client.hpp"
#include "snd/tool/connection.hpp"

namespace snd::tool {
namespace ll = livelink;

namespace {
constexpr const char* kGameNoMove = "the game: an audition stays at its start position (no live emitter control)";
constexpr const char* kGameNoListener = "the game: the listener is the game's (not set by the tool)";
}  // namespace

bool AuditionRouter::Ready(std::string* why) const {
  std::string w;
  bool ok = false;
  if (target_ == AuditionTarget::Preview) {
    ok = preview_ != nullptr;
    if (!ok) w = "no preview engine";
  } else {
    ok = game_ != nullptr && game_->client() != nullptr && game_->state() == LinkState::Online;
    if (!ok) w = "not connected to a game";
  }
  if (why != nullptr) *why = w;
  return ok;
}

AuditionResult AuditionRouter::Begin() const {
  AuditionResult r;
  r.target = target_;
  std::string why;
  if (!Ready(&why)) r.error = why;
  return r;
}

AuditionResult AuditionRouter::PlayEvent(const AuditionRequest& q) {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  if (q.eventId == 0u) {
    r.error = "no event";
    return r;
  }
  if (target_ == AuditionTarget::Preview) {
    if (q.atPosition) preview_->SetListener(q.listener);
    EmitterPose e;
    e.position = Vec3f{q.position[0], q.position[1], q.position[2]};
    r.playback = preview_->Play(q.eventId, q.atPosition ? &e : nullptr);
    r.ok = r.playback != 0u;
    if (!r.ok) r.error = "the preview engine refused the event";
    if (r.ok) playing_.push_back(r.playback);
    return r;
  }
  ll::Client* c = game_->client();
  r.seq = c->Audition(q.eventId, q.atPosition ? q.position : nullptr, false, true);
  r.ok = r.seq != 0u;
  if (!r.ok) r.error = "could not send the audition";
  if (q.atPosition) r.unsupported.push_back(kGameNoListener);
  return r;
}

AuditionResult AuditionRouter::Start3D(const AuditionRequest& q) {
  AuditionResult r = PlayEvent(q);
  if (r.ok && target_ == AuditionTarget::Preview) moving_ = r.playback;
  if (r.ok && target_ == AuditionTarget::Game) r.unsupported.push_back(kGameNoMove);
  return r;
}

AuditionResult AuditionRouter::Update3D(const ListenerPose& listener, const EmitterPose& emitter) {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  if (target_ == AuditionTarget::Game) {
    r.unsupported.push_back(kGameNoMove);
    r.unsupported.push_back(kGameNoListener);
    return r;
  }
  if (moving_ == 0u) {
    r.error = "no 3D audition";
    return r;
  }
  r.playback = moving_;
  r.ok = preview_->SetListener(listener) && preview_->MoveEmitter(moving_, emitter);
  if (!r.ok) r.error = "the audition ended";
  return r;
}

AuditionResult AuditionRouter::PlayTransition(const TransitionAudition& t) {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  if (t.musicEvent == 0u || t.from == 0u || t.to == 0u) {
    r.error = "no music event or state";
    return r;
  }
  if (target_ == AuditionTarget::Preview) {
    const std::uint64_t start = preview_->NextBlock();
    r.playback = preview_->Play(t.musicEvent, nullptr);
    if (r.playback == 0u) {
      r.error = "the preview engine refused the music event";
      return r;
    }
    playing_.push_back(r.playback);
    const PreviewStatus st = preview_->Status();
    const double rate = st.sampleRate == 0u ? 48000.0 : static_cast<double>(st.sampleRate);
    const double frames = st.blockFrames == 0u ? 256.0 : static_cast<double>(st.blockFrames);
    // The request lands in the block that holds its sample (requests are applied at block starts).
    const auto atBlock = start + static_cast<std::uint64_t>(t.requestSeconds * rate / frames);
    r.ok = preview_->SetMusicState(r.playback, t.from, start) && preview_->SetMusicState(r.playback, t.to, atBlock);
    if (!r.ok) r.error = "the preview engine refused the state";
    musicPlaybacks_.emplace_back(t.musicEvent, r.playback);
    return r;
  }
  // Game: the audition plays, the state overrides switch when the messages arrive.
  ll::Client* c = game_->client();
  r.seq = c->Audition(t.musicEvent, nullptr, false, true);
  const std::uint32_t s1 = c->OverrideState(t.musicEvent, t.from, true, false);
  r.ok = r.seq != 0u && s1 != 0u;
  if (!r.ok) r.error = "could not send the audition";
  r.unsupported.push_back(
      "the game: the switch to the target state is asked when the tool sends it (SetState), not at an exact "
      "block; the rule's timing applies from there");
  return r;
}

AuditionResult AuditionRouter::SetParam(Id param, float value) {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  if (target_ == AuditionTarget::Preview) {
    r.ok = preview_->SetParam(param, value);
    if (!r.ok) r.error = "the preview engine refused the parameter";
    return r;
  }
  r.seq = game_->client()->OverrideParam(param, value, true, true);
  r.ok = r.seq != 0u;
  if (!r.ok) r.error = "could not send the override";
  return r;
}

AuditionResult AuditionRouter::ReleaseParam(Id param) {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  if (target_ == AuditionTarget::Preview) {
    r.ok = true;  // the engine's own value stays (no override layer for the engine's API)
    r.unsupported.push_back("the preview engine: the parameter keeps the last value set");
    return r;
  }
  r.seq = game_->client()->OverrideParam(param, 0.0f, false, true);
  r.ok = r.seq != 0u;
  if (!r.ok) r.error = "could not send the release";
  return r;
}

AuditionResult AuditionRouter::SetState(Id musicEvent, Id state) {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  if (target_ == AuditionTarget::Preview) {
    for (const auto& [ev, pb] : musicPlaybacks_) {
      if (ev != musicEvent) continue;
      r.ok = preview_->SetMusicState(pb, state, 0) || r.ok;
      r.playback = pb;
    }
    if (!r.ok) r.error = "no music audition of that event";
    return r;
  }
  r.seq = game_->client()->OverrideState(musicEvent, state, true, true);
  r.ok = r.seq != 0u;
  if (!r.ok) r.error = "could not send the state";
  return r;
}

AuditionResult AuditionRouter::StopAll() {
  AuditionResult r = Begin();
  if (!r.error.empty()) return r;
  moving_ = 0;
  if (target_ == AuditionTarget::Preview) {
    preview_->StopAll();
    playing_.clear();
    musicPlaybacks_.clear();
    r.ok = true;
    return r;
  }
  ll::Client* c = game_->client();
  if (!c->Supports(ll::MessageType::Audition) || c->PeerMinor() < 1u) {
    r.unsupported.push_back("the game speaks protocol 1.0: no stop-all of auditions (1.1)");
    return r;
  }
  r.seq = c->StopAuditions(true);
  r.ok = r.seq != 0u;
  if (!r.ok) r.error = "could not send the stop";
  return r;
}

}  // namespace snd::tool
