// snd_create / snd_destroy (A15-3, CONTRACTS.md D-91, D-93):
//  - Config validation at the boundaries: max_voice_channels 0 / 1 / 8 / 9,
//    max_pitch_ratio NaN / +-Inf / just below 1 / 1 / 32 / just above 32, the 1.2
//    capacities 0 / 1 / 65536, plugin tables, the live link's extra thread;
//  - struct_size: a 1.0 or 1.1 caller gets the later defaults whatever bytes follow
//    its struct (the tail is not read); sizes inside a field are refused; a newer,
//    longer struct is accepted; the contract major must match;
//  - every byte snd_create takes from the game's allocator is given back by
//    snd_destroy (and on every failure path);
//  - the order of the lifecycle steps, with and without the live link.
#include <cmath>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "api_rig.hpp"
#include "config_check.hpp"
#include "snd/livelink/server.hpp"

using namespace apitest;

namespace {

// Counting allocator through the C table (balance of every tag).
struct Counting {
  std::atomic<std::int64_t> live{0};
  std::atomic<std::int64_t> bytes{0};
  std::atomic<std::uint64_t> calls{0};
  static void* Alloc(void* u, size_t n, size_t align, snd_mem_tag) {
    auto* c = static_cast<Counting*>(u);
    c->live.fetch_add(1);
    c->calls.fetch_add(1);
    if (align < 16) align = 16;
#if defined(_WIN32)
    return _aligned_malloc(n == 0 ? 1 : n, align);
#else
    void* p = nullptr;
    if (posix_memalign(&p, align, n == 0 ? 1 : n) != 0) return nullptr;
    return p;
#endif
  }
  static void Free(void* u, void* p, snd_mem_tag) {
    if (p == nullptr) return;
    static_cast<Counting*>(u)->live.fetch_sub(1);
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
  }
  snd_allocator Table() { return snd_allocator{this, &Alloc, &Free}; }
};

snd_result TryCreate(snd_config cfg, snd_system** out = nullptr, const snd_allocator* a = nullptr) {
  snd::mock::NullOutputDevice dev;
  snd::mock::MemoryFileSystem fs;
  snd_output_device dt = snd::ToC(dev);
  snd_file_system ft = snd::ToC(fs);
  cfg.output.device = &dt;
  snd_system* sys = reinterpret_cast<snd_system*>(&cfg);  // must be reset
  const snd_result r = snd_create(&cfg, a, &ft, &sys);
  if (r != SND_OK) SND_CHECK(sys == nullptr);
  if (out != nullptr) *out = sys;
  snd_destroy(sys);
  return r;
}

std::vector<std::string>* gSteps = nullptr;
std::mutex gStepsMutex;
void Observe(void*, const char* step) noexcept {
  std::lock_guard<std::mutex> lock(gStepsMutex);
  if (gSteps != nullptr) gSteps->push_back(step);
}

std::size_t IndexOf(const std::vector<std::string>& v, const char* s) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == s) return i;
  }
  return SIZE_MAX;
}

}  // namespace

SND_TEST(ApiCreate, MaxVoiceChannelsBoundaries) {
  const std::uint32_t values[] = {0u, 1u, 8u, 9u, 0xFFFFFFFFu};
  const snd_result want[] = {SND_ERR_INVALID_ARGUMENT, SND_OK, SND_OK, SND_ERR_INVALID_ARGUMENT, SND_ERR_INVALID_ARGUMENT};
  for (std::size_t i = 0; i < 5; ++i) {
    snd_config c = SmallConfig();
    c.max_voice_channels = values[i];
    SND_CHECK_MSG(TryCreate(c) == want[i], "max_voice_channels %u", values[i]);
  }
}

SND_TEST(ApiCreate, MaxPitchRatioBoundaries) {
  const float inf = std::numeric_limits<float>::infinity();
  const float values[] = {std::numeric_limits<float>::quiet_NaN(), inf, -inf, std::nextafter(1.0f, 0.0f), 1.0f,
                          SND_MAX_PITCH_RATIO, std::nextafter(SND_MAX_PITCH_RATIO, 100.0f), 0.0f, -1.0f, 4.0f};
  const snd_result want[] = {SND_ERR_INVALID_ARGUMENT, SND_ERR_INVALID_ARGUMENT, SND_ERR_INVALID_ARGUMENT,
                             SND_ERR_INVALID_ARGUMENT, SND_OK, SND_OK, SND_ERR_INVALID_ARGUMENT,
                             SND_ERR_INVALID_ARGUMENT, SND_ERR_INVALID_ARGUMENT, SND_OK};
  for (std::size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    snd_config c = SmallConfig();
    c.max_pitch_ratio = values[i];
    SND_CHECK_MSG(TryCreate(c) == want[i], "max_pitch_ratio %g", static_cast<double>(values[i]));
  }
}

