// Independent tests of the physics module (spec/handoff/README.md 3): a rig on top of the
// implementer's phys_rig.hpp (system on the manual device, observer recorder, counting
// allocator) with
//   - a bank built in the test with the buildtool (3D one-shot / looping DC events, a
//     materialTable -> PHYS),
//   - a bank writer for hand-made PHYS chunks (corruption tests),
//   - a reference model of the 9.4 culling written from the specification only
//     (decision D of the acceptance side), used against random contact streams.
// The property list is physics/test/INDEP_PROPERTIES.md.
#ifndef SND_PHYSICS_TEST_INDEP_PHYS_RIG_HPP
#define SND_PHYSICS_TEST_INDEP_PHYS_RIG_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "mini_project.hpp"
#include "phys_rig.hpp"
#include "snd/buildtool/bank_writer.hpp"
#include "snd/format/bank_format.hpp"

namespace indepphys {

using phystest::Contact;
using phystest::P;
using phystest::Rec;
using phystest::Recorder;
using phystest::Sys;

inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
inline constexpr float kInf = std::numeric_limits<float>::infinity();

// ---------------------------------------------------------------------------
// Bank built with the buildtool.
//   I/Dc     3D one-shot, mono DC 0.5, 0.25 s
//   I/Dc2D   2D one-shot, same asset
//   I/Loop   3D, looping mono DC 0.5 (loopCount 0)
//   I/Short  3D one-shot, 256 frames (ends by itself quickly)
//   I/Table  materialTable: materials m0, m1, m2
//            single m0: impact I/Dc, slide I/Loop, roll I/Loop, break I/Dc
//            single m1: impact I/Dc2D, slide I/Short
//            pair (m0, m2): I/Dc2D
//            footsteps m1: I/Dc2D
//            defaults: impact I/Dc, slide I/Loop
//            curves m0: impulseToVelocity {0.2, 2, 20, 200}, massToPitchCents {-1200, 1200},
//                       speedToVolumeDb {-20, 0}
//            (m1, m2 get the buildtool default {0.1, 1, 10, 100}, {0, 0}, {-30, 0})
// ---------------------------------------------------------------------------
inline const std::vector<std::uint8_t>& TestBank() {
  static std::vector<std::uint8_t> bytes;
  if (!bytes.empty()) return bytes;
  mini::Project proj("phys-indep");
  proj.Add("banks/Phys.json", "bank", "Phys");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  snd::buildtool::PcmData dc;
  dc.channels = 1;
  dc.sampleRate = 48000;
  dc.frames = 12000;
  dc.samples.assign(12000, 0.5f);
  proj.Wav("audio/dc.wav", dc, snd::buildtool::WavSampleFormat::Float32);
  snd::buildtool::PcmData sh = dc;
  sh.frames = 256;
  sh.samples.assign(256, 0.5f);
  proj.Wav("audio/short.wav", sh, snd::buildtool::WavSampleFormat::Float32);
  proj.Add("assets/dc.json", "asset", "dc", "\"file\": \"audio/dc.wav\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("assets/dcl.json", "asset", "dcl",
           "\"file\": \"audio/dc.wav\", \"loop\": {\"start\": 0, \"end\": 12000}, "
           "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("assets/short.json", "asset", "short",
           "\"file\": \"audio/short.wav\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("events/Dc.json", "event", "I/Dc", "\"bank\": \"Phys\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"dc\"}]}]");
  proj.Add("events/Dc2D.json", "event", "I/Dc2D", "\"bank\": \"Phys\", \"tracks\": [{\"clips\": [{\"asset\": \"dc\"}]}]");
  proj.Add("events/Loop.json", "event", "I/Loop",
           "\"bank\": \"Phys\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"dcl\", \"loopCount\": 0}]}]");
  proj.Add("events/Short.json", "event", "I/Short",
           "\"bank\": \"Phys\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"short\"}]}]");
  proj.Add("tables/T.json", "materialTable", "I/Table",
           "\"bank\": \"Phys\", \"materials\": [\"m0\", \"m1\", \"m2\"],\n"
           "  \"single\": {\"m0\": {\"impact\": \"I/Dc\", \"slide\": \"I/Loop\", \"roll\": \"I/Loop\", \"break\": \"I/Dc\"},\n"
           "             \"m1\": {\"impact\": \"I/Dc2D\", \"slide\": \"I/Short\"}},\n"
           "  \"pairs\": [{\"a\": \"m2\", \"b\": \"m0\", \"event\": \"I/Dc2D\"}],\n"
           "  \"footsteps\": {\"m1\": \"I/Dc2D\"},\n"
           "  \"defaults\": {\"impact\": \"I/Dc\", \"slide\": \"I/Loop\"},\n"
           "  \"curves\": {\"m0\": {\"impulseToVelocity\": [0.2, 2, 20, 200], \"massToPitchCents\": [-1200, 1200],\n"
           "                      \"speedToVolumeDb\": [-20, 0]}}");
  snd::buildtool::Diagnostics diag;
  snd::buildtool::BuildResult res;
  if (!proj.Build(&diag, &res)) {
    snd::test::Note("bank build failed:\n%s", mini::Dump(diag).c_str());
    return bytes;
  }
  bytes = mini::ReadBytes(proj.out() / "Phys.sndb");
  return bytes;
}

// A bank made of one PHYS blob (and nothing else), for hand-made tables.
inline std::vector<std::uint8_t> PhysOnlyBank(const std::vector<std::uint8_t>& phys, std::uint32_t bankId = 0x1234u) {
  snd::buildtool::BankWriter w;
  w.AddBlob(snd::bank_format::chunk::PHYS, phys);
  snd::buildtool::BankHeaderInfo h;
  h.bankId = bankId;
  return w.Finish(h);
}

// A PHYS chunk with one table of N materials, everything laid out after the table array.
struct PhysBuilder {
  std::uint32_t id = 0x77u;
  std::uint32_t n = 2;
  std::vector<std::uint32_t> single;  // [n*4]
  std::vector<std::uint32_t> pair;    // [n*n]
  std::vector<std::uint32_t> foot;    // [n]
  std::uint32_t def[4] = {0, 0, 0, 0};
  std::vector<snd::bank_format::PhysCurves> curves;  // [n]
  explicit PhysBuilder(std::uint32_t count = 2) : n(count) {
    single.assign(std::size_t{n} * 4u, 0u);
    pair.assign(std::size_t{n} * n, 0u);
    foot.assign(n, 0u);
    curves.assign(n, snd::bank_format::PhysCurves{{0.1f, 1.0f, 10.0f, 100.0f}, {0.0f, 0.0f}, {-30.0f, 0.0f}});
  }
  // Offsets of the arrays inside the produced chunk (for corruption).
  std::uint64_t tableAt = 16, singleAt = 0, pairAt = 0, footAt = 0, curveAt = 0;
  std::vector<std::uint8_t> Build() {
    std::vector<std::uint8_t> b(16 + 64, 0);
    auto put = [&b](const void* p, std::size_t bytes) {
      while (b.size() % 8u != 0u) b.push_back(0);
      const std::uint64_t at = b.size();
      b.resize(b.size() + bytes);
      if (bytes != 0) std::memcpy(b.data() + at, p, bytes);
      return at;
    };
    singleAt = put(single.data(), single.size() * 4u);
    pairAt = put(pair.data(), pair.size() * 4u);
    footAt = put(foot.data(), foot.size() * 4u);
    curveAt = put(curves.data(), curves.size() * sizeof(snd::bank_format::PhysCurves));
    snd::bank_format::PhysChunkHeader h{1u, 0u, 16u};
    std::memcpy(b.data(), &h, sizeof h);
    snd::bank_format::PhysTable t{};
    t.id = id;
    t.materialCount = n;
    t.singleOffset = singleAt;
    t.pairOffset = pairAt;
    t.footstepOffset = footAt;
    for (int k = 0; k < 4; ++k) t.defaultEvent[k] = def[k];
    t.curveOffset = curveAt;
    std::memcpy(b.data() + 16, &t, sizeof t);
    return b;
  }
};

// Default listener at the origin, facing +z, up +y.
inline void SetListener(snd_system* sys) {
  snd_listener_desc l{};
  l.forward = snd_vec3{0.0f, 0.0f, 1.0f};
  l.up = snd_vec3{0.0f, 1.0f, 0.0f};
  l.weight = 1.0f;
  l.enabled = 1u;
  snd_set_listener(sys, 0, &l);
}

// Mean of a channel over the last `n` captured frames.
inline double TailMean(const Sys& s, int ch, std::uint64_t n) {
  const std::uint64_t f = s.Frames();
  if (f < n) return 0.0;
  double sum = 0.0;
  for (std::uint64_t i = f - n; i < f; ++i) sum += ch == 0 ? s.L(i) : s.R(i);
  return sum / static_cast<double>(n);
}

// Config for the culling tests: observer on, default culling.
inline snd_physics_config Cfg(Recorder* rec) {
  snd_physics_config c;
  snd_physics_config_init(&c);
  c.observer = rec != nullptr ? &Recorder::Fn : nullptr;
  c.observer_user = rec;
  return c;
}

// A table of n materials where every material has single events for every kind.
inline snd_id SEv(std::uint32_t m, std::uint32_t k) { return 0x5000u + m * 16u + k; }

// ---------------------------------------------------------------------------
// Reference model of 9.4 (Impact / Break one-shots only), from decision D:
//   time in whole us, threshold (< drops, = passes, NaN / negative invalid, Break exempt),
//   grouping by (floor(p/g) per axis, unordered material pair, kind), representative = max
//   impulse first in array, cooldown key (unordered objects, unordered materials, kind) with
//   >= passing and recording only actual requests, budget floor(carry + bps*dt) with only
//   the fraction carried, Break first then impulse descending (array order on ties).
// Event resolution of the model: single events only (`hasSingle[m]`); a group whose two
// materials have no single event resolves to nothing (no default, no pair).
// ---------------------------------------------------------------------------
struct ModelPlay {
  snd_id event;
  std::uint32_t kind;
  std::uint32_t count;
  float impulse;
  std::uint64_t objA, objB;
  float pos[3];
};

struct ModelStats {
  std::uint64_t received = 0, invalid = 0, threshold = 0, merged = 0, groups = 0, noEvent = 0, cooldown = 0,
                budget = 0, requests = 0, reqImpact = 0, reqBreak = 0, evictions = 0;
};

struct CullModel {
  float threshold = 0.0f;
  float grid = 0.5f;
  std::uint64_t cooldownUs = 50000;
  double bps = 300.0;
  std::uint64_t maxDtUs = 100000;
  std::uint32_t cooldownCap = 256;
  std::vector<bool> hasSingle;  // per material

