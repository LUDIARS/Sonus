// Independent live link tests: helpers shared by the indep_livelink_*.cpp files.
// Written from the specification (livelink-protocol.md, design.md 11.2/11.3) and
// the public headers; reuses the implementer's fixture (livelink_fixture.hpp) only
// for the System / memory link plumbing.
//
// IndepBank: a dev bank with optional extras for the override layer:
//   BUSS   row 0 "Sfx" (parent Master, stereo, optional limiter), row 1 "Master"
//   ASST   "dc" (mono, kDc, 4800 frames)
//   EVNT   "Tone" (Sfx, loops, category "Cat" if withCategory),
//          "Blip" (Sfx, once), "ToneG" (Sfx, loops, voice limit group "Grp" if withGroup),
//          plus `extraEvent` if non-null
//   CATG   "Cat" (0 dB) if withCategory
//   VLGR   "Grp" (maxVoices, steal oldest) if withGroup
//   EFFX   snd.fx.limiter on Sfx (CeilingDb = limiterDb, lookahead 0) if withLimiter
//   PARM   "Knob"
#ifndef SND_TESTS_INDEP_LIVELINK_COMMON_HPP
#define SND_TESTS_INDEP_LIVELINK_COMMON_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "livelink_fixture.hpp"
#include "snd/dsp/effects.hpp"

namespace indep {

namespace bf = snd::bank_format;
namespace bt = snd::buildtool;
namespace ll = snd::livelink;
namespace cmd = snd::cmd;
using snd::HashId;

struct BankOpts {
  const char* name = "Live";
  std::uint64_t hash = lltest::kProjectHash;
  bool dev = true;
  bool withCategory = false;
  bool withGroup = false;
  std::uint32_t groupMax = 4;
  bool withLimiter = false;
  float limiterDb = -12.0f;
  const char* extraEvent = nullptr;
};

inline std::vector<std::uint8_t> IndepBank(const BankOpts& o = {}) {
  namespace c = bf::col;
  bt::BankWriter w;
  bt::TableBuilder ev(bf::chunk::EVNT), tr(bf::chunk::TRAK), cl(bf::chunk::CLIP), bu(bf::chunk::BUSS),
      as(bf::chunk::ASST), pa(bf::chunk::PARM), ca(bf::chunk::CATG), vg(bf::chunk::VLGR), fx(bf::chunk::EFFX),
      fp(bf::chunk::EFPV);
  std::uint32_t r = bu.AddRow();
  bu.U32(c::BUSS_id, r, HashId("Sfx"));
  bu.U32(c::BUSS_parent, r, 1u);
  bu.U32(c::BUSS_channels, r, 2u);
  if (o.withLimiter) {
    bu.U32(c::BUSS_effectFirst, r, 0u);
    bu.U32(c::BUSS_effectCount, r, 1u);
    const std::uint32_t x = fx.AddRow();
    fx.U32(c::EFFX_effectType, x, snd::dsp::kEffectLimiter);
    fx.U32(c::EFFX_paramFirst, x, 0u);
    fx.U32(c::EFFX_paramCount, x, 4u);
    const float params[4] = {o.limiterDb, 50.0f, 0.0f, 0.0f};
    for (float p : params) {
      const std::uint32_t pr = fp.AddRow();
      fp.F32(c::EFPV_value, pr, p);
    }
  }
  r = bu.AddRow();
  bu.U32(c::BUSS_id, r, HashId("Master"));
  bu.U32(c::BUSS_parent, r, bf::kNone);
  bu.U32(c::BUSS_channels, r, 2u);
  bu.U32(c::BUSS_flags, r, bf::kBusMaster);
  std::vector<std::uint8_t> wave(4800 * sizeof(float));
  for (std::size_t i = 0; i < 4800; ++i) std::memcpy(wave.data() + i * 4, &lltest::kDc, 4);
  r = as.AddRow();
  as.U32(c::ASST_id, r, HashId("dc"));
  as.U32(c::ASST_loadForm, r, 0);
  as.U32(c::ASST_codec, r, SND_CODEC_PCM_F32);
  as.U32(c::ASST_channels, r, 1);
  as.U32(c::ASST_sampleRate, r, 48000);
  as.U64(c::ASST_totalFrames, r, 4800);
  as.U32(c::ASST_blockAlign, r, 4);
  as.U32(c::ASST_framesPerPacket, r, 1);
  as.U64(c::ASST_dataOffset, r, 0);
  as.U64(c::ASST_dataBytes, r, wave.size());
  if (o.withCategory) {
    r = ca.AddRow();
    ca.U32(c::CATG_id, r, HashId("Cat"));
  }
  if (o.withGroup) {
    r = vg.AddRow();
    vg.U32(c::VLGR_id, r, HashId("Grp"));
    vg.U32(c::VLGR_maxVoices, r, o.groupMax);
    vg.U32(c::VLGR_stealMode, r, 0u);
  }
  auto event = [&](const char* name, std::uint32_t loopCount, bool cat, bool grp) {
    const std::uint32_t e = ev.AddRow();
    ev.U32(c::EVNT_id, e, HashId(name));
    ev.U32(c::EVNT_trackFirst, e, tr.rows());
    ev.U32(c::EVNT_trackCount, e, 1u);
    ev.U32(c::EVNT_bus, e, 0u);
    if (cat) ev.U32(c::EVNT_category, e, 0u);
    if (grp) ev.U32(c::EVNT_limitGroup, e, 0u);
    const std::uint32_t t = tr.AddRow();
    tr.U32(c::TRAK_event, t, e);
    tr.U32(c::TRAK_clipFirst, t, cl.rows());
    tr.U32(c::TRAK_clipCount, t, 1u);
    const std::uint32_t k = cl.AddRow();
    cl.U32(c::CLIP_kind, k, 0u);
    cl.U32(c::CLIP_target, k, 0u);
    cl.U32(c::CLIP_loopCount, k, loopCount);
  };
  event("Tone", 0u, o.withCategory, false);
  event("Blip", 1u, false, false);
  event("ToneG", 0u, false, o.withGroup);
  if (o.extraEvent != nullptr) event(o.extraEvent, 0u, false, false);
  r = pa.AddRow();
  pa.U32(c::PARM_id, r, HashId("Knob"));
  pa.F32(c::PARM_maxValue, r, 1.0f);
  w.AddTable(ev);
  w.AddTable(tr);
  w.AddTable(cl);
  w.AddTable(bu);
  w.AddTable(as);
  w.AddBlob(bf::chunk::WAVE, wave);
  w.AddTable(pa);
  w.AddTable(ca);
  w.AddTable(vg);
  w.AddTable(fx);
  w.AddTable(fp);
  bt::BankHeaderInfo hi;
  hi.bankId = HashId(o.name);
  hi.projectBuildHash = o.hash;
  if (o.dev) hi.flags |= bf::kFileDevBuild;
  return w.Finish(hi);
}

inline std::uint64_t F32Bits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  return b;
}
inline double DbRatio(double db) { return std::pow(10.0, db / 20.0); }

