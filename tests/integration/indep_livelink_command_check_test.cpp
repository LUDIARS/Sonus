// Independent tests of command_check (acceptance decision 5): every recorded command
// goes through the game API's entry rules before a replay hands it to core.
//  - boundary values of each command type (valid just inside, refused just outside);
//  - commands the game API accepted are never left out of a replay.
#include <cmath>
#include <limits>
#include <string>

#include "indep_livelink_common.hpp"
#include "snd/livelink/command_check.hpp"

using namespace indep;
using lltest::Fixture;

namespace {

template <class P>
std::vector<std::uint8_t> Cmd(cmd::CommandType t, const P& payload) {
  const std::uint32_t size = cmd::kCommandInfo[static_cast<std::uint16_t>(t)].wireSize;
  std::vector<std::uint8_t> b(size, 0);
  const cmd::CommandHeader h{static_cast<std::uint16_t>(t), static_cast<std::uint16_t>(size), 0u};
  std::memcpy(b.data(), &h, sizeof(h));
  std::memcpy(b.data() + sizeof(h), &payload, sizeof(P));
  return b;
}

bool Ok(const std::vector<std::uint8_t>& b, const ll::CommandLimits& k) {
  cmd::CommandHeader h;
  std::memcpy(&h, b.data(), sizeof(h));
  return ll::CheckCommand(h, b.data() + sizeof(h), k);
}

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

}  // namespace

