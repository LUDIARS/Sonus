// The C API entry checks are the shared rule table (D-84, D-93): for each
// recording function, a value is refused by the C API exactly when
// RuleOf(type).api refuses the payload the command would carry. Checked on both
// sides of every rule (accepted extremes and refused values), plus the order of
// the checks (handle 0 first: SND_ERR_INVALID_HANDLE) and the 1.2 functions.
#include <cmath>
#include <cstring>
#include <limits>

#include "api_rig.hpp"
#include "snd/internal/commands.hpp"
#include "snd/livelink/command_check.hpp"

using namespace apitest;
namespace cmd = snd::cmd;
namespace ll = snd::livelink;
using CT = cmd::CommandType;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

template <class P>
bool Table(const snd_config& c, CT type, const P& p) {
  const ll::CommandRule* r = ll::RuleOf(static_cast<std::uint16_t>(type));
  if (r == nullptr) return false;
  const ll::CommandLimits k = ll::LimitsOf(c);
  return r->api(reinterpret_cast<const std::uint8_t*>(&p), k);
}

snd_when W(std::uint8_t kind, std::uint8_t flags = 0) {
  snd_when w{};
  w.kind = kind;
  w.flags = flags;
  return w;
}

}  // namespace

// A handle that is not 0 but stale passes the entry rule and is then refused by core
// (SND_ERR_INVALID_HANDLE); a refused rule gives SND_ERR_INVALID_ARGUMENT. Stale handles
// keep these checks free of side effects.
bool Accepted(snd_result r) { return r != SND_ERR_INVALID_ARGUMENT; }
const snd_playback_handle kStalePb{SND_HANDLE_MAKE(5u, 0x1234u)};
const snd_transport_handle kStaleTr{SND_HANDLE_MAKE(5u, 0x1234u)};

SND_TEST(ApiEntryRules, WhenKindAndFlagsAgreeWithTheTable) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_config c = SmallConfig();
  const snd::HandleRef ph = snd::HandleRef::FromValue(kStalePb.value);
  const snd::HandleRef th = snd::HandleRef::FromValue(kStaleTr.value);
  for (std::uint8_t kind = 0; kind < 12; ++kind) {
    for (std::uint8_t flags : {std::uint8_t{0}, std::uint8_t{SND_WHEN_FLAG_PICKUP_ALIGN}, std::uint8_t{2}}) {
      snd_when w = W(kind, flags);
      w.transport = kStaleTr;
      cmd::Play pc{};
      pc.when = w;
      const bool ok = Table(c, CT::Play, pc);
      SND_CHECK_EQ(ok, kind <= SND_WHEN_DELAY_US && flags != 2);  // the table's rule itself
      SND_CHECK_EQ(snd_play(r.sys, 0x1234u, snd_emitter_handle{0}, &w).value != 0u, ok);
      SND_CHECK_EQ(snd_play_one_shot(r.sys, 0x1234u, snd_emitter_handle{0}, &w) == SND_OK, ok);
      const snd_vec3 pos{1, 2, 3};
      SND_CHECK_EQ(snd_play_at(r.sys, 0x1234u, &pos, &w) == SND_OK, ok);
      SND_CHECK_EQ(Accepted(snd_set_scheduled_stop(r.sys, kStalePb, &w, 0.0f)),
                   Table(c, CT::SetScheduledStop, cmd::SetScheduledStop{ph, 0.0f, 0u, w}));
      SND_CHECK_EQ(Accepted(snd_set_music_state(r.sys, kStalePb, 1u, &w)),
                   Table(c, CT::SetMusicState, cmd::SetMusicState{ph, 1u, 0u, w}));
      SND_CHECK_EQ(Accepted(snd_set_tempo(r.sys, kStaleTr, 100.0f, &w)),
                   Table(c, CT::SetTempo, cmd::SetTempo{th, 100.0f, 0u, w}));
      SND_CHECK_EQ(Accepted(snd_set_transport_pitch(r.sys, kStaleTr, 1.5f, &w)),
                   Table(c, CT::SetTempo, cmd::SetTempo{th, 1.5f, cmd::kTempoVariantPitch, w}));
      SND_CHECK_EQ(Accepted(snd_play_stinger(r.sys, kStalePb, 7u, &w)),
                   Table(c, CT::SetMusicState, cmd::SetMusicState{ph, 7u, cmd::kMusicStateVariantStinger, w}));
      const snd_musical_time mt{1, 0, 0};
      SND_CHECK_EQ(Accepted(snd_seek_transport(r.sys, kStaleTr, &mt, &w)),
                   Table(c, CT::SeekTransport, cmd::SeekTransport{th, mt, 0u, w}));
      SND_CHECK_EQ(Accepted(snd_set_transport_state(r.sys, kStaleTr, SND_TRANSPORT_PLAYING, &w)),
                   Table(c, CT::SetTransportState, cmd::SetTransportState{th, SND_TRANSPORT_PLAYING, 0u, w}));
      SND_CHECK_EQ(Accepted(snd_stop(r.sys, kStalePb, -1.0f, &w)), Table(c, CT::Stop, cmd::Stop{ph, -1.0f, 0u, w}));
    }
    r.Step(4);  // the plays end (not loaded) and give their slots back
  }
}

