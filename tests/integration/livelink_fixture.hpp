// Fixture for the live link integration tests: one core System with the core
// tests' analytic Sample kind, a dev bank (override layer) in a memory FS, the
// deterministic job system, and either a LiveRuntime (+ Server over an
// in-memory or TCP transport) or a Replayer installed into its CoreDesc.
//
// Bank "Live" (dev build, projectBuildHash kProjectHash):
//   BUSS  row 0 "Sfx" (parent Master, stereo, 0 dB), row 1 "Master" (master)
//   ASST  "dc": mono 48 kHz, 4800 frames of kDc, looped
//   EVNT  "Tone" (Sfx, loops forever), "Blip" (Sfx, plays once, 2400 frames)
//   PARM  "Knob" (0..1, default 0)
// The Sample kind plays the asset exactly, so the master output of "Tone" is
// a constant whose level follows the bus volumes analytically.
#ifndef SND_TESTS_LIVELINK_FIXTURE_HPP
#define SND_TESTS_LIVELINK_FIXTURE_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/buildtool/bank_writer.hpp"
#include "snd/core/core.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/id.hpp"
#include "snd/livelink/client.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/livelink/memory_transport.hpp"
#include "snd/livelink/replayer.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/livelink/server.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_sources.hpp"

namespace lltest {

namespace bf = snd::bank_format;
namespace bt = snd::buildtool;
namespace ll = snd::livelink;

inline constexpr std::uint64_t kProjectHash = 0x5EED5EED12345678ull;
inline constexpr float kDc = 0.5f;
inline constexpr snd::Id kBankId = snd::HashId("Live");

inline std::vector<std::uint8_t> DevBank(std::uint64_t projectHash = kProjectHash, bool dev = true) {
  namespace c = bf::col;
  bt::BankWriter w;
  bt::TableBuilder ev(bf::chunk::EVNT), tr(bf::chunk::TRAK), cl(bf::chunk::CLIP), bu(bf::chunk::BUSS),
      as(bf::chunk::ASST), pa(bf::chunk::PARM);
  // buses (inputs first)
  std::uint32_t r = bu.AddRow();
  bu.U32(c::BUSS_id, r, snd::HashId("Sfx"));
  bu.U32(c::BUSS_parent, r, 1u);
  bu.U32(c::BUSS_channels, r, 2u);
  r = bu.AddRow();
  bu.U32(c::BUSS_id, r, snd::HashId("Master"));
  bu.U32(c::BUSS_parent, r, bf::kNone);
  bu.U32(c::BUSS_channels, r, 2u);
  bu.U32(c::BUSS_flags, r, bf::kBusMaster);
  // asset
  std::vector<std::uint8_t> wave(4800 * sizeof(float));
  for (std::size_t i = 0; i < 4800; ++i) std::memcpy(wave.data() + i * 4, &kDc, 4);
  r = as.AddRow();
  as.U32(c::ASST_id, r, snd::HashId("dc"));
  as.U32(c::ASST_loadForm, r, 0);
  as.U32(c::ASST_codec, r, SND_CODEC_PCM_F32);
  as.U32(c::ASST_channels, r, 1);
  as.U32(c::ASST_sampleRate, r, 48000);
  as.U64(c::ASST_totalFrames, r, 4800);
  as.U32(c::ASST_blockAlign, r, 4);
  as.U32(c::ASST_framesPerPacket, r, 1);
  as.U64(c::ASST_dataOffset, r, 0);
  as.U64(c::ASST_dataBytes, r, wave.size());
  // events
  auto event = [&](const char* name, std::uint32_t loopCount) {
    const std::uint32_t e = ev.AddRow();
    ev.U32(c::EVNT_id, e, snd::HashId(name));
    ev.U32(c::EVNT_trackFirst, e, tr.rows());
    ev.U32(c::EVNT_trackCount, e, 1u);
    ev.U32(c::EVNT_bus, e, 0u);
    const std::uint32_t t = tr.AddRow();
    tr.U32(c::TRAK_event, t, e);
    tr.U32(c::TRAK_clipFirst, t, cl.rows());
    tr.U32(c::TRAK_clipCount, t, 1u);
    const std::uint32_t k = cl.AddRow();
    cl.U32(c::CLIP_kind, k, 0u);
    cl.U32(c::CLIP_target, k, 0u);
    cl.U32(c::CLIP_loopCount, k, loopCount);
  };
  event("Tone", 0u);
  event("Blip", 1u);
  r = pa.AddRow();
  pa.U32(c::PARM_id, r, snd::HashId("Knob"));
  pa.F32(c::PARM_maxValue, r, 1.0f);
  w.AddTable(ev);
  w.AddTable(tr);
  w.AddTable(cl);
  w.AddTable(bu);
  w.AddTable(as);
  w.AddBlob(bf::chunk::WAVE, wave);
  w.AddTable(pa);
  bt::BankHeaderInfo hi;
  hi.bankId = kBankId;
  hi.projectBuildHash = projectHash;
  if (dev) hi.flags |= bf::kFileDevBuild;
  return w.Finish(hi);
}

inline snd_config Config() {
  snd_config c;
  snd_config_init(&c);
  c.max_real_voices = 16;
  c.max_virtual_voices = 64;
  c.fade_reserve_voices = 4;
  c.max_playbacks = 64;
  c.max_emitters = 16;
  c.max_transports = 4;
  c.max_reservations = 32;
  c.max_streams = 4;
  c.max_banks = 8;
  c.max_user_buffers = 4;
  c.max_global_params = 32;
  c.param_overflow_blocks = 8;
  c.event_ring_capacity = 256;
  c.max_registered_threads = 4;
  c.commands.arena_bytes = 16u << 10;
  c.commands.reserve_bytes = 2u << 10;
  c.commands.overflow_param_slots = 16;
  c.memory.frame_arena_bytes = 128u << 10;
  c.memory.thread_arena_bytes = 256u << 10;
  c.jobs.realtime_workers = 0;
  c.jobs.voices_per_chunk = 4;
  c.max_voice_channels = 2;
  c.tuning.lod_down_blocks = 0;  // 2.5: LOD disabled for bit identity
  c.livelink.enabled = 1;
  return c;
}

enum class Hooks { None, Live, Replay };

class Fixture {
 public:
  explicit Fixture(Hooks hooks = Hooks::Live, const snd_config& cfg = Config(), ll::Replayer* replayer = nullptr,
                   ll::RuntimeDesc rd = {})
      : cfg_(cfg) {
    kinds_.kinds[static_cast<int>(snd::SourceKind::Sample)] = &sample;
    snd::core::CoreDesc d;
    d.config = &cfg_;
    d.allocator = &snd::base::DefaultAllocator();
    d.sources = &kinds_;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    if (hooks == Hooks::Live) {
      rd.config = &cfg_;
      rd.allocator = &snd::base::DefaultAllocator();
      created = ll::LiveRuntime::Create(rd, &rt);
      if (rt != nullptr) rt->Install(&d);
    } else if (hooks == Hooks::Replay && replayer != nullptr) {
      replayer->Install(&d);
    }
    const snd_result r = snd::core::System::Create(d, &sys);
    if (created == SND_OK) created = r;
    if (sys != nullptr) {
      if (rt != nullptr) rt->Bind(sys);
      if (replayer != nullptr) replayer->Bind(sys);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~Fixture() {
    StopServer();
    snd::core::System::Destroy(sys);
    ll::LiveRuntime::Destroy(rt);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  // Server over the in-memory link (default) or TCP (acceptor = nullptr, tcp = true).
  snd_result StartServer(ll::ServerDesc sd = {}, bool tcp = false) {
    if (!tcp && !link.Init(snd::base::DefaultAllocator(), 1u << 20)) return SND_ERR_OUT_OF_BUDGET;
    sd.system = sys;
    sd.runtime = rt;
    sd.allocator = &snd::base::DefaultAllocator();
    if (!tcp) sd.acceptor = &link.acceptor();
    if (tcp && sd.port < 0) sd.port = 0;
    snd_result r = ll::Server::Create(sd, &server);
    if (r == SND_OK) r = server->Start();
    return r;
  }
  void StopServer() {
    if (server != nullptr) ll::Server::Destroy(server);
    server = nullptr;
  }

  void Step() {
    sys->Update();
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
  }
  void Steps(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Step();
  }
  snd_bank_handle Load(const std::vector<std::uint8_t>& bytes, const char* path = "Live.sndb") {
    fs.AddFile(path, bytes);
    const snd_bank_handle h = sys->LoadBankAsync(path);
    for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) Step();
    sys->Update();
    return h;
  }
  // Renders blocks (real time not needed) until pred() or the timeout; true if pred().
  template <class Pred>
  bool StepUntil(Pred&& pred, std::uint32_t timeoutMs = 5000, std::uint32_t sleepUs = 200) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      Step();
      std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    }
    return true;
  }
  // Left-channel sample of the last rendered block (constant signal: any frame).
  float LastLeft() const { return out[(out.size() / 2 / sys->OutputChannels()) * sys->OutputChannels()]; }

