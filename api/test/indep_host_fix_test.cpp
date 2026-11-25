// Independent tests of the fixes 23f1b59 / fc38dea (CONTRACTS.md D-102..D-104 notes).
//  X1 PlatformOutputDevice::OutputClock (seqlock): readers on other threads never see a torn
//     sample (the manual device publishes written = k x period with nothing queued, so
//     played == written in every consistent sample), never a false "no clock" once the device
//     ran, and the values never go back for one reader.
//  X2 reserved ids (SND_ID_RESERVED_FIRST..LAST): snd_set_param_* refuse each of them in every
//     scope; their neighbours are ordinary ids; the rule table's SetParam api rule and the
//     fallback's CheckOverflowParam accept a reserved id exactly in the form
//     snd_set_playback_volume / _pitch record, and agree with the C API on the values.
//  X3 stats.fallback_commands counts the coalesced commands; reserve_overflows stays the
//     number of values lost.
//  X4 (POSIX) a FIFO is never opened for reading by the file system, also behind a link
//     inside the root and behind a link that leaves it (a writer's O_NONBLOCK open succeeds
//     only while someone has the FIFO open for reading).
#include <atomic>
#include <barrier>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "indep_rig.hpp"
#include "snd/base/allocator.hpp"
#include "snd/core/core.hpp"
#include "snd/io/default_file_system.hpp"
#include "snd/livelink/command_check.hpp"
#include "snd/platform/manual_output_device.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using apitest::SmallConfig;

namespace {

void SND_CALL Silence(void*, float* interleaved, std::uint32_t frames, std::uint32_t channels) {
  std::memset(interleaved, 0, sizeof(float) * frames * channels);
}

}  // namespace

// X1
SND_TEST(IndepHostFix, OutputClockIsNeverTornNorFalse) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  snd::platform::ManualOutputDevice dev(acc);
  snd::platform::OutputClockSample s{};
  SND_CHECK(!dev.OutputClock(&s));  // before anything was published
  SND_REQUIRE(dev.Open(snd::DeviceDesc{48000, 2, 64, 0}, &Silence, nullptr));
  SND_REQUIRE(dev.Start());
  SND_REQUIRE_EQ(dev.Advance(1, nullptr), 1u);
  constexpr int readerCount = 3, batches = 4, periodsPerBatch = 50000;
  constexpr unsigned minSamplesPerBatch = 128;
  std::barrier batchFinished(readerCount + 1);
  std::atomic<int> publishedBatches{0};
  std::atomic<std::uint64_t> torn{0}, falses{0}, back{0}, reads{0}, badRate{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < readerCount; ++i) {
    readers.emplace_back([&] {
      std::uint64_t lastW = 0, lastT = 0;
      for (int batch = 0; batch < batches; ++batch) {
        unsigned samples = 0;
        do {
          ++samples;
          snd::platform::OutputClockSample v{};
          if (!dev.OutputClock(&v)) {
            falses.fetch_add(1);
            continue;
          }
          reads.fetch_add(1, std::memory_order_relaxed);
          if (v.framesPlayed != v.framesWritten || v.framesWritten % 64u != 0u) torn.fetch_add(1);
          if (v.framesWritten < lastW || v.timestampNs < lastT) back.fetch_add(1);
          if (v.sampleRate != 48000u) badRate.fetch_add(1);
          lastW = v.framesWritten;
          lastT = v.timestampNs;
        } while (samples < minSamplesPerBatch || publishedBatches.load() <= batch);
        batchFinished.arrive_and_wait();
      }
    });
  }
  std::uint64_t periods = 1;
  for (int batch = 0; batch < batches; ++batch) {
    for (int k = 0; k < periodsPerBatch; ++k) periods += dev.Advance(1, nullptr);
    // Keep checking during writes, and give each reader a turn between batches on a busy host.
    publishedBatches.store(batch + 1);
    batchFinished.arrive_and_wait();
  }
  for (std::thread& t : readers) t.join();
  snd::test::Note("%llu periods, %llu reads", static_cast<unsigned long long>(periods),
                  static_cast<unsigned long long>(reads.load()));
  SND_CHECK_EQ(torn.load(), 0u);
  SND_CHECK_EQ(falses.load(), 0u);
  SND_CHECK_EQ(back.load(), 0u);
  SND_CHECK_EQ(badRate.load(), 0u);
  SND_CHECK(reads.load() >= readerCount * batches * minSamplesPerBatch);
  SND_REQUIRE(dev.OutputClock(&s));
  SND_CHECK_EQ(s.framesWritten, periods * 64u);
  dev.Close();
}

