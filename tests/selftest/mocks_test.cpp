// Tests of the stage-0 mocks (design.md 12.2-6).
#include <cstdint>
#include <vector>

#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/mocks/null_output_device.hpp"
#include "snd/test/test.hpp"

using snd::mock::DeterministicJobSystem;
using snd::mock::JobMode;
using snd::mock::MemoryFileSystem;
using snd::mock::NullDeviceConfig;
using snd::mock::NullOutputDevice;

// ---------------------------------------------------------------------------
// NullOutputDevice
// ---------------------------------------------------------------------------
namespace {
struct RampWriter {
  float next = 0.0f;
  std::uint32_t calls = 0;
  static void Callback(void* user, float* out, std::uint32_t frames, std::uint32_t channels) {
    auto* self = static_cast<RampWriter*>(user);
    ++self->calls;
    for (std::uint32_t i = 0; i < frames; ++i) {
      for (std::uint32_t c = 0; c < channels; ++c) out[i * channels + c] = self->next + static_cast<float>(c) * 1000.0f;
      self->next += 1.0f;
    }
  }
};
struct EventSink {
  std::vector<snd::DeviceEvent> events;
  static void Fn(void* user, const snd::DeviceEvent* e) { static_cast<EventSink*>(user)->events.push_back(*e); }
};
}  // namespace

SND_TEST(NullDevice, ConfigurablePeriodAndRate) {
  NullDeviceConfig cfg;
  cfg.sampleRate = 44100;
  cfg.channels = 2;
  cfg.periodFrames = 441;
  cfg.latencySeconds = 0.02;
  NullOutputDevice dev(cfg);
  RampWriter w;
  snd::DeviceDesc desc{};  // all defaults
  SND_REQUIRE(dev.Open(desc, &RampWriter::Callback, &w));
  SND_CHECK_EQ(dev.SampleRate(), 44100u);
  SND_CHECK_EQ(dev.PeriodFrames(), 441u);
  SND_CHECK_EQ(dev.Channels(), 2u);
  SND_CHECK_NEAR(dev.OutputLatencySeconds(), 0.02, 1e-12);

  NullOutputDevice dev2(cfg);
  snd::DeviceDesc req{48000, 1, 128, 0};
  SND_REQUIRE(dev2.Open(req, &RampWriter::Callback, &w));
  SND_CHECK_EQ(dev2.SampleRate(), 48000u);
  SND_CHECK_EQ(dev2.Channels(), 1u);
  SND_CHECK_EQ(dev2.PeriodFrames(), 128u);
}

SND_TEST(NullDevice, PumpRunsCallbackOnlyWhenStarted) {
  NullDeviceConfig cfg;
  cfg.periodFrames = 4;
  NullOutputDevice dev(cfg);
  RampWriter w;
  SND_CHECK_EQ(dev.Pump(1), 0u);  // not open
  SND_REQUIRE(dev.Open(snd::DeviceDesc{}, &RampWriter::Callback, &w));
  SND_CHECK_EQ(dev.Pump(1), 0u);  // not started
  SND_REQUIRE(dev.Start());
  SND_CHECK_EQ(dev.Pump(3), 3u);
  SND_CHECK_EQ(w.calls, 3u);
  SND_CHECK_EQ(dev.FramesPumped(), 12u);
  SND_CHECK_EQ(dev.CapturedFrames(), 12u);
  std::vector<float> left = dev.CapturedChannel(0);
  std::vector<float> right = dev.CapturedChannel(1);
  SND_REQUIRE_EQ(left.size(), 12u);
  SND_CHECK_EQ(left[0], 0.0f);
  SND_CHECK_EQ(left[11], 11.0f);
  SND_CHECK_EQ(right[5], 1005.0f);
  dev.Stop();
  SND_CHECK_EQ(dev.Pump(1), 0u);
  dev.ClearCapture();
  SND_CHECK_EQ(dev.CapturedFrames(), 0u);
  dev.Close();
  SND_CHECK(!dev.IsOpen());
}

