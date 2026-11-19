// Independent tests: snd_create's Config check and the composition root's life cycle
// (CONTRACTS.md D-44, D-91, D-92, D-93; snd/config.h, snd/sonus.h).
// Properties (written from the spec before reading the implementation):
//  A1 cfg / out NULL -> INVALID_ARGUMENT, *out NULL.
//  A2 another contract major -> VERSION_MISMATCH; any minor of the same major is accepted.
//  A3 struct_size: exactly the 1.0 size, exactly the 1.1 size, or >= the current size;
//     every other value (0, inside a field, shorter than 1.0) -> VERSION_MISMATCH.
//  A4 the tail of a short struct is not read (garbage after it changes nothing) and the
//     missing fields get the snd_config_init defaults; a 1.1-sized struct's 1.1 fields ARE read.
//  A5-A9 boundaries of max_voice_channels, max_pitch_ratio, the 1.2 capacities, the plugin
//     tables, max_registered_threads with the live link.
//  A10 every failure (including an allocation failing at any point of snd_create) leaves
//     *out NULL and gives every byte back to the game allocator; allocation failures are
//     SND_ERR_OUT_OF_BUDGET (snd/types.h: "budget cannot hold the configured limits").
//  E6 snd_destroy(NULL) is a no-op.
#include <cmath>
#include <cstdio>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <cstring>
#include <limits>
#include <map>
#include <thread>
#include <vector>

#include "config_check.hpp"
#include "indep_rig.hpp"

using apitest::SmallConfig;
using indep::CountingAllocator;
using snd::compose::kConfigSize10;
using snd::compose::kConfigSize11;

namespace {

// Creates and destroys; returns the result and checks *out / the allocator balance.
snd_result CreateOnce(const snd_config& cfgIn, CountingAllocator* a, snd::mock::NullDeviceConfig dc = {},
                      bool* outWasNull = nullptr) {
  snd::mock::NullOutputDevice dev(dc);
  snd::mock::MemoryFileSystem fs;
  snd_output_device dt = snd::ToC(dev);
  snd_file_system ft = snd::ToC(fs);
  snd_config cfg = cfgIn;
  if (cfg.struct_size >= offsetof(snd_config, output) + sizeof(snd_output_config)) cfg.output.device = &dt;
  snd_allocator at{};
  if (a != nullptr) at = a->Table();
  snd_system* sys = reinterpret_cast<snd_system*>(&cfg);  // must be overwritten
  const snd_result r = snd_create(&cfg, a != nullptr ? &at : nullptr, &ft, &sys);
  if (outWasNull != nullptr) *outWasNull = sys == nullptr;
  if (r != SND_OK) {
    SND_CHECK_MSG(sys == nullptr, "result %d left *out set", r);
    sys = nullptr;
  }
  snd_destroy(sys);
  return r;
}

snd_result Validate(const snd_config& c, bool live) { return snd::compose::ValidateConfig(c, live); }

}  // namespace