  std::uint64_t now = 0;
  double carry = 0.0;
  struct CdRec {
    std::uint64_t a = 0, b = 0;
    std::uint32_t ma = 0, mb = 0, kind = 0;
    std::uint64_t last = 0;
    bool used = false;
    bool thisCall = false;
  };
  std::vector<CdRec> cd;  // fixed slots, in order of first use
  ModelStats st;

  static std::uint64_t Us(double s) { return static_cast<std::uint64_t>(std::llround(s * 1e6)); }

  snd_id EventOf(std::uint16_t m, std::uint32_t k) const { return SEv(m, k); }

  // Returns false for a rejected dt.
  bool Submit(const std::vector<snd_contact>& cs, float dt, std::vector<ModelPlay>* plays) {
    if (!std::isfinite(dt) || dt < 0.0f) return false;
    std::uint64_t dtUs = Us(static_cast<double>(dt));
    if (dtUs > maxDtUs) dtUs = maxDtUs;
    now += dtUs;
    for (CdRec& x : cd) x.thisCall = false;
    const double slotsF = carry + bps * (static_cast<double>(dtUs) / 1e6);
    std::uint64_t slots = static_cast<std::uint64_t>(std::floor(slotsF));
    carry = slotsF - std::floor(slotsF);
    struct G {
      std::int64_t cx, cy, cz;
      std::uint32_t lo, hi, kind;
      std::size_t rep;
      float impulse;
      std::uint32_t count;
    };
    std::vector<G> gs;
    for (std::size_t i = 0; i < cs.size(); ++i) {
      const snd_contact& c = cs[i];
      ++st.received;
      if (c.kind != SND_CONTACT_IMPACT && c.kind != SND_CONTACT_BREAK) continue;  // model: one-shots only
      if (!std::isfinite(c.impulse) || c.impulse < 0.0f || !std::isfinite(c.position[0]) ||
          !std::isfinite(c.position[1]) || !std::isfinite(c.position[2])) {
        ++st.invalid;
        continue;
      }
      if (c.kind == SND_CONTACT_IMPACT && c.impulse < threshold) {
        ++st.threshold;
        continue;
      }
      const std::int64_t cx = static_cast<std::int64_t>(std::floor(c.position[0] / grid));
      const std::int64_t cy = static_cast<std::int64_t>(std::floor(c.position[1] / grid));
      const std::int64_t cz = static_cast<std::int64_t>(std::floor(c.position[2] / grid));
      const std::uint32_t lo = std::min(c.material_a, c.material_b), hi = std::max(c.material_a, c.material_b);
      bool found = false;
      for (G& g : gs) {
        if (g.cx == cx && g.cy == cy && g.cz == cz && g.lo == lo && g.hi == hi && g.kind == c.kind) {
          ++g.count;
          ++st.merged;
          if (c.impulse > g.impulse) {
            g.impulse = c.impulse;
            g.rep = i;
          }
          found = true;
          break;
        }
      }
      if (!found) {
        ++st.groups;
        gs.push_back(G{cx, cy, cz, lo, hi, c.kind, i, c.impulse, 1u});
      }
    }
    // Drop groups without an event (before cooldown and budget).
    std::vector<G> live;
    for (const G& g : gs) {
      const snd_contact& r = cs[g.rep];
      const bool any = hasSingle[r.material_a] || hasSingle[r.material_b];
      if (!any) {
        ++st.noEvent;
        continue;
      }
      live.push_back(g);
    }
    // Step 3 against the table as it was before this call (the steps run in order: the
    // cooldown decision comes before any request of this call is recorded).
    auto cdFind = [&](std::uint64_t oa, std::uint64_t ob, const G& g) -> CdRec* {
      for (CdRec& x : cd) {
        if (x.used && x.a == oa && x.b == ob && x.ma == g.lo && x.mb == g.hi && x.kind == g.kind) return &x;
      }
      return nullptr;
    };
    std::vector<G> cand;
    for (const G& g : live) {
      const snd_contact& r = cs[g.rep];
      const CdRec* hit = cdFind(std::min(r.object_a, r.object_b), std::max(r.object_a, r.object_b), g);
      if (hit != nullptr && now - hit->last < cooldownUs) {
        ++st.cooldown;
        continue;
      }
      cand.push_back(g);
    }
    std::stable_sort(cand.begin(), cand.end(), [](const G& x, const G& y) {
      const bool bx = x.kind == SND_CONTACT_BREAK, by = y.kind == SND_CONTACT_BREAK;
      if (bx != by) return bx;
      if (x.impulse != y.impulse) return x.impulse > y.impulse;
      return x.rep < y.rep;
    });
    for (const G& g : cand) {
      const snd_contact& r = cs[g.rep];
      const std::uint64_t oa = std::min(r.object_a, r.object_b), ob = std::max(r.object_a, r.object_b);
      CdRec* hit = cdFind(oa, ob, g);
      // Undecided 7: the same cooldown key requested earlier in this call (another cell).
      if (cooldownUs > 0 && hit != nullptr && hit->last == now && hit->thisCall) {
        ++st.cooldown;
        continue;
      }
      if (slots == 0) {
        ++st.budget;
        continue;
      }
      --slots;
      ++st.requests;
      // Record the cooldown. Full table: the oldest record goes; on equal times the
      // implementer's tie rule is the lowest slot (records live in fixed slots).
      if (hit != nullptr) {
        hit->last = now;
        hit->thisCall = true;
      } else {
        std::size_t slot = cd.size();
        if (cd.size() >= cooldownCap) {
          slot = 0;
          for (std::size_t j = 1; j < cd.size(); ++j) {
            if (cd[j].last < cd[slot].last) slot = j;
          }
          if (now - cd[slot].last < cooldownUs) ++st.evictions;
        } else {
          cd.push_back(CdRec{});
        }
        cd[slot] = CdRec{oa, ob, g.lo, g.hi, g.kind, now, true, true};
      }
      auto emit = [&](std::uint16_t m) {
        ModelPlay p{EventOf(m, g.kind), g.kind, g.count, g.impulse, r.object_a, r.object_b,
                    {r.position[0], r.position[1], r.position[2]}};
        plays->push_back(p);
        if (g.kind == SND_CONTACT_BREAK) {
          ++st.reqBreak;
        } else {
          ++st.reqImpact;
        }
      };
      if (hasSingle[r.material_a]) emit(r.material_a);
      if (r.material_b != r.material_a && hasSingle[r.material_b]) emit(r.material_b);
    }
    return true;
  }
};

// Small deterministic generator (xorshift64*).
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1u) {}
  std::uint64_t Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 2685821657736338717ull;
  }
  std::uint32_t Below(std::uint32_t n) { return static_cast<std::uint32_t>(Next() % n); }
  float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1u << 24); }
};

}  // namespace indepphys

#endif  // SND_PHYSICS_TEST_INDEP_PHYS_RIG_HPP