SND_TEST(NullDevice, FailOpenAndEvents) {
  NullDeviceConfig bad;
  bad.failOpen = true;
  NullOutputDevice none(bad);
  RampWriter w;
  SND_CHECK(!none.Open(snd::DeviceDesc{}, &RampWriter::Callback, &w));

  NullOutputDevice dev;
  EventSink sink;
  dev.SetEventListener(&EventSink::Fn, &sink);
  SND_REQUIRE(dev.Open(snd::DeviceDesc{}, &RampWriter::Callback, &w));
  dev.EmitEvent(snd::DeviceEvent{SND_DEVICE_EVENT_CHANGED, 6, 0, 0});
  dev.EmitEvent(snd::DeviceEvent{SND_DEVICE_EVENT_SUSPENDED, 0, 0, 0});
  SND_REQUIRE_EQ(sink.events.size(), 2u);
  SND_CHECK_EQ(sink.events[0].kind, SND_DEVICE_EVENT_CHANGED);
  SND_CHECK_EQ(dev.Channels(), 6u);
  SND_CHECK_EQ(sink.events[1].kind, SND_DEVICE_EVENT_SUSPENDED);
}

SND_TEST(NullDevice, WorksThroughTheCTable) {
  NullOutputDevice dev;
  snd_output_device c = snd::ToC(dev);
  RampWriter w;
  snd_device_desc desc{0, 0, 0, 0};
  SND_REQUIRE_EQ(c.open(c.user, &desc, &RampWriter::Callback, &w), 1);
  SND_REQUIRE_EQ(c.start(c.user), 1);
  SND_CHECK_EQ(c.period_frames(c.user), 256u);
  SND_CHECK_EQ(dev.Pump(2), 2u);
  SND_CHECK_EQ(w.calls, 2u);
  snd::COutputDevice back(c);  // and back to C++
  SND_CHECK_EQ(back.SampleRate(), 48000u);
}

// ---------------------------------------------------------------------------
// MemoryFileSystem
// ---------------------------------------------------------------------------
namespace {
std::vector<std::uint8_t> Bytes(std::uint32_t n) {
  std::vector<std::uint8_t> v(n);
  for (std::uint32_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i & 0xFF);
  return v;
}
snd::IoToken Tok(std::uint64_t v) { return snd::IoToken{v}; }
}  // namespace

SND_TEST(MemoryFs, OpenSizeReadClose) {
  MemoryFileSystem fs;
  fs.AddFile("banks/a.bank", Bytes(1000));
  SND_CHECK_EQ(fs.Open("missing"), snd::kInvalidFile);
  const snd::FileId f = fs.Open("banks/a.bank");
  SND_REQUIRE(f != snd::kInvalidFile);
  SND_CHECK_EQ(fs.Size(f), 1000u);
  SND_CHECK_EQ(fs.Size(999), UINT64_MAX);

  std::uint8_t buf[64] = {};
  SND_REQUIRE(fs.ReadAsync(f, 100, buf, 64, Tok(1)));
  snd::IoStatus s = fs.Poll(Tok(1));
  SND_CHECK_EQ(s.state, SND_IO_DONE);
  SND_CHECK_EQ(s.bytes, 64u);
  SND_CHECK_EQ(buf[0], 100);
  SND_CHECK_EQ(buf[63], 163);
  SND_CHECK_EQ(fs.Poll(Tok(1)).state, SND_IO_UNKNOWN_TOKEN);  // retired

  // Short read at end of file.
  SND_REQUIRE(fs.ReadAsync(f, 990, buf, 64, Tok(2)));
  s = fs.Poll(Tok(2));
  SND_CHECK_EQ(s.state, SND_IO_DONE);
  SND_CHECK_EQ(s.bytes, 10u);
  fs.Close(f);
  SND_CHECK_EQ(fs.OpenFiles(), 0u);
  SND_CHECK(!fs.ReadAsync(f, 0, buf, 1, Tok(3)));
}

SND_TEST(MemoryFs, LatencyInjection) {
  MemoryFileSystem fs;
  fs.AddFile("s.str", Bytes(256));
  fs.SetDefaultLatency(1);
  fs.SetLatency("s.str", 3);
  const snd::FileId f = fs.Open("s.str");
  std::uint8_t buf[8];
  SND_REQUIRE(fs.ReadAsync(f, 0, buf, 8, Tok(7)));
  SND_CHECK_EQ(fs.Poll(Tok(7)).state, SND_IO_PENDING);
  SND_CHECK_EQ(fs.Poll(Tok(7)).state, SND_IO_PENDING);
  SND_CHECK_EQ(fs.Poll(Tok(7)).state, SND_IO_PENDING);
  SND_CHECK_EQ(fs.Poll(Tok(7)).state, SND_IO_DONE);

  fs.Hold("s.str");
  SND_REQUIRE(fs.ReadAsync(f, 0, buf, 8, Tok(8)));
  for (int i = 0; i < 10; ++i) SND_CHECK_EQ(fs.Poll(Tok(8)).state, SND_IO_PENDING);
  fs.Release("s.str");
  SND_CHECK_EQ(fs.Poll(Tok(8)).state, SND_IO_PENDING);  // latency 3 still applies after release
  fs.SetLatency("s.str", 0);
}