SND_TEST(IndepCreate, NullArguments) {
  snd_config c = SmallConfig();
  snd_system* sys = reinterpret_cast<snd_system*>(&c);
  SND_CHECK_EQ(snd_create(nullptr, nullptr, nullptr, &sys), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(sys == nullptr);
  SND_CHECK_EQ(snd_create(&c, nullptr, nullptr, nullptr), SND_ERR_INVALID_ARGUMENT);
  // An allocator table with a missing function is refused (nothing allocated, *out NULL).
  snd_allocator half{nullptr, &CountingAllocator::Alloc, nullptr};
  sys = reinterpret_cast<snd_system*>(&c);
  SND_CHECK_EQ(snd_create(&c, &half, nullptr, &sys), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(sys == nullptr);
  snd_destroy(nullptr);  // E6
}

// A3: sweep every 4-byte struct_size from 0 to sizeof + 64.
SND_TEST(IndepCreate, StructSizeSweep) {
  for (std::size_t size = 0; size <= sizeof(snd_config) + 64; size += 4) {
    alignas(8) unsigned char buf[sizeof(snd_config) + 128];
    snd_config c = SmallConfig();
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, &c, sizeof(c));
    auto* p = reinterpret_cast<snd_config*>(buf);
    p->struct_size = static_cast<std::uint32_t>(size);
    snd_config norm;
    const snd_result r = snd::compose::NormalizeConfig(p, &norm);
    // The 1.2 layout before the host fields (D-102 / D-103) is a known layout end too; it is
    // checked by IndepHostOutput.Defect_EarlierOneTwoStructSizeGetsTheHostDefaults.
    if (size == offsetof(snd_config, file_system_root)) continue;
    const bool want = size == kConfigSize10 || size == kConfigSize11 || size >= sizeof(snd_config);
    SND_CHECK_MSG((r == SND_OK) == want, "struct_size %llu -> %d", static_cast<unsigned long long>(size), r);
    if (!want) SND_CHECK_EQ(r, SND_ERR_VERSION_MISMATCH);
    if (want) SND_CHECK_EQ(norm.struct_size, static_cast<std::uint32_t>(sizeof(snd_config)));
  }
  // And through snd_create for the in-between sizes right after each layout end.
  for (const std::size_t size : {kConfigSize10 + 4, kConfigSize11 - 4, kConfigSize11 + 4, kConfigSize11 + 8, sizeof(snd_config) - 8}) {
    snd_config c = SmallConfig();
    c.struct_size = static_cast<std::uint32_t>(size);
    const snd_result r = CreateOnce(c, nullptr);
    SND_CHECK_MSG(r == SND_ERR_VERSION_MISMATCH, "struct_size %llu -> %d", static_cast<unsigned long long>(size), r);
  }
}

// A4: the tail of a 1.0 / 1.1 struct is not read, its own fields are.
SND_TEST(IndepCreate, ShortStructsReadOnlyTheirOwnFields) {
  alignas(8) unsigned char buf[sizeof(snd_config) + 64];
  // 1.1-sized struct with BAD 1.1 fields: they are part of it, so they are checked.
  {
    snd_config c = SmallConfig();
    c.max_voice_channels = 9;
    std::memset(buf, 0xA5, sizeof(buf));
    std::memcpy(buf, &c, kConfigSize11);
    auto* p = reinterpret_cast<snd_config*>(buf);
    p->struct_size = static_cast<std::uint32_t>(kConfigSize11);
    snd_config norm;
    SND_REQUIRE_EQ(snd::compose::NormalizeConfig(p, &norm), SND_OK);
    SND_CHECK_EQ(norm.max_voice_channels, 9u);
    SND_CHECK_EQ(Validate(norm, false), SND_ERR_INVALID_ARGUMENT);
    c.max_voice_channels = 2;
    c.max_pitch_ratio = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(buf, &c, kConfigSize11);
    p->struct_size = static_cast<std::uint32_t>(kConfigSize11);
    SND_REQUIRE_EQ(snd::compose::NormalizeConfig(p, &norm), SND_OK);
    SND_CHECK_EQ(Validate(norm, false), SND_ERR_INVALID_ARGUMENT);
  }
  // 1.1-sized struct with good 1.1 fields and an invalid 1.2 tail (0 capacities): accepted,
  // the 1.2 fields are the defaults.
  {
    snd_config c = SmallConfig();
    c.max_buses = 0;
    c.max_duck_rules = 0;
    c.max_pitch_ratio = 2.5f;
    std::memcpy(buf, &c, sizeof(c));  // the tail holds zeros (invalid if read)
    auto* p = reinterpret_cast<snd_config*>(buf);
    p->struct_size = static_cast<std::uint32_t>(kConfigSize11);
    snd_config norm;
    SND_REQUIRE_EQ(snd::compose::NormalizeConfig(p, &norm), SND_OK);
    SND_CHECK_EQ(norm.max_pitch_ratio, 2.5f);
    SND_CHECK_EQ(norm.max_buses, 128u);
    SND_CHECK_EQ(norm.max_categories, 64u);
    SND_CHECK_EQ(norm.max_snapshots, 64u);
    SND_CHECK_EQ(norm.max_limit_groups, 64u);
    SND_CHECK_EQ(norm.max_param_defs, 512u);
    SND_CHECK_EQ(norm.max_duck_rules, 64u);
    SND_CHECK_EQ(Validate(norm, false), SND_OK);
    SND_CHECK_EQ(CreateOnce(*p, nullptr), SND_OK);
  }
  // 1.0-sized struct whose tail would be invalid both as 1.1 and as 1.2 fields.
  {
    snd_config c = SmallConfig();
    c.max_voice_channels = 0;
    c.max_pitch_ratio = -std::numeric_limits<float>::infinity();
    c.max_buses = 70000;
    std::memcpy(buf, &c, sizeof(c));
    auto* p = reinterpret_cast<snd_config*>(buf);
    p->struct_size = static_cast<std::uint32_t>(kConfigSize10);
    snd_config norm;
    SND_REQUIRE_EQ(snd::compose::NormalizeConfig(p, &norm), SND_OK);
    SND_CHECK_EQ(norm.max_voice_channels, 8u);
    SND_CHECK_EQ(norm.max_pitch_ratio, 4.0f);
    SND_CHECK_EQ(norm.max_buses, 128u);
    // The caller's 1.0 values are kept.
    SND_CHECK_EQ(norm.max_real_voices, 32u);
    SND_CHECK_EQ(norm.commands.arena_bytes, 32u << 10);
    SND_CHECK(norm.output.device == p->output.device);
    SND_CHECK_EQ(CreateOnce(*p, nullptr), SND_OK);
  }
}

SND_TEST(IndepCreate, ContractVersionMajorAndMinor) {
  snd_config c = SmallConfig();
  for (const std::uint32_t minor : {0u, 1u, 2u, 0xFFFFu}) {
    c.contract_version = (SND_CONTRACT_VERSION_MAJOR << 16) | minor;
    SND_CHECK_MSG(CreateOnce(c, nullptr) == SND_OK, "minor %u", minor);
  }
  for (const std::uint32_t major : {0u, SND_CONTRACT_VERSION_MAJOR + 1u, 0xFFFFu}) {
    c.contract_version = (major << 16) | SND_CONTRACT_VERSION_MINOR;
    SND_CHECK_MSG(CreateOnce(c, nullptr) == SND_ERR_VERSION_MISMATCH, "major %u", major);
  }
}

// A5-A9 on the pure check, at both sides of every bound.
SND_TEST(IndepCreate, BoundariesOfTheApiChecks) {
  const snd_config base = SmallConfig();
  SND_REQUIRE_EQ(Validate(base, false), SND_OK);
  auto with = [&](auto f) {
    snd_config c = base;
    f(c);
    return c;
  };
  // max_voice_channels
  for (std::uint32_t v = 0; v <= 10; ++v) {
    const snd_result want = (v >= 1u && v <= 8u) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
    SND_CHECK_MSG(Validate(with([&](snd_config& c) { c.max_voice_channels = v; }), false) == want, "channels %u", v);
  }
  // max_pitch_ratio
  const float inf = std::numeric_limits<float>::infinity();
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(), inf, -inf,
                       0.0f, -0.0f, -1.0f, std::nextafter(1.0f, 0.0f), std::nextafter(32.0f, 64.0f), 33.0f,
                       std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min()};
  for (const float v : bad) {
    SND_CHECK_MSG(Validate(with([&](snd_config& c) { c.max_pitch_ratio = v; }), false) == SND_ERR_INVALID_ARGUMENT,
                  "ratio %g", static_cast<double>(v));
  }
  const float good[] = {1.0f, std::nextafter(1.0f, 2.0f), 4.0f, std::nextafter(32.0f, 0.0f), 32.0f};
  for (const float v : good) {
    SND_CHECK_MSG(Validate(with([&](snd_config& c) { c.max_pitch_ratio = v; }), false) == SND_OK, "ratio %g",
                  static_cast<double>(v));
  }
  // The 1.2 capacities, one by one.
  using Field = std::uint32_t snd_config::*;
  const Field caps[] = {&snd_config::max_buses,        &snd_config::max_categories, &snd_config::max_snapshots,
                        &snd_config::max_limit_groups, &snd_config::max_param_defs, &snd_config::max_duck_rules};
  for (const Field f : caps) {
    for (const std::uint32_t v : {0u, 65536u, 0xFFFFFFFFu}) {
      SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.*f = v; }), false), SND_ERR_INVALID_ARGUMENT);
    }
    for (const std::uint32_t v : {1u, 65535u}) {
      SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.*f = v; }), false), SND_OK);
    }
  }
  // Plugin tables: a positive count needs a table, at most 32 each.
  static snd_codec codecs[33] = {};
  static snd_dsp_effect effects[33] = {};
  SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.plugins.codec_count = 1; }), false), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.plugins.effect_count = 1; }), false), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) {
                 c.plugins.codecs = codecs;
                 c.plugins.codec_count = 32;
               }), false),
               SND_OK);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) {
                 c.plugins.codecs = codecs;
                 c.plugins.codec_count = 33;
               }), false),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) {
                 c.plugins.effects = effects;
                 c.plugins.effect_count = 32;
               }), false),
               SND_OK);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) {
                 c.plugins.effects = effects;
                 c.plugins.effect_count = 33;
               }), false),
               SND_ERR_INVALID_ARGUMENT);
  // A table with a count of 0 is fine (the pointer is ignored).
  SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.plugins.codecs = codecs; }), false), SND_OK);
  // Registered threads: <= 63 when the live link runs (it takes one more); no such bound otherwise.
  SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.max_registered_threads = 63; }), true), SND_OK);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.max_registered_threads = 64; }), true), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Validate(with([&](snd_config& c) { c.max_registered_threads = 64; }), false), SND_OK);
}