SND_TEST(ApiCreate, ContractOneTwoCapacities) {
  std::uint32_t snd_config::*fields[] = {&snd_config::max_buses,        &snd_config::max_categories,
                                          &snd_config::max_snapshots,    &snd_config::max_limit_groups,
                                          &snd_config::max_param_defs,   &snd_config::max_duck_rules};
  for (auto f : fields) {
    snd_config c = SmallConfig();
    c.*f = 0u;
    SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
    c.*f = 65536u;
    SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
    c.*f = 1u;
    SND_CHECK_EQ(TryCreate(c), SND_OK);
  }
  snd_config d;
  snd_config_init(&d);  // the documented defaults (config.h, CONTRACTS.md D-92)
  SND_CHECK_EQ(d.max_buses, 128u);
  SND_CHECK_EQ(d.max_categories, 64u);
  SND_CHECK_EQ(d.max_snapshots, 64u);
  SND_CHECK_EQ(d.max_limit_groups, 64u);
  SND_CHECK_EQ(d.max_param_defs, 512u);
  SND_CHECK_EQ(d.max_duck_rules, 64u);
  snd::Config cpp;
  cpp.maxBuses = 3;
  snd_config out;
  snd::detail::ToC(cpp, &out);
  SND_CHECK_EQ(out.max_buses, 3u);
}