SND_TEST(MemoryFs, FailureInjection) {
  MemoryFileSystem fs;
  fs.AddFile("x", Bytes(4096));
  fs.FailOpen("x");
  SND_CHECK_EQ(fs.Open("x"), snd::kInvalidFile);
  fs.FailOpen("x", false);
  const snd::FileId f = fs.Open("x");
  SND_REQUIRE(f != snd::kInvalidFile);

  std::uint8_t buf[16];
  fs.FailReads("x", 1000, 2000);
  SND_REQUIRE(fs.ReadAsync(f, 990, buf, 16, Tok(1)));  // overlaps the range
  SND_CHECK_EQ(fs.Poll(Tok(1)).state, SND_IO_ERROR);
  SND_REQUIRE(fs.ReadAsync(f, 0, buf, 16, Tok(2)));
  SND_CHECK_EQ(fs.Poll(Tok(2)).state, SND_IO_DONE);

  fs.ClearFailures();
  fs.FailNthRead(1);  // second read from now
  SND_REQUIRE(fs.ReadAsync(f, 0, buf, 16, Tok(3)));
  SND_REQUIRE(fs.ReadAsync(f, 0, buf, 16, Tok(4)));
  SND_REQUIRE(fs.ReadAsync(f, 0, buf, 16, Tok(5)));
  SND_CHECK_EQ(fs.Poll(Tok(3)).state, SND_IO_DONE);
  SND_CHECK_EQ(fs.Poll(Tok(4)).state, SND_IO_ERROR);
  SND_CHECK_EQ(fs.Poll(Tok(5)).state, SND_IO_DONE);
}

SND_TEST(MemoryFs, InFlightLimitAndDuplicateTokens) {
  MemoryFileSystem fs;
  fs.AddFile("x", Bytes(64));
  fs.SetMaxInFlight(2);
  fs.SetDefaultLatency(5);
  const snd::FileId f = fs.Open("x");
  std::uint8_t buf[4];
  SND_CHECK(fs.ReadAsync(f, 0, buf, 4, Tok(1)));
  SND_CHECK(!fs.ReadAsync(f, 0, buf, 4, Tok(1)));  // token in flight
  SND_CHECK(fs.ReadAsync(f, 0, buf, 4, Tok(2)));
  SND_CHECK(!fs.ReadAsync(f, 0, buf, 4, Tok(3)));  // limit
  SND_CHECK_EQ(fs.InFlight(), 2u);
  SND_CHECK_EQ(fs.ReadCalls(), 2u);
}

SND_TEST(MemoryFs, WorksThroughTheCTable) {
  MemoryFileSystem fs;
  fs.AddFile("c", Bytes(10));
  snd_file_system c = snd::ToC(fs);
  const snd_file_id id = c.open(c.user, "c");
  SND_REQUIRE(id != SND_FILE_INVALID);
  std::uint8_t buf[10];
  SND_REQUIRE_EQ(c.read_async(c.user, id, 0, buf, 10, snd_io_token{42}), 1);
  snd_io_status s = c.poll(c.user, snd_io_token{42});
  SND_CHECK_EQ(s.state, SND_IO_DONE);
  SND_CHECK_EQ(buf[9], 9);
}

// ---------------------------------------------------------------------------
// DeterministicJobSystem
// ---------------------------------------------------------------------------
namespace {
struct Log {
  std::vector<int> order;
};
struct JobData {
  Log* log;
  int id;
  DeterministicJobSystem* js;  // non-null: submit a child job
  JobData* child;
};
void Record(void* p) {
  auto* d = static_cast<JobData*>(p);
  d->log->order.push_back(d->id);
  if (d->js != nullptr && d->child != nullptr) {
    snd::JobDesc j{&Record, d->child, nullptr, nullptr, 0, 0};
    d->js->Submit(&j, 1, snd::JobLane::Background);
  }
}
snd::JobDesc Job(JobData* d, std::uint32_t* done = nullptr, const std::uint32_t* cancel = nullptr) {
  return snd::JobDesc{&Record, d, done, cancel, 0, 0};
}
}  // namespace