SND_TEST(IndepLivelinkCommandCheck, BoundariesOfEveryCheckedCommand) {
  const snd_config cfg = lltest::Config();
  const ll::CommandLimits k = ll::LimitsOf(cfg);
  using CT = cmd::CommandType;
  const snd::HandleRef pb{cfg.max_playbacks - 1u, 1u};
  // Play: handle shape, emitter, flags, position, priority, When.
  cmd::Play p{};
  p.playback = pb;
  p.event = HashId("Tone");
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.playback = snd::HandleRef{cfg.max_playbacks, 1u};
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.playback = snd::HandleRef{0u, 0u};
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.playback = snd::HandleRef{0u, 0x7FFFFFFFu};
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.playback = snd::HandleRef{0u, 0x80000000u};
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.playback = pb;
  p.emitter = snd::HandleRef{cfg.max_emitters - 1u, 3u};
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.emitter = snd::HandleRef{cfg.max_emitters, 3u};
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.emitter = {};
  p.flags = cmd::kPlayOneShot | cmd::kPlayHasPosition;
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.flags = 4u;
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.flags = 0;
  p.position = snd::Vec3{0, kNan, 0};
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.position = {};
  p.priorityHint = 255u;
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.priorityHint = 256u;
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.priorityHint = 0;
  p.when.kind = SND_WHEN_DELAY_US;
  p.when.value = 3600ull * 1000000ull;
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.when.value += 1u;
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.when = snd_when{};
  p.when.kind = 9u;
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.when = snd_when{};
  p.when.reserved[5] = 1u;
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.when = snd_when{};
  p.when.kind = SND_WHEN_GRID;
  p.when.count = 256u;
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.when.count = 257u;
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  p.when = snd_when{};
  p.when.kind = SND_WHEN_NEXT_BAR;
  p.when.transport.value = SND_HANDLE_MAKE(cfg.max_transports - 1u, 1u);
  SND_CHECK(Ok(Cmd(CT::Play, p), k));
  p.when.transport.value = SND_HANDLE_MAKE(cfg.max_transports, 1u);
  SND_CHECK(!Ok(Cmd(CT::Play, p), k));
  // Stop: fade < 0 is "event default"; up to an hour; finite.
  cmd::Stop s{pb, -1.0f, 0u, snd_when{}};
  SND_CHECK(Ok(Cmd(CT::Stop, s), k));
  s.fadeSeconds = 3600.0f;
  SND_CHECK(Ok(Cmd(CT::Stop, s), k));
  s.fadeSeconds = kInf;
  SND_CHECK(!Ok(Cmd(CT::Stop, s), k));
  // Listener.
  cmd::SetListener l{};
  l.index = cfg.max_listeners - 1u;
  l.listener.enabled = 1u;
  SND_CHECK(Ok(Cmd(CT::SetListener, l), k));
  l.index = cfg.max_listeners;
  SND_CHECK(!Ok(Cmd(CT::SetListener, l), k));
  l.index = 0;
  l.listener.weight = -0.5f;
  SND_CHECK(!Ok(Cmd(CT::SetListener, l), k));
  l.listener.weight = 0;
  l.listener.enabled = 2u;
  SND_CHECK(!Ok(Cmd(CT::SetListener, l), k));
  // Parameters: scope / target agreement, finite value.
  cmd::SetParam sp{};
  sp.scope = static_cast<std::uint8_t>(snd::ParamScope::Global);
  sp.value = 0.5f;
  SND_CHECK(Ok(Cmd(CT::SetParam, sp), k));
  sp.target = pb;
  SND_CHECK(!Ok(Cmd(CT::SetParam, sp), k));
  sp.scope = static_cast<std::uint8_t>(snd::ParamScope::Playback);
  SND_CHECK(Ok(Cmd(CT::SetParam, sp), k));
  sp.value = kNan;
  SND_CHECK(!Ok(Cmd(CT::SetParam, sp), k));
  sp.value = 0;
  sp.scope = 3u;
  SND_CHECK(!Ok(Cmd(CT::SetParam, sp), k));
  // Volumes.
  cmd::SetBusVolume bv{HashId("Sfx"), 0.0f, 0.0f, 0u};
  SND_CHECK(Ok(Cmd(CT::SetBusVolume, bv), k));
  bv.volume = -0.001f;
  SND_CHECK(!Ok(Cmd(CT::SetBusVolume, bv), k));
  bv.volume = kNan;
  SND_CHECK(!Ok(Cmd(CT::SetBusVolume, bv), k));
  // Transports.
  cmd::CreateTransport ct{};
  ct.transport = snd::HandleRef{0u, 1u};
  ct.desc.bpm = 120.0f;
  ct.desc.beats_per_bar = 4;
  ct.desc.beat_unit = 4;
  SND_CHECK(Ok(Cmd(CT::CreateTransport, ct), k));
  for (std::uint16_t u : {std::uint16_t{2}, std::uint16_t{8}, std::uint16_t{16}}) {
    ct.desc.beat_unit = u;
    SND_CHECK(Ok(Cmd(CT::CreateTransport, ct), k));
  }
  for (std::uint16_t u : {std::uint16_t{0}, std::uint16_t{3}, std::uint16_t{32}, std::uint16_t{0xFFFF}}) {
    ct.desc.beat_unit = u;
    SND_CHECK(!Ok(Cmd(CT::CreateTransport, ct), k));
  }
  ct.desc.beat_unit = 4;
  ct.desc.bpm = 0.0f;
  SND_CHECK(!Ok(Cmd(CT::CreateTransport, ct), k));
  ct.desc.bpm = 120.0f;
  ct.desc.beats_per_bar = 0;
  SND_CHECK(!Ok(Cmd(CT::CreateTransport, ct), k));
  ct.desc.beats_per_bar = 4;
  ct.transport = snd::HandleRef{SND_TRANSPORT_PLAYBACK_BIT | 1u, 1u};  // a playback's transport: not creatable
  SND_CHECK(!Ok(Cmd(CT::CreateTransport, ct), k));
  cmd::SetTempo st{};
  st.transport = snd::HandleRef{SND_TRANSPORT_PLAYBACK_BIT | 1u, 1u};
  st.bpm = 90.0f;
  SND_CHECK(Ok(Cmd(CT::SetTempo, st), k));
  st.bpm = -1.0f;
  SND_CHECK(!Ok(Cmd(CT::SetTempo, st), k));
  cmd::SeekTransport sk{};
  sk.transport = snd::HandleRef{0u, 1u};
  sk.position.tick = static_cast<std::int32_t>(SND_TICKS_PER_BEAT) - 1;
  SND_CHECK(Ok(Cmd(CT::SeekTransport, sk), k));
  sk.position.tick = static_cast<std::int32_t>(SND_TICKS_PER_BEAT);
  SND_CHECK(!Ok(Cmd(CT::SeekTransport, sk), k));
  sk.position.tick = 0;
  sk.position.bar = -1;
  SND_CHECK(!Ok(Cmd(CT::SeekTransport, sk), k));
  // Live commands.
  cmd::LiveSetValue lv{};
  lv.field = ll::kFieldRevertAll;
  SND_CHECK(Ok(Cmd(CT::LiveSetValue, lv), k));
  lv.objectId = 5u;
  SND_CHECK(!Ok(Cmd(CT::LiveSetValue, lv), k));
  lv.field = ll::EncodeField(bf::chunk::BUSS, bf::col::BUSS_volumeDb);
  SND_CHECK(Ok(Cmd(CT::LiveSetValue, lv), k));
  lv.valueType = 4u;
  SND_CHECK(!Ok(Cmd(CT::LiveSetValue, lv), k));
  lv.valueType = 0;
  lv.field = 0x7FFF0000u;
  SND_CHECK(!Ok(Cmd(CT::LiveSetValue, lv), k));
  cmd::LiveOverride lo{};
  lo.kind = static_cast<std::uint32_t>(cmd::LiveOverrideKind::Switch);
  lo.enable = 1u;
  SND_CHECK(Ok(Cmd(CT::LiveOverride, lo), k));
  lo.kind = 3u;
  SND_CHECK(!Ok(Cmd(CT::LiveOverride, lo), k));
  // Languages and banks.
  cmd::DeclareLanguage dl{};
  dl.nameBytes = 2;
  dl.name[0] = 'e';
  dl.name[1] = 'n';
  SND_CHECK(Ok(Cmd(CT::DeclareLanguage, dl), k));
  dl.name[1] = '/';
  SND_CHECK(!Ok(Cmd(CT::DeclareLanguage, dl), k));
  dl.name[1] = 'n';
  dl.name[2] = 'x';  // not NUL terminated at nameBytes
  SND_CHECK(!Ok(Cmd(CT::DeclareLanguage, dl), k));
  cmd::BankUnload bu{snd::HandleRef{0u, 1u}, SND_UNLOAD_IF_UNUSED, 0u};
  SND_CHECK(Ok(Cmd(CT::BankUnload, bu), k));
  bu.mode = 2u;
  SND_CHECK(!Ok(Cmd(CT::BankUnload, bu), k));
  // Never replayable, and size mismatches.
  SND_CHECK(!Ok(Cmd(CT::BankRegister, cmd::BankRegister{}), k));
  SND_CHECK(!Ok(Cmd(CT::LiveReplaceChunk, cmd::LiveReplaceChunk{}), k));
  std::vector<std::uint8_t> wrong = Cmd(CT::SetParam, sp);
  wrong[2] = static_cast<std::uint8_t>(wrong[2] + 8u);
  SND_CHECK(!Ok(wrong, k));
  // Overflow parameters.
  cmd::OverflowParam op{};
  op.scope = static_cast<std::uint8_t>(snd::ParamScope::Global);
  std::memcpy(&op.value, &kInf, 4);
  op.used = 1u;
  SND_CHECK(!ll::CheckOverflowParam(op, k));
  op.isSwitch = 1u;  // a switch value is an id: any bits
  SND_CHECK(ll::CheckOverflowParam(op, k));
}