// The same boundaries reach snd_create unchanged (no other layer refuses a value the
// check accepts, and the check runs before anything is allocated).
SND_TEST(IndepCreate, BoundariesThroughCreateAllocateNothing) {
  CountingAllocator a;
  snd_config c = SmallConfig();
  c.max_pitch_ratio = std::numeric_limits<float>::quiet_NaN();
  SND_CHECK_EQ(CreateOnce(c, &a), SND_ERR_INVALID_ARGUMENT);
  c = SmallConfig();
  c.max_voice_channels = 9;
  SND_CHECK_EQ(CreateOnce(c, &a), SND_ERR_INVALID_ARGUMENT);
  c = SmallConfig();
  c.max_param_defs = 65536;
  SND_CHECK_EQ(CreateOnce(c, &a), SND_ERR_INVALID_ARGUMENT);
  c = SmallConfig();
  c.struct_size = static_cast<std::uint32_t>(kConfigSize10 + 4);
  SND_CHECK_EQ(CreateOnce(c, &a), SND_ERR_VERSION_MISMATCH);
  SND_CHECK_EQ(a.calls.load(), 0u);  // refused before the first allocation
  // Accepted extremes create (memory permitting): ratio 32, 1 voice channel, capacities 1.
  c = SmallConfig();
  c.max_pitch_ratio = 32.0f;
  c.max_voice_channels = 1;
  c.max_buses = 1;
  c.max_categories = 1;
  c.max_snapshots = 1;
  c.max_limit_groups = 1;
  c.max_param_defs = 1;
  c.max_duck_rules = 1;
  SND_CHECK_EQ(CreateOnce(c, &a), SND_OK);
  SND_CHECK_EQ(a.live.load(), 0);
}