  snd_config cfg_;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  coretest::TestSampleKind sample;
  ll::LiveRuntime* rt = nullptr;
  snd::core::System* sys = nullptr;
  ll::MemoryLink link;
  ll::Server* server = nullptr;
  snd_result created = SND_OK;
  std::vector<float> out;
  std::vector<float> all;

 private:
  snd::source::SourceKindSet kinds_{};
};

inline ll::SetValue BusVolume(const char* bus, float db) {
  ll::SetValue v{};
  v.runtimeId = snd::HashId(bus);
  v.chunk = bf::chunk::BUSS;
  v.column = bf::col::BUSS_volumeDb;
  v.valueType = static_cast<std::uint32_t>(ll::ValueType::F32);
  std::uint32_t bits = 0;
  std::memcpy(&bits, &db, 4);
  v.value = bits;
  return v;
}

inline ll::ClientDesc MemoryClient(ll::MemoryLink& link, std::uint64_t hash = kProjectHash) {
  ll::ClientDesc d;
  d.connector = &link.connector();
  d.projectBuildHash = hash;
  d.name = "test-tool";
  return d;
}

// A hand-driven tool end (raw frames) for protocol and robustness tests.
class RawPeer final : public ll::FrameReader::Handler {
 public:
  struct Frame {
    ll::FrameHeader header;
    std::vector<std::uint8_t> payload;
  };
  RawPeer() { reader_.Init(snd::base::DefaultAllocator(), 1u << 20); }
  ~RawPeer() override { Close(); }
  bool Connect(ll::IConnector& c, std::uint32_t timeoutMs = 2000) {
    connector_ = &c;
    t_ = c.Connect(timeoutMs);
    return t_ != nullptr;
  }
  void Close() {
    if (t_ != nullptr) connector_->Release(t_);
    t_ = nullptr;
  }
  // Sends bytes as given (false if the stream closed).
  bool SendBytes(const void* data, std::size_t n, std::uint32_t timeoutMs = 2000) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (n != 0u) {
      std::size_t sent = 0;
      const ll::IoStatus st = t_->Send(p, n, &sent);
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) return false;
      p += sent;
      n -= sent;
      if (n != 0u) {
        if (std::chrono::steady_clock::now() > end) return false;
        t_->Wait(false, true, 5);
      }
    }
    return true;
  }
  bool Send(ll::MessageType type, const void* payload, std::uint32_t n, std::uint16_t flags = 0, std::uint32_t seq = 1) {
    std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(type), flags, n, seq);
    if (n != 0u) std::memcpy(b.data() + ll::kFrameHeaderBytes, payload, n);
    return SendBytes(b.data(), b.size());
  }
  bool Hello(std::uint64_t hash = kProjectHash, std::uint16_t major = ll::kProtocolMajor) {
    ll::Hello h{};
    h.protocolMajor = major;
    h.role = static_cast<std::uint32_t>(ll::Role::Tool);
    h.projectBuildHash = hash;
    return Send(ll::MessageType::Hello, &h, sizeof(h));
  }
  // Reads what is available for up to `ms`. false once the stream closed.
  bool Pump(std::uint32_t ms) {
    if (t_ == nullptr) return false;
    t_->Wait(true, false, ms);
    for (;;) {
      std::uint8_t buf[4096];
      std::size_t got = 0;
      const ll::IoStatus st = t_->Recv(buf, sizeof(buf), &got);
      if (st == ll::IoStatus::Ok) {
        reader_.Feed(buf, got, *this);
        continue;
      }
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) {
        closed = true;
        return false;
      }
      return true;
    }
  }
  // Waits for a frame of `type` (consumes it). nullptr on timeout.
  const Frame* WaitFor(ll::MessageType type, std::uint32_t timeoutMs = 2000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      for (std::size_t i = taken_; i < frames.size(); ++i) {
        if (frames[i].header.type == static_cast<std::uint16_t>(type)) {
          last_ = frames[i];
          frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(i));
          return &last_;
        }
      }
      if (std::chrono::steady_clock::now() > end) return nullptr;
      if (!Pump(5) && closed) {
        // drain what arrived before the close, then give up
        bool any = false;
        for (const Frame& f : frames) any = any || f.header.type == static_cast<std::uint16_t>(type);
        if (!any) return nullptr;
      }
    }
  }
  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* p) noexcept override {
    frames.push_back(Frame{h, std::vector<std::uint8_t>(p, p + h.payloadBytes)});
  }
  void OnOversized(const ll::FrameHeader&) noexcept override {}

  std::vector<Frame> frames;
  bool closed = false;

 private:
  ll::IConnector* connector_ = nullptr;
  ll::ITransport* t_ = nullptr;
  ll::FrameReader reader_;
  Frame last_{};
  std::size_t taken_ = 0;
};

}  // namespace lltest

#endif  // SND_TESTS_LIVELINK_FIXTURE_HPP