namespace {
// Records game API calls on a System with the live runtime (no server) and returns the
// recording (the runtime's record ring, end marker removed).
std::vector<std::uint8_t> RecordApiCalls(void (*calls)(Fixture&)) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.Steps(4);
  f.rt->RequestRecording(true);
  f.Step();
  calls(f);
  f.Steps(4);
  f.rt->RequestRecording(false);
  f.Steps(2);
  std::vector<std::uint8_t> out;
  std::uint32_t n = 0;
  while (f.rt->records().Next(&n)) {
    std::vector<std::uint8_t> m(n);
    f.rt->records().Copy(0, m.data(), n);
    f.rt->records().Pop();
    std::uint32_t kind = 0;
    std::memcpy(&kind, m.data(), 4);
    if (kind == ll::kRecordEndMarker) continue;
    out.insert(out.end(), m.begin(), m.end());
  }
  return out;
}

ll::ReplayInfo ReplayInfoOf(const std::vector<std::uint8_t>& rec) {
  const snd_config cfg = lltest::Config();
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.data();
  rd.bytes = rec.size();
  ll::Replayer* rp = nullptr;
  ll::ReplayInfo info{};
  if (ll::Replayer::Create(rd, &rp) == SND_OK) info = rp->Info();
  ll::Replayer::Destroy(rp);
  return info;
}

std::string Invalid(const ll::ReplayInfo& i) {
  std::string s;
  for (std::uint16_t t = 1; t < static_cast<std::uint16_t>(cmd::CommandType::Count); ++t) {
    if (i.invalidByType[t] != 0u) {
      s += cmd::kCommandInfo[t].name;
      s += "x" + std::to_string(i.invalidByType[t]) + " ";
    }
  }
  return s;
}
}  // namespace