// X2: the C API.
SND_TEST(IndepHostFix, ReservedIdsAreRefusedByTheParamFunctions) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  static const float kV = 0.1f;
  const snd_synth_desc d = indep::DcDesc(&kV);
  const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
  const snd_emitter_handle e = snd_create_emitter(r.sys);
  SND_REQUIRE(p.value != 0u && e.value != 0u);
  SND_CHECK_EQ(SND_ID_RESERVED_FIRST, 0xFFFFFFFCu);
  SND_CHECK_EQ(SND_ID_RESERVED_LAST, 0xFFFFFFFEu);
  for (snd_id id = SND_ID_RESERVED_FIRST; id >= SND_ID_RESERVED_FIRST && id <= SND_ID_RESERVED_LAST; ++id) {
    SND_CHECK_EQ(snd_set_param_global(r.sys, id, 0.5f), SND_ERR_INVALID_ARGUMENT);
    SND_CHECK_EQ(snd_set_param_emitter(r.sys, e, id, 0.5f), SND_ERR_INVALID_ARGUMENT);
    SND_CHECK_EQ(snd_set_param_playback(r.sys, p, id, 0.5f), SND_ERR_INVALID_ARGUMENT);
    SND_CHECK(snd::livelink::IsReservedId(id));
    // Handle 0 is still INVALID_HANDLE first.
    SND_CHECK_EQ(snd_set_param_playback(r.sys, snd_playback_handle{0}, id, 0.5f), SND_ERR_INVALID_HANDLE);
  }
  for (const snd_id id : {0xFFFFFFFBu, 0xFFFFFFFFu, 1u}) {
    SND_CHECK(!snd::livelink::IsReservedId(id));
    SND_CHECK_EQ(snd_set_param_global(r.sys, id, 0.5f), SND_OK);
    SND_CHECK_EQ(snd_set_param_playback(r.sys, p, id, 0.5f), SND_OK);
  }
  // The level is untouched by all of that (the refused calls recorded nothing).
  r.Step(10);
  SND_CHECK_NEAR(r.Raw(r.Frames() - 1) / apitest::kCenter, kV, 1e-6f);
}

// X2: the rule table and the fallback check agree with the C API's value rules.
SND_TEST(IndepHostFix, ReservedIdRulesAgreeAcrossApiReplayAndFallback) {
  snd_config c = SmallConfig();
  const snd::livelink::CommandLimits lim = snd::livelink::LimitsOf(c);
  const snd::livelink::CommandRule* rule = snd::livelink::RuleOf(static_cast<std::uint16_t>(snd::cmd::CommandType::SetParam));
  SND_REQUIRE(rule != nullptr);
  const snd::HandleRef pb{3, 1};
  const snd::HandleRef em{2, 1};
  const float values[] = {-1.0f, -0.0f, 0.0f, std::numeric_limits<float>::denorm_min(), 0.5f, 1.0f, 1000.0f, 1e30f,
                          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  const snd_id ids[] = {snd::core::kParamPlaybackVolume, snd::core::kParamPlaybackPitch, 0xFFFFFFFEu, 0x1234u};
  const std::uint8_t scopes[] = {static_cast<std::uint8_t>(snd::ParamScope::Global),
                                 static_cast<std::uint8_t>(snd::ParamScope::Emitter),
                                 static_cast<std::uint8_t>(snd::ParamScope::Playback)};
  std::uint32_t mismatches = 0;
  for (const snd_id id : ids) {
    for (const std::uint8_t scope : scopes) {
      for (const float v : values) {
        snd::cmd::SetParam sp{};
        sp.scope = scope;
        sp.param = id;
        sp.target = scope == scopes[0] ? snd::HandleRef{} : (scope == scopes[1] ? em : pb);
        sp.value = v;
        const bool apiRule = rule->api(reinterpret_cast<const std::uint8_t*>(&sp), lim);
        snd::cmd::OverflowParam op{};
        op.scope = scope;
        op.isSwitch = 0;
        op.id = id;
        op.target = sp.target;
        std::memcpy(&op.value, &v, 4);
        op.used = 1;
        const bool fb = snd::livelink::CheckOverflowParam(op, lim);
        // What the C API would record for this (id, scope, value).
        bool cApi = false;
        if (id == snd::core::kParamPlaybackVolume && scope == scopes[2]) cApi = std::isfinite(v) && v >= 0.0f;
        else if (id == snd::core::kParamPlaybackPitch && scope == scopes[2]) cApi = std::isfinite(v) && v > 0.0f;
        else if (!snd::livelink::IsReservedId(id)) cApi = std::isfinite(v);
        if (apiRule != cApi || fb != cApi) {
          if (mismatches < 6u) {
            snd::test::Note("id %08x scope %u value %g: rule %d, fallback %d, C API %d", id, scope,
                            static_cast<double>(v), apiRule, fb, cApi);
          }
          ++mismatches;
        }
      }
    }
  }
  SND_CHECK_EQ(mismatches, 0u);
}

// X3
SND_TEST(IndepHostFix, FallbackCommandsAreCounted) {
  std::vector<float> mem(1024, 0.01f);
  snd_config c = SmallConfig();
  c.commands.arena_bytes = 2048;
  c.commands.reserve_bytes = 512;
  c.commands.overflow_param_slots = 32;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  float* ch[1] = {mem.data()};
  const snd_user_buffer_desc bd{ch, 1, 1024, 0, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &bd);
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  r.Step(8);
  snd_update(r.sys);
  snd_stats st{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
  SND_CHECK_EQ(st.fallback_commands, 0u);
  // Each SetParam is 8 + 24 bytes or so: 1000 of them cannot fit 2.5 KiB; all but the first
  // few dozen are coalesced (one entry: the same playback and id).
  std::uint32_t ok = 0;
  for (int i = 0; i < 1000; ++i) ok += snd_set_playback_volume(r.sys, p, 0.5f + 0.001f * float(i)) == SND_OK ? 1u : 0u;
  SND_CHECK_EQ(ok, 1000u);
  r.Step(10);
  snd_update(r.sys);
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
  snd::test::Note("fallback_commands %u, reserve_overflows %u", st.fallback_commands, st.reserve_overflows);
  SND_CHECK(st.fallback_commands >= 900u && st.fallback_commands <= 1000u);
  SND_CHECK_EQ(st.reserve_overflows, 0u);  // nothing lost: one table entry suffices
  SND_CHECK_NEAR(r.Raw(r.Frames() - 1) / apitest::kCenter, 0.01f * (0.5f + 0.001f * 999.0f), 1e-6f);
  // Many distinct targets: more than the table holds -> values lost, counted.
  std::vector<snd_playback_handle> ps;
  for (int i = 0; i < 40; ++i) ps.push_back(snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr));
  r.Step(4);
  for (int round = 0; round < 3; ++round) {
    for (const snd_playback_handle& q : ps) snd_set_playback_volume(r.sys, q, 0.0f);
    for (int i = 0; i < 200; ++i) snd_set_playback_volume(r.sys, p, 0.0f);  // fill the arena first
  }
  r.Step(6);
  snd_update(r.sys);
  snd_stats st2{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &st2), SND_OK);
  snd::test::Note("after 40 targets: fallback_commands %u, reserve_overflows %u", st2.fallback_commands,
                  st2.reserve_overflows);
  SND_CHECK(st2.fallback_commands > st.fallback_commands);
  for (const snd_playback_handle& q : ps) snd_stop(r.sys, q, 0.0f, nullptr);
  snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(6);
  snd_destroy_user_buffer(r.sys, b);
  r.Step(4);
}