// Records one LiveSetValue from the (registered) owner thread, renders one block and
// returns the result the runtime reported for it (deterministic: no live link thread).
inline ll::ApplyResult ApplyDirect(lltest::Fixture& f, snd::Id object, std::uint32_t chunk, std::uint32_t column,
                                   ll::ValueType vt, std::uint32_t index, std::uint64_t value) {
  cmd::LiveSetValue c{};
  c.objectId = object;
  c.field = ll::EncodeField(chunk, column);
  c.valueType = static_cast<std::uint32_t>(vt);
  c.index = index;
  c.value = value;
  ll::ApplyResult out{0u, 12345, 0u, 0u};
  if (f.sys->RecordLive(cmd::CommandType::LiveSetValue, &c, sizeof(c)) != SND_OK) return out;
  f.Step();
  ll::ApplyResult ar{};
  while (f.rt->results().TryPop(ar)) out = ar;
  return out;
}
inline ll::ApplyResult VolumeDirect(lltest::Fixture& f, const char* bus, float db) {
  return ApplyDirect(f, HashId(bus), bf::chunk::BUSS, bf::col::BUSS_volumeDb, ll::ValueType::F32, 0, F32Bits(db));
}
inline ll::ApplyResult RevertDirect(lltest::Fixture& f) {
  cmd::LiveSetValue c{};
  c.field = ll::kFieldRevertAll;
  ll::ApplyResult out{0u, 12345, 0u, 0u};
  if (f.sys->RecordLive(cmd::CommandType::LiveSetValue, &c, sizeof(c)) != SND_OK) return out;
  f.Step();
  ll::ApplyResult ar{};
  while (f.rt->results().TryPop(ar)) out = ar;
  return out;
}

// Mean of the left channel over the last rendered block.
inline double LeftMean(const lltest::Fixture& f) {
  const std::uint32_t ch = f.sys->OutputChannels();
  const std::size_t frames = f.out.size() / ch;
  double s = 0;
  for (std::size_t i = 0; i < frames; ++i) s += f.out[i * ch];
  return s / static_cast<double>(frames);
}

// A connected memory client on a fixture whose server is started.
struct Tool {
  lltest::Fixture& f;
  ll::Client* c = nullptr;
  explicit Tool(lltest::Fixture& fx, ll::ClientDesc d = {}) : f(fx) {
    if (d.connector == nullptr) d = lltest::MemoryClient(f.link);
    if (ll::Client::Create(d, &c) == SND_OK) c->Start();
  }
  ~Tool() { ll::Client::Destroy(c); }
  bool Ready() {
    return c != nullptr &&
           f.StepUntil([&] { return c->State() == ll::ClientState::Ready && f.server->Connected(); }, 10000);
  }
  // Ack of seq while rendering; 1 on timeout, 2 if not queued.
  std::int32_t Ack(std::uint32_t seq, std::uint32_t timeoutMs = 10000) {
    if (seq == 0u) return 2;
    std::int32_t r = 1;
    if (!f.StepUntil([&] { return c->TakeAck(seq, &r); }, timeoutMs)) return 1;
    return r;
  }
};

}  // namespace indep

#endif  // SND_TESTS_INDEP_LIVELINK_COMMON_HPP