// Ordinary API use with ordinary values: nothing is left out.
SND_TEST(IndepLivelinkCommandCheck, OrdinaryApiCommandsAreKept) {
  const std::vector<std::uint8_t> rec = RecordApiCalls([](Fixture& f) {
    const snd_emitter_handle e = f.sys->CreateEmitter();
    const snd_vec3 pos{1, 2, 3}, fwd{0, 0, 1}, vel{0, 0, 0};
    f.sys->SetTransform(e, &pos, &fwd, &vel);
    const snd_playback_handle a = f.sys->Play(HashId("Tone"), e, nullptr);
    f.sys->PlayAt(HashId("Blip"), &pos, nullptr);
    f.sys->PlayOneShot(HashId("Blip"), snd_emitter_handle{0}, nullptr);
    f.sys->SetParamGlobal(HashId("Knob"), 0.3f);
    f.sys->SetParamEmitter(e, HashId("Knob"), 0.4f);
    f.sys->SetParamPlayback(a, HashId("Knob"), 0.5f);
    f.sys->SetSwitchGlobal(HashId("Sw"), HashId("A"));
    f.sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.1f);
    f.sys->SetCategoryVolume(HashId("Cat"), 0.7f, 0.0f);
    f.sys->PauseCategory(HashId("Cat"), true);
    f.sys->MuteCategory(HashId("Cat"), false);
    f.sys->ApplySnapshot(HashId("Snap"), 1.0f, 0.5f);
    snd_listener_desc ld{};
    ld.forward = snd_vec3{0, 0, 1};
    ld.up = snd_vec3{0, 1, 0};
    ld.enabled = 1u;
    f.sys->SetListener(0, &ld);
    snd_transport_desc td{};
    td.bpm = 128.0f;
    td.beats_per_bar = 3;
    td.beat_unit = 8;
    td.start_playing = 1;
    const snd_transport_handle t = f.sys->CreateTransport(&td);
    snd_when w{};
    w.kind = SND_WHEN_NEXT_BAR;
    w.transport = t;
    f.sys->SetTempo(t, 140.0f, nullptr);
    const snd_musical_time mt{2, 1, 10};
    f.sys->SeekTransport(t, &mt, nullptr);
    f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, &w);
    f.sys->SetTransportState(t, SND_TRANSPORT_PAUSED, nullptr);
    f.sys->Pause(a, 0.05f);
    f.sys->ResumePlayback(a, 0.05f);
    snd_when d{};
    d.kind = SND_WHEN_DELAY_US;
    d.value = 20000;
    f.sys->SetScheduledStop(a, &d, 0.1f);
    f.sys->Stop(a, -1.0f, nullptr);
    f.sys->DestroyTransport(t);
    f.sys->DestroyEmitter(e);
    f.sys->DeclareLanguage("ja");
    f.sys->SetLanguage(HashId("ja"));
  });
  SND_REQUIRE(!rec.empty());
  const ll::ReplayInfo i = ReplayInfoOf(rec);
  SND_CHECK(i.commands >= 25u);
  SND_CHECK_MSG(i.invalidCommands == 0u, "left out: %s", Invalid(i).c_str());
}

// Values the game API accepts at the edges of its own rules: a replay must keep them
// (decision 5: "the same rules as the API entry").
SND_TEST(IndepLivelinkCommandCheck, Defect_livelink_command_check_refuses_what_the_api_accepts) {
  const std::vector<std::uint8_t> rec = RecordApiCalls([](Fixture& f) {
    const snd_playback_handle a = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    SND_CHECK_EQ(f.sys->ApplySnapshot(HashId("Snap"), 1.5f, 0.0f), SND_OK);         // weight > 1
    SND_CHECK_EQ(f.sys->SetBusVolume(HashId("Sfx"), 0.5f, 5000.0f), SND_OK);       // fade > 1 h
    SND_CHECK_EQ(f.sys->SetCategoryVolume(HashId("Cat"), 2.0e6f, 0.0f), SND_OK);   // volume > 1e6
    snd_transport_desc td{};
    td.bpm = 20000.0f;  // > 10000
    td.beats_per_bar = 100;  // > 64
    td.beat_unit = 4;
    const snd_transport_handle t = f.sys->CreateTransport(&td);
    SND_CHECK(t.value != 0u);
    SND_CHECK_EQ(f.sys->SetTempo(t, 12000.0f, nullptr), SND_OK);
    SND_CHECK_EQ(f.sys->Pause(a, 5000.0f), SND_OK);  // fade > 1 h
    SND_CHECK_EQ(f.sys->Stop(a, kInf, nullptr), SND_OK);  // API refuses NaN only
  });
  SND_REQUIRE(!rec.empty());
  const ll::ReplayInfo i = ReplayInfoOf(rec);
  SND_CHECK_MSG(i.invalidCommands == 0u, "accepted by the API, left out of the replay: %s", Invalid(i).c_str());
}