// A10: an allocation failing at ANY point of snd_create rolls everything back. Each
// attempt runs in a child process (fork), so a crash in one roll-back path is counted
// instead of ending the test program. Without fork (Windows) the sweep is skipped.
#if defined(_WIN32)
#define SND_INDEP_FORK_SWEEP 0
#else
#define SND_INDEP_FORK_SWEEP 1
#endif
#if SND_INDEP_FORK_SWEEP
namespace {

struct Sweep {
  std::uint64_t total = 0;
  std::uint32_t crashed = 0, leaked = 0, outSet = 0, wrongCode = 0, accepted = 0;
  std::vector<std::uint64_t> crashAt, wrongAt;
  std::vector<int> wrongCodes;
};

// Child: 0 = clean failure with OUT_OF_BUDGET, 1 = leak, 2 = *out set on failure,
// 4 = accepted (SND_OK) and clean, 10 + n = failed with the error code -n (not OUT_OF_BUDGET).
int ChildAttempt(const snd_config& cfg, std::uint64_t k) {
  snd::mock::NullOutputDevice dev;
  snd::mock::MemoryFileSystem fs;
  snd_output_device dt = snd::ToC(dev);
  snd_file_system ft = snd::ToC(fs);
  snd_config c = cfg;
  c.output.device = &dt;
  CountingAllocator a;
  a.failAt.store(k);
  const snd_allocator at = a.Table();
  snd_system* sys = reinterpret_cast<snd_system*>(&c);
  const snd_result r = snd_create(&c, &at, &ft, &sys);
  if (r != SND_OK && sys != nullptr) return 2;
  if (r == SND_OK) snd_destroy(sys);
  if (a.live.load() != 0) return 1;
  if (r == SND_OK) return 4;
  if (r != SND_ERR_OUT_OF_BUDGET) return 10 + (-r);
  return 0;
}

const Sweep& SweepOf(bool liveLink) {
  static Sweep cache[2];
  static bool done[2] = {false, false};
  Sweep& s = cache[liveLink ? 1 : 0];
  if (done[liveLink ? 1 : 0]) return s;
  done[liveLink ? 1 : 0] = true;
  snd_config c = SmallConfig();
  c.livelink.enabled = liveLink ? 1u : 0u;
  c.livelink.port = 0;
  CountingAllocator probe;
  if (CreateOnce(c, &probe) != SND_OK || probe.live.load() != 0) return s;
  s.total = probe.calls.load();
  for (std::uint64_t k = 1; k <= s.total; ++k) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) _exit(ChildAttempt(c, k));
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
      ++s.crashed;
      s.crashAt.push_back(k);
      continue;
    }
    const int code = WEXITSTATUS(status);
    switch (code) {
      case 0: break;
      case 1: ++s.leaked; break;
      case 2: ++s.outSet; break;
      case 4: ++s.accepted; break;
      default:
        ++s.wrongCode;
        s.wrongAt.push_back(k);
        s.wrongCodes.push_back(-(code - 10));
        break;
    }
  }
  return s;
}