SND_TEST(ApiCreate, PluginTablesAndArguments) {
  snd_config c = SmallConfig();
  c.plugins.codec_count = 1;  // count without a table
  SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
  c = SmallConfig();
  c.plugins.effect_count = 1;
  SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
  c = SmallConfig();
  snd_codec codecs[33] = {};
  c.plugins.codecs = codecs;
  c.plugins.codec_count = 33;
  SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
  c.plugins.codec_count = 1;  // fourcc 0 / null functions: the registry refuses the entry
  SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
  // Out and config pointers.
  snd_system* sys = nullptr;
  SND_CHECK_EQ(snd_create(nullptr, nullptr, nullptr, &sys), SND_ERR_INVALID_ARGUMENT);
  c = SmallConfig();
  SND_CHECK_EQ(snd_create(&c, nullptr, nullptr, nullptr), SND_ERR_INVALID_ARGUMENT);
  snd_allocator bad{nullptr, nullptr, nullptr};
  SND_CHECK_EQ(snd_create(&c, &bad, nullptr, &sys), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(sys == nullptr);
  // A device that does not open: SND_ERR_NO_DEVICE, nothing left behind (D-61: no silent fallback).
  snd::mock::NullDeviceConfig dc;
  dc.failOpen = true;
  snd::mock::NullOutputDevice dev(dc);
  snd_output_device dt = snd::ToC(dev);
  c.output.device = &dt;
  Counting count;
  const snd_allocator at = count.Table();
  snd::mock::MemoryFileSystem fs;
  snd_file_system ft = snd::ToC(fs);
  SND_CHECK_EQ(snd_create(&c, &at, &ft, &sys), SND_ERR_NO_DEVICE);
  SND_CHECK(sys == nullptr);
  SND_CHECK_EQ(count.live.load(), 0);
  SND_CHECK(count.calls.load() > 10u);  // it got that far
}

// "struct_size の補完": the struct of an earlier contract ends where its last field ends;
// what follows in the caller's memory is not part of it and must not be read.
SND_TEST(ApiCreate, StructSizeOfEarlierLayoutsGetsTheDefaults) {
  using snd::compose::kConfigSize10;
  using snd::compose::kConfigSize11;
  SND_CHECK(kConfigSize10 < kConfigSize11 && kConfigSize11 < sizeof(snd_config));
  alignas(8) unsigned char buf[sizeof(snd_config) + 64];
  for (const std::size_t size : {kConfigSize10, kConfigSize11}) {
    snd_config c = SmallConfig();
    std::memset(buf, 0xFF, sizeof(buf));  // garbage after the caller's struct
    std::memcpy(buf, &c, size);
    auto* p = reinterpret_cast<snd_config*>(buf);
    p->struct_size = static_cast<std::uint32_t>(size);
    snd_config norm;
    SND_CHECK_EQ(snd::compose::NormalizeConfig(p, &norm), SND_OK);
    SND_CHECK_EQ(norm.struct_size, static_cast<std::uint32_t>(sizeof(snd_config)));
    SND_CHECK_EQ(norm.max_buses, 128u);  // 1.2 defaults
    SND_CHECK_EQ(norm.max_param_defs, 512u);
    if (size == kConfigSize10) {
      SND_CHECK_EQ(norm.max_voice_channels, 8u);  // 1.1 defaults
      SND_CHECK_EQ(norm.max_pitch_ratio, 4.0f);
    } else {
      SND_CHECK_EQ(norm.max_voice_channels, 2u);  // the caller's 1.1 values
    }
    SND_CHECK_EQ(norm.max_playbacks, 64u);  // the caller's 1.0 values
    // And snd_create accepts it (the garbage would be refused if it were read).
    snd::mock::NullOutputDevice dev;
    snd::mock::MemoryFileSystem fs;
    snd_output_device dt = snd::ToC(dev);
    snd_file_system ft = snd::ToC(fs);
    p->output.device = &dt;
    snd_system* sys = nullptr;
    SND_CHECK_EQ(snd_create(p, nullptr, &ft, &sys), SND_OK);
    snd_destroy(sys);
  }
  // Inside a field, or shorter than 1.0: refused.
  snd_config c = SmallConfig();
  snd_config norm;
  for (const std::size_t size : {std::size_t{4}, kConfigSize10 - 8, kConfigSize10 + 4, kConfigSize11 + 4,
                                 sizeof(snd_config) - 4}) {
    c.struct_size = static_cast<std::uint32_t>(size);
    SND_CHECK_MSG(snd::compose::NormalizeConfig(&c, &norm) == SND_ERR_VERSION_MISMATCH, "size %llu", static_cast<unsigned long long>(size));
  }
  // A longer struct (a newer minor): its tail is ignored.
  std::memset(buf, 0xFF, sizeof(buf));
  c = SmallConfig();
  std::memcpy(buf, &c, sizeof(c));
  reinterpret_cast<snd_config*>(buf)->struct_size = static_cast<std::uint32_t>(sizeof(snd_config) + 64);
  reinterpret_cast<snd_config*>(buf)->contract_version = (SND_CONTRACT_VERSION_MAJOR << 16) | 99u;
  SND_CHECK_EQ(snd::compose::NormalizeConfig(reinterpret_cast<snd_config*>(buf), &norm), SND_OK);
  SND_CHECK_EQ(norm.contract_version, static_cast<std::uint32_t>(SND_CONTRACT_VERSION));
  // Another major.
  c = SmallConfig();
  c.contract_version = ((SND_CONTRACT_VERSION_MAJOR + 1u) << 16);
  SND_CHECK_EQ(TryCreate(c), SND_ERR_VERSION_MISMATCH);
  c.contract_version = SND_CONTRACT_VERSION_MAJOR << 16;  // minor 0 of the same major
  SND_CHECK_EQ(TryCreate(c), SND_OK);
}

SND_TEST(ApiCreate, GameAllocatorIsBalanced) {
  Counting count;
  const snd_allocator a = count.Table();
  snd_system* sys = nullptr;
  SND_CHECK_EQ(TryCreate(SmallConfig(), &sys, &a), SND_OK);
  SND_CHECK(count.calls.load() > 10u);
  SND_CHECK_EQ(count.live.load(), 0);
  // A failure inside core (block size not a power of two) frees everything too.
  snd_config c = SmallConfig();
  c.block_size = 300;
  SND_CHECK_EQ(TryCreate(c, nullptr, &a), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(count.live.load(), 0);
}

SND_TEST(ApiCreate, LifecycleOrderWithoutLiveLink) {
  std::vector<std::string> steps;
  {
    std::lock_guard<std::mutex> lock(gStepsMutex);
    gSteps = &steps;
  }
  snd::compose::SetLifecycleObserver(&Observe, nullptr);
  snd_config c = SmallConfig();
  c.livelink.enabled = 0;
  SND_CHECK_EQ(TryCreate(c), SND_OK);
  snd::compose::SetLifecycleObserver(nullptr, nullptr);
  {
    std::lock_guard<std::mutex> lock(gStepsMutex);
    gSteps = nullptr;
  }
  const char* const want[] = {"create:config",        "create:file-system", "create:workers",      "create:codecs",
                              "create:io",            "create:source-kinds", "create:core",         "create:audio-driver",
                              "destroy:audio-driver", "destroy:io-shutdown", "destroy:workers",     "destroy:core",
                              "destroy:source-kinds", "destroy:io",          "destroy:services",    "destroy:system"};
  SND_REQUIRE_EQ(steps.size(), sizeof(want) / sizeof(want[0]));
  for (std::size_t i = 0; i < steps.size(); ++i) SND_CHECK_MSG(steps[i] == want[i], "%llu: %s", static_cast<unsigned long long>(i), steps[i].c_str());
}

SND_TEST(ApiCreate, LifecycleOrderWithLiveLink) {
#if SND_LIVELINK
  std::vector<std::string> steps;
  {
    std::lock_guard<std::mutex> lock(gStepsMutex);
    gSteps = &steps;
  }
  snd::compose::SetLifecycleObserver(&Observe, nullptr);
  snd_config c = SmallConfig();
  c.livelink.enabled = 1;
  c.livelink.port = 0;  // any free port
  c.max_registered_threads = 3;
  {
    Rig rig(c);
    SND_REQUIRE_EQ(rig.created, SND_OK);
    const snd::compose::Parts parts = snd::compose::PartsOf(rig.sys);
    SND_CHECK(parts.live != nullptr);
    SND_REQUIRE(parts.server != nullptr);
    SND_CHECK(parts.server->Port() != 0u);              // listening
    SND_CHECK_EQ(parts.coreRegisteredThreads, 4u);       // + the live link thread
    // The game still has all of its threads: 2 more besides the owner (3 in total).
    std::thread t1([&] {
      SND_CHECK_EQ(snd_register_thread(rig.sys), SND_OK);
      std::thread t2([&] {
        SND_CHECK_EQ(snd_register_thread(rig.sys), SND_OK);
        SND_CHECK_EQ(snd_unregister_thread(rig.sys), SND_OK);
      });
      t2.join();
      SND_CHECK_EQ(snd_unregister_thread(rig.sys), SND_OK);
    });
    t1.join();
    rig.Step(8);
  }
  snd::compose::SetLifecycleObserver(nullptr, nullptr);
  {
    std::lock_guard<std::mutex> lock(gStepsMutex);
    gSteps = nullptr;
  }
  const char* const order[] = {"create:source-kinds", "create:live-runtime", "create:live-install", "create:core",
                               "create:audio-driver", "create:live-server",  "create:live-start",
                               "destroy:live-server", "destroy:audio-driver", "destroy:io-shutdown",
                               "destroy:workers",     "destroy:core",         "destroy:live-runtime",
                               "destroy:source-kinds", "destroy:io"};
  std::size_t last = 0;
  for (const char* s : order) {
    const std::size_t at = IndexOf(steps, s);
    SND_CHECK_MSG(at != SIZE_MAX && at >= last, "%s at %llu (after %llu)", s, static_cast<unsigned long long>(at),
                  static_cast<unsigned long long>(last));
    if (at != SIZE_MAX) last = at;
  }
  // 63 game threads + the live link = 64 (core's limit); 64 game threads cannot fit.
  c.max_registered_threads = 64;
  SND_CHECK_EQ(TryCreate(c), SND_ERR_INVALID_ARGUMENT);
  c.livelink.enabled = 0;
  SND_CHECK_EQ(TryCreate(c), SND_OK);
#else
  // Built without the live link: Config.livelink.enabled is ignored.
  snd_config c = SmallConfig();
  c.livelink.enabled = 1;
  Rig rig(c);
  SND_CHECK_EQ(rig.created, SND_OK);
  SND_CHECK(snd::compose::PartsOf(rig.sys).live == nullptr);
  SND_CHECK_EQ(snd::compose::PartsOf(rig.sys).coreRegisteredThreads, c.max_registered_threads);
#endif
}

SND_TEST(ApiCreate, LiveLinkOffMeansNoServerAndNoExtraThread) {
  snd_config c = SmallConfig();
  c.livelink.enabled = 0;
  Rig rig(c);
  SND_REQUIRE_EQ(rig.created, SND_OK);
  const snd::compose::Parts parts = snd::compose::PartsOf(rig.sys);
  SND_CHECK(parts.live == nullptr);
  SND_CHECK(parts.server == nullptr);
  SND_CHECK_EQ(parts.coreRegisteredThreads, c.max_registered_threads);
  SND_CHECK(parts.music != nullptr);
  SND_CHECK(parts.driver != nullptr);
  // The owner plus 3 more threads, then the limit.
  std::vector<std::thread> ts;
  std::atomic<int> ok{0}, limit{0};
  std::atomic<int> registered{0};
  std::atomic<bool> release{false};
  for (int i = 0; i < 4; ++i) {
    ts.emplace_back([&] {
      const snd_result r = snd_register_thread(rig.sys);
      if (r == SND_OK) ok.fetch_add(1);
      if (r == SND_ERR_LIMIT_REACHED) limit.fetch_add(1);
      registered.fetch_add(1);
      while (!release.load()) std::this_thread::yield();
      if (r == SND_OK) snd_unregister_thread(rig.sys);
    });
  }
  while (registered.load() < 4) std::this_thread::yield();
  release.store(true);
  for (std::thread& t : ts) t.join();
  SND_CHECK_EQ(ok.load(), 3);
  SND_CHECK_EQ(limit.load(), 1);
}