SND_TEST(Jobs, ImmediateRunsInSubmissionOrder) {
  DeterministicJobSystem js(JobMode::Immediate);
  Log log;
  JobData child{&log, 99, nullptr, nullptr};
  JobData a{&log, 1, &js, &child};  // submits 99 while running
  JobData b{&log, 2, nullptr, nullptr};
  std::uint32_t done = 2;
  snd::JobDesc jobs[2] = {Job(&a, &done), Job(&b, &done)};
  js.Submit(jobs, 2, snd::JobLane::Prefetch);
  // 99 was submitted after 2, so it runs after 2 (breadth-first, submission order).
  SND_REQUIRE_EQ(log.order.size(), 3u);
  SND_CHECK_EQ(log.order[0], 1);
  SND_CHECK_EQ(log.order[1], 2);
  SND_CHECK_EQ(log.order[2], 99);
  SND_CHECK_EQ(done, 0u);
  SND_CHECK_EQ(js.Pending(), 0u);
  SND_CHECK_EQ(js.ExecutedCount(), 3u);
}

SND_TEST(Jobs, ManualStepping) {
  DeterministicJobSystem js(JobMode::Manual);
  Log log;
  JobData a{&log, 1, nullptr, nullptr}, b{&log, 2, nullptr, nullptr}, c{&log, 3, nullptr, nullptr};
  snd::JobDesc ja = Job(&a), jb = Job(&b), jc = Job(&c);
  js.Submit(&ja, 1, snd::JobLane::Background);
  js.Submit(&jb, 1, snd::JobLane::Prefetch);
  js.Submit(&jc, 1, snd::JobLane::Background);
  SND_CHECK_EQ(js.Pending(), 3u);
  SND_CHECK_EQ(js.Pending(snd::JobLane::Background), 2u);
  SND_CHECK(log.order.empty());

  SND_CHECK(js.RunNext(snd::JobLane::Prefetch));  // b
  SND_CHECK(!js.RunNext(snd::JobLane::Prefetch));
  SND_CHECK(js.RunNext());                         // a (oldest)
  SND_CHECK_EQ(js.RunAll(), 1u);                   // c
  SND_REQUIRE_EQ(log.order.size(), 3u);
  SND_CHECK_EQ(log.order[0], 2);
  SND_CHECK_EQ(log.order[1], 1);
  SND_CHECK_EQ(log.order[2], 3);
  SND_CHECK(!js.RunNext());
}

SND_TEST(Jobs, CancelFlagSkipsButCompletes) {
  DeterministicJobSystem js(JobMode::Manual);
  Log log;
  JobData a{&log, 1, nullptr, nullptr};
  std::uint32_t done = 1;
  std::uint32_t cancel = 0;
  snd::JobDesc j = Job(&a, &done, &cancel);
  js.Submit(&j, 1, snd::JobLane::Prefetch);
  cancel = 1;  // cancelled before start
  SND_CHECK(js.RunNext());
  SND_CHECK(log.order.empty());
  SND_CHECK_EQ(done, 0u);
  SND_CHECK_EQ(js.SkippedCount(), 1u);
  SND_CHECK_EQ(js.ExecutedCount(), 0u);
}

SND_TEST(Jobs, SwitchingToImmediateDrains) {
  DeterministicJobSystem js(JobMode::Manual);
  Log log;
  JobData a{&log, 5, nullptr, nullptr};
  snd::JobDesc j = Job(&a);
  js.Submit(&j, 1, snd::JobLane::Prefetch);
  SND_CHECK(log.order.empty());
  js.SetMode(JobMode::Immediate);
  SND_CHECK_EQ(log.order.size(), 1u);
}

SND_TEST(Jobs, WorksThroughTheCTable) {
  DeterministicJobSystem js;
  snd_job_system c = snd::ToC(js);
  Log log;
  JobData a{&log, 8, nullptr, nullptr};
  snd_job_desc d{&Record, &a, nullptr, nullptr, 0, 0};
  c.submit(c.user, &d, 1, SND_JOB_LANE_BACKGROUND);
  SND_CHECK_EQ(log.order.size(), 1u);
}