SND_TEST(ApiEntryRules, ValuesAgreeWithTheTable) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_config c = SmallConfig();
  const float values[] = {kNan, kInf, -kInf, -1.0f, -0.0f, 0.0f, 1e-30f, 0.5f, 1.0f, 2.0e6f, 1e30f};
  snd_transport_desc td{120.0f, 4, 4, 1, 0};
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  const snd_emitter_handle e = snd_create_emitter(r.sys);
  SND_REQUIRE(t.value != 0u && e.value != 0u);
  const snd::HandleRef th = snd::HandleRef::FromValue(t.value);
  const snd::HandleRef ph = snd::HandleRef::FromValue(kStalePb.value);
  for (const float v : values) {
    for (const float fade : {0.0f, -1.0f, 5000.0f, kInf, kNan}) {
      const float rec = fade < 0 ? 0 : fade;  // core records the fade clamped to >= 0
      const bool fadeOk = std::isfinite(fade);  // the API refuses a non-finite fade (the rule's "finite fade")
      SND_CHECK_EQ(snd_set_bus_volume(r.sys, 1u, v, fade) == SND_OK,
                   fadeOk && Table(c, CT::SetBusVolume, cmd::SetBusVolume{1u, v, rec, 0u}));
      SND_CHECK_EQ(snd_set_category_volume(r.sys, 1u, v, fade) == SND_OK,
                   fadeOk && Table(c, CT::SetCategoryVolume, cmd::SetCategoryVolume{1u, v, rec, 0u}));
      SND_CHECK_EQ(snd_apply_snapshot(r.sys, 1u, v, fade) == SND_OK,
                   fadeOk && Table(c, CT::ApplySnapshot, cmd::ApplySnapshot{1u, v, rec, 0u}));
    }
    cmd::SetParam sp{};
    sp.value = v;
    const bool param = Table(c, CT::SetParam, sp);
    SND_CHECK_EQ(snd_set_param_global(r.sys, 2u, v) == SND_OK, param);
    SND_CHECK_EQ(snd_set_param_emitter(r.sys, e, 2u, v) == SND_OK, param);
    SND_CHECK_EQ(Accepted(snd_set_param_playback(r.sys, kStalePb, 2u, v)), param);
    SND_CHECK_EQ(snd_set_tempo(r.sys, t, v, nullptr) == SND_OK,
                 Table(c, CT::SetTempo, cmd::SetTempo{th, v, cmd::kTempoVariantBpm, W(0)}));
    SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t, v, nullptr) == SND_OK,
                 Table(c, CT::SetTempo, cmd::SetTempo{th, v, cmd::kTempoVariantPitch, W(0)}));
    SND_CHECK_EQ(Accepted(snd_stop(r.sys, kStalePb, v, nullptr)), Table(c, CT::Stop, cmd::Stop{ph, v, 0u, W(0)}));
    snd_transport_desc d2{v, 4, 4, 0, 0};
    const snd_transport_handle t2 = snd_create_transport(r.sys, &d2);
    SND_CHECK_EQ(t2.value != 0u, Table(c, CT::CreateTransport, cmd::CreateTransport{snd::HandleRef{}, d2}));
    if (t2.value != 0u) snd_destroy_transport(r.sys, t2);
    r.Step(4);
  }
  // Time signatures.
  for (std::uint16_t unit = 0; unit <= 17; ++unit) {
    for (std::uint16_t bpb : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{100}}) {
      snd_transport_desc d2{120.0f, bpb, unit, 0, 0};
      const snd_transport_handle t2 = snd_create_transport(r.sys, &d2);
      SND_CHECK_EQ(t2.value != 0u, Table(c, CT::CreateTransport, cmd::CreateTransport{snd::HandleRef{}, d2}));
      if (t2.value != 0u) snd_destroy_transport(r.sys, t2);
      r.Step(4);  // the slot comes back once the audio side released it (D-46)
    }
  }
  // Listener index, unload modes, transport states.
  for (std::uint32_t i = 0; i < 6; ++i) {
    snd_listener_desc l{};
    SND_CHECK_EQ(snd_set_listener(r.sys, i, &l) == SND_OK, Table(c, CT::SetListener, cmd::SetListener{i, 0u, l}));
  }
  for (snd_transport_state s = 0; s < 5; ++s) {
    SND_CHECK_EQ(snd_set_transport_state(r.sys, t, s, nullptr) == SND_OK,
                 Table(c, CT::SetTransportState, cmd::SetTransportState{th, s, 0u, W(0)}));
  }
  // Language names (D-50).
  const char* names[] = {"ja", "en-US", "", "a/b", "a.b", "a\\b", "0123456789012345678901234567890",
                         "01234567890123456789012345678901"};
  for (const char* n : names) {
    cmd::DeclareLanguage dl{};
    const std::size_t len = std::strlen(n);
    dl.nameBytes = static_cast<std::uint32_t>(len);
    if (len < sizeof(dl.name)) std::memcpy(dl.name, n, len);
    const bool table = len < sizeof(dl.name) && Table(c, CT::DeclareLanguage, dl);
    SND_CHECK_MSG((snd_declare_language(r.sys, n) == SND_OK) == table, "%s", n);
  }
  SND_CHECK_EQ(snd_declare_language(r.sys, nullptr), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(ApiEntryRules, HandleZeroIsRefusedFirst) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_playback_handle p0{0};
  const snd_transport_handle t0{0};
  const snd_when bad = W(200);
  SND_CHECK_EQ(snd_stop(r.sys, p0, kNan, &bad), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_tempo(r.sys, t0, kNan, &bad), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t0, 1.0f, nullptr), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_play_stinger(r.sys, p0, 1u, nullptr), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_param_playback(r.sys, p0, 1u, kNan), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_unload_bank(r.sys, snd_bank_handle{0}, 7u), SND_ERR_INVALID_HANDLE);
  // A stale (never issued) handle passes the entry rule, then core finds it stale.
  const snd_playback_handle stale{SND_HANDLE_MAKE(3u, 77u)};
  SND_CHECK_EQ(snd_stop(r.sys, stale, 0.0f, nullptr), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_stop(r.sys, stale, kNan, nullptr), SND_ERR_INVALID_ARGUMENT);  // the rule before staleness
  // 1.2: stinger id 0 is refused (the table's rule for variant Stinger).
  const snd_playback_handle p = snd_play(r.sys, 1u, snd_emitter_handle{0}, nullptr);
  SND_CHECK_EQ(snd_play_stinger(r.sys, p, 0u, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(!Table(SmallConfig(), CT::SetMusicState,
                   cmd::SetMusicState{snd::HandleRef::FromValue(p.value), 0u, cmd::kMusicStateVariantStinger, W(0)}));
  SND_CHECK(Table(SmallConfig(), CT::SetMusicState,
                  cmd::SetMusicState{snd::HandleRef::FromValue(p.value), 0u, cmd::kMusicStateVariantState, W(0)}));
  SND_CHECK_EQ(snd_play_stinger(r.sys, p, 5u, nullptr), SND_OK);
  // sys == NULL.
  SND_CHECK_EQ(snd_set_transport_pitch(nullptr, t0, 1.0f, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_play_stinger(nullptr, p, 1u, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_update(nullptr), SND_ERR_INVALID_ARGUMENT);
  snd_stats st{};
  SND_CHECK_EQ(snd_get_stats(nullptr, &st), SND_ERR_INVALID_ARGUMENT);
}

// The replay's shape rule refuses the variants 1.2 does not define (commands.hpp).
SND_TEST(ApiEntryRules, ShapeRuleKnowsTheOneTwoVariants) {
  const ll::CommandLimits k = ll::LimitsOf(SmallConfig());
  const ll::CommandRule* tempo = ll::RuleOf(static_cast<std::uint16_t>(CT::SetTempo));
  const ll::CommandRule* music = ll::RuleOf(static_cast<std::uint16_t>(CT::SetMusicState));
  SND_REQUIRE(tempo != nullptr && music != nullptr);
  for (std::uint32_t v = 0; v < 4; ++v) {
    cmd::SetTempo st{snd::HandleRef{1u, 1u}, 120.0f, v, W(0)};
    SND_CHECK_EQ(tempo->shape(reinterpret_cast<const std::uint8_t*>(&st), k), v <= 1u);
    cmd::SetMusicState ms{snd::HandleRef{1u, 1u}, 5u, v, W(0)};
    SND_CHECK_EQ(music->shape(reinterpret_cast<const std::uint8_t*>(&ms), k), v <= 1u);
  }
}