#if !defined(_WIN32)
// X4
SND_TEST(IndepHostFix, FifoIsNeverOpenedForReading) {
  char tmpl[] = "/tmp/snd-indep-fifo-XXXXXX";
  const char* d = mkdtemp(tmpl);
  SND_REQUIRE(d != nullptr);
  const std::string base = d, root = base + "/root";
  SND_REQUIRE_EQ(mkdir(root.c_str(), 0700), 0);
  SND_REQUIRE_EQ(mkfifo((root + "/in.fifo").c_str(), 0600), 0);
  SND_REQUIRE_EQ(mkfifo((base + "/out.fifo").c_str(), 0600), 0);
  SND_REQUIRE_EQ(symlink("in.fifo", (root + "/link_in").c_str()), 0);
  SND_REQUIRE_EQ(symlink((base + "/out.fifo").c_str(), (root + "/link_out").c_str()), 0);
  SND_REQUIRE_EQ(symlink("../out.fifo", (root + "/rel_out").c_str()), 0);
  snd::io::DefaultFileSystem fs;
  snd::io::DefaultFileSystemConfig fc;
  fc.root = root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), fc));
  for (const char* name : {"in.fifo", "link_in", "link_out", "rel_out"}) {
    const std::string fifo = std::string(name).find("out") != std::string::npos ? base + "/out.fifo" : root + "/in.fifo";
    std::atomic<bool> stop{false};
    std::atomic<int> sawReader{0};
    std::thread probe([&] {
      while (!stop.load()) {
        const int w = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (w >= 0) {  // succeeds only while the FIFO is open for reading somewhere
          sawReader.fetch_add(1);
          ::close(w);
        }
      }
    });
    int opened = 0;
    for (int i = 0; i < 3000; ++i) {
      const snd::FileId id = fs.Open(name);
      if (id != snd::kInvalidFile) {
        ++opened;
        fs.Close(id);
      }
    }
    stop.store(true);
    probe.join();
    SND_CHECK_MSG(opened == 0, "%s: opened %d times", name, opened);
    SND_CHECK_MSG(sawReader.load() == 0, "%s: the FIFO was opened for reading %d times while checking", name,
                  sawReader.load());
  }
  fs.Shutdown();
  const std::string cmd = "rm -rf '" + base + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
}
#endif