void ReportSweep(const Sweep& s, bool live) {
  snd::test::Note("live link %d: %llu allocations; crashed %u, leaked %u, *out set %u, other code %u, accepted %u", live,
                  static_cast<unsigned long long>(s.total), s.crashed, s.leaked, s.outSet, s.wrongCode, s.accepted);
  for (std::size_t i = 0; i < s.crashAt.size();) {  // contiguous ranges
    std::size_t j = i;
    while (j + 1 < s.crashAt.size() && s.crashAt[j + 1] == s.crashAt[j] + 1u) ++j;
    snd::test::Note("  crash when allocation %llu..%llu fails", static_cast<unsigned long long>(s.crashAt[i]),
                    static_cast<unsigned long long>(s.crashAt[j]));
    i = j + 1;
  }
  for (std::size_t i = 0; i < s.wrongAt.size() && i < 6; ++i) {
    snd::test::Note("  allocation %llu fails -> %d", static_cast<unsigned long long>(s.wrongAt[i]), s.wrongCodes[i]);
  }
}

}  // namespace
#endif  // SND_INDEP_FORK_SWEEP

SND_TEST(IndepCreate, Defect_AllocationFailureAnywhereRollsBackWithoutLiveLink) {
#if SND_INDEP_FORK_SWEEP
  const Sweep& s = SweepOf(false);
  ReportSweep(s, false);
  SND_REQUIRE(s.total > 5u);
  SND_CHECK_EQ(s.crashed, 0u);
  SND_CHECK_EQ(s.leaked, 0u);
  SND_CHECK_EQ(s.outSet, 0u);
#else
  snd::test::Note("skipped: no fork on this platform");
#endif
}
SND_TEST(IndepCreate, Defect_AllocationFailureAnywhereRollsBackWithLiveLink) {
#if SND_INDEP_FORK_SWEEP
  const Sweep& s = SweepOf(true);
  ReportSweep(s, true);
  SND_REQUIRE(s.total > 5u);
  SND_CHECK_EQ(s.crashed, 0u);
  SND_CHECK_EQ(s.leaked, 0u);
  SND_CHECK_EQ(s.outSet, 0u);
#else
  snd::test::Note("skipped: no fork on this platform");
#endif
}
// A10 (result code): a NULL from the game's allocator is a budget failure: the result must
// be SND_ERR_OUT_OF_BUDGET, not "no device" or "invalid argument" (snd/types.h).
SND_TEST(IndepCreate, Defect_AllocationFailureIsReportedAsOutOfBudget) {
#if SND_INDEP_FORK_SWEEP
  const Sweep& a = SweepOf(false);
  const Sweep& b = SweepOf(true);
  SND_CHECK_EQ(a.wrongCode + b.wrongCode, 0u);
#else
  snd::test::Note("skipped: no fork on this platform");
#endif
}

// The device cannot be opened: NO_DEVICE, everything given back.
SND_TEST(IndepCreate, DeviceOpenFailureRollsBack) {
  for (const bool live : {false, true}) {
    snd_config c = SmallConfig();
    c.livelink.enabled = live ? 1u : 0u;
    c.livelink.port = 0;
    snd::mock::NullDeviceConfig dc;
    dc.failOpen = true;
    CountingAllocator a;
    bool outNull = false;
    SND_CHECK_EQ(CreateOnce(c, &a, dc, &outNull), SND_ERR_NO_DEVICE);
    SND_CHECK(outNull);
    SND_CHECK_EQ(a.live.load(), 0);
  }
}

// Create and destroy on a thread that is not the one the earlier system used: the owner
// is the creating thread; another system's owner is another thread (no global state).
SND_TEST(IndepCreate, TwoSystemsOnTwoThreads) {
  indep::Rig a;
  SND_REQUIRE_EQ(a.created, SND_OK);
  snd_result other = SND_ERR_INVALID_ARGUMENT, wrong = SND_OK, update = SND_ERR_INVALID_ARGUMENT;
  std::thread t([&] {
    indep::Rig b;
    other = b.created;
    if (b.sys != nullptr) {
      update = snd_update(b.sys);   // b's owner
      wrong = snd_update(a.sys);    // not a's owner
    }
  });
  t.join();
  SND_CHECK_EQ(other, SND_OK);
  SND_CHECK_EQ(update, SND_OK);
  SND_CHECK_EQ(wrong, SND_ERR_WRONG_THREAD);
  SND_CHECK_EQ(snd_update(a.sys), SND_OK);
}
