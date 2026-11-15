// Independent re-check of D-54 (c65fd9a): the runtime's DSP pool reservation.
//  - compute set: reserve() gets exactly dsp::ComputeDspPoolNeed over descs built
//    HERE from the bank (EFFX/EFPV/EFBL, bus channels else nearest parent else
//    outputChannels, blockSize, sampleRate), for several Config values; the
//    bank's DSPN stays in builtDspNeed; release() on unload returns the same need.
//  - compute not set: the bank's DSPN is reserved (fallback).
//  - compute refuses: FAILED / SND_ERR_INVALID_ARGUMENT, reserve() never called, no leak.
//  - reserve refuses: FAILED / SND_ERR_OUT_OF_BUDGET, nothing reserved, no leak.
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pipeline_util.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace bk = snd::bank;
namespace bf = snd::bank_format;

namespace {

class Counting final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag) override {
    void* p = nullptr;
    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, bytes == 0 ? 1 : bytes) != 0) return nullptr;
    std::lock_guard<std::mutex> l(m_);
    live_.insert(p);
    return p;
  }
  void Free(void* p, snd::MemTag) override {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> l(m_);
    live_.erase(p);
    std::free(p);
  }
  std::size_t Live() {
    std::lock_guard<std::mutex> l(m_);
    return live_.size();
  }

 private:
  std::mutex m_;
  std::set<void*> live_;
};

struct Pool {
  std::uint64_t bytes = 0;
  std::uint32_t count = 0;
  std::uint32_t reserveCalls = 0, computeCalls = 0;
  bool refuseCompute = false, refuseReserve = false;
};
bool Reserve(void* u, const bf::DspPoolNeed& n) {
  auto* p = static_cast<Pool*>(u);
  ++p->reserveCalls;
  if (p->refuseReserve) return false;
  p->bytes += n.instanceBytes;
  p->count += n.instanceCount;
  return true;
}
void Release(void* u, const bf::DspPoolNeed& n) {
  auto* p = static_cast<Pool*>(u);
  p->bytes -= n.instanceBytes;
  p->count -= n.instanceCount;
}
bool Compute(void* u, const snd_effect_desc* d, std::uint32_t n, std::uint32_t rate, bf::DspPoolNeed* out) {
  auto* p = static_cast<Pool*>(u);
  ++p->computeCalls;
  if (p->refuseCompute) return false;
  *out = snd::dsp::ComputeDspPoolNeed(d, n, rate, nullptr, 0);
  return true;
}

std::uint32_t Channels(const bk::BankView& v, std::uint32_t row, std::uint32_t output) {
  for (int depth = 0; row != bf::kNone && depth < 64; ++depth) {
    const std::uint32_t ch = v.Column<bk::col::BUSS_channels>()[row];
    if (ch != 0) return ch;
    row = v.Column<bk::col::BUSS_parent>()[row];
  }
  return output;
}

struct Loaded {
  snd_bank_state state;
  snd_result result;
};

Loaded Load(const std::vector<std::uint8_t>& bytes, Pool& pool, bool compute, std::uint32_t rate, std::uint32_t block,
            std::uint32_t outCh, bf::DspPoolNeed* reservedNeed, bf::DspPoolNeed* builtNeed, std::size_t* leaked) {
  Counting alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  fs.AddFile("Main.sndb", bytes);
  bk::BankManagerDesc md;
  md.allocator = &alloc;
  md.fileSystem = &fs;
  md.ioMode = bk::BankIoMode::DirectFileSystem;
  md.jobs = &jobs;
  md.dsp.user = &pool;
  md.dsp.reserve = &Reserve;
  md.dsp.release = &Release;
  md.dsp.compute = compute ? &Compute : nullptr;
  md.sampleRate = rate;
  md.blockSize = block;
  md.outputChannels = outCh;
  bk::BankManager* m = nullptr;
  if (bk::BankManager::Create(md, &m) != SND_OK) return {SND_BANK_STATE_FAILED, SND_ERR_INVALID_ARGUMENT};
  const snd::HandleRef h = m->RequestLoad("Main.sndb");
  snd::cmd::BankRegister reg{};
  for (int i = 0; i < 32; ++i) {
    m->Update();
    jobs.RunAll();
    while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
  }
  const Loaded out{m->State(h), m->Result(h)};
  if (const bk::PreparedBank* pb = m->Prepared(h)) {
    *reservedNeed = pb->dspNeed;
    *builtNeed = pb->builtDspNeed;
  }
  if (out.state == SND_BANK_STATE_LOADED) {
    m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
    m->Release(h);
  }
  bk::BankManager::Destroy(m);
  *leaked = alloc.Live();
  return out;
}

void FxProject(mini::Project& proj) {
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master",
           "\"master\": true, \"effects\": [{\"type\": \"snd.fx.limiter\", \"params\": [-1, 50, 4]}]");
  proj.Add("buses/Room.json", "bus", "Room",
           "\"bank\": \"Main\", \"parent\": \"Master\", \"effects\": [{\"type\": \"snd.fx.reverb\"}, {\"type\": \"snd.fx.echo\", \"params\": [200, 0.3, 1, 1, 2500]}]");
  proj.Add("buses/Quad.json", "bus", "Quad", "\"bank\": \"Main\", \"parent\": \"Master\", \"channels\": 4");
  proj.Add("buses/QuadKid.json", "bus", "QuadKid",
           "\"bank\": \"Main\", \"parent\": \"Quad\", \"effects\": [{\"type\": \"snd.fx.delay\", \"params\": [50, 0.2, 0, -6, 20000, 0, 3000]}]");
  proj.Wav("audio/a.wav", mini::Tone(1, 48000, 480));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\"");
  proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
}

}  // namespace

SND_TEST(IndepDspReserve11, ReservationIsTheRuntimeNeedForTheConfig) {
  mini::Project proj("indep-dsp-reserve");
  FxProject(proj);
  bt::Diagnostics d;
  SND_REQUIRE(proj.Build(&d));
  const std::vector<std::uint8_t> bytes = mini::ReadBytes(proj.out() / "Main.sndb");
  pipeline::ReadBank bank(bytes);
  SND_REQUIRE(bank.ok);
  struct Cfg {
    std::uint32_t rate, block, out;
  };
  for (const Cfg c : {Cfg{48000, 256, 2}, Cfg{44100, 1024, 6}, Cfg{96000, 64, 8}, Cfg{32000, 480, 1}}) {
    std::vector<snd::EffectDesc> descs;
    std::vector<std::vector<float>> params;
    params.reserve(16);
    for (std::uint32_t b = 0; b < bank.view.Rows(bf::chunk::BUSS); ++b) {
      const auto fx = pipeline::BusEffectDescs(bank.view, b, Channels(bank.view, b, c.out), c.rate, c.block, &params);
      descs.insert(descs.end(), fx.begin(), fx.end());
    }
    SND_REQUIRE_EQ(descs.size(), 4u);
    const bf::DspPoolNeed want =
        snd::dsp::ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()), c.rate, nullptr, 0);
    Pool pool;
    bf::DspPoolNeed reserved{}, built{};
    std::size_t leaked = 99;
    const Loaded r = Load(bytes, pool, true, c.rate, c.block, c.out, &reserved, &built, &leaked);
    SND_CHECK_MSG(r.state == SND_BANK_STATE_UNLOADED || r.state == SND_BANK_STATE_LOADED, "state %u (%d)", r.state,
                  r.result);
    SND_CHECK_EQ(pool.computeCalls, 1u);
    SND_CHECK_EQ(pool.reserveCalls, 1u);
    SND_CHECK_MSG(reserved.instanceBytes == want.instanceBytes && reserved.instanceCount == want.instanceCount,
                  "rate %u block %u out %u: reserved %llu/%u, dsp %llu/%u", c.rate, c.block, c.out,
                  static_cast<unsigned long long>(reserved.instanceBytes), reserved.instanceCount,
                  static_cast<unsigned long long>(want.instanceBytes), want.instanceCount);
    SND_CHECK_EQ(built.instanceBytes, bank.view.DspNeed().instanceBytes);
    SND_CHECK_EQ(pool.bytes, 0u);  // release() on unload gave back exactly what was reserved
    SND_CHECK_EQ(pool.count, 0u);
    SND_CHECK_EQ(leaked, 0u);
  }
}

SND_TEST(IndepDspReserve11, FallbackAndRefusals) {
  mini::Project proj("indep-dsp-refuse");
  FxProject(proj);
  bt::Diagnostics d;
  SND_REQUIRE(proj.Build(&d));
  const std::vector<std::uint8_t> bytes = mini::ReadBytes(proj.out() / "Main.sndb");
  pipeline::ReadBank bank(bytes);
  SND_REQUIRE(bank.ok);
  {  // compute not set: the bank's DSPN, whatever the Config says
    Pool pool;
    bf::DspPoolNeed reserved{}, built{};
    std::size_t leaked = 99;
    (void)Load(bytes, pool, false, 96000, 64, 8, &reserved, &built, &leaked);
    SND_CHECK_EQ(pool.computeCalls, 0u);
    SND_CHECK_EQ(reserved.instanceBytes, bank.view.DspNeed().instanceBytes);
    SND_CHECK_EQ(reserved.instanceCount, bank.view.DspNeed().instanceCount);
    SND_CHECK_EQ(leaked, 0u);
  }
  {  // compute refuses
    Pool pool;
    pool.refuseCompute = true;
    bf::DspPoolNeed reserved{}, built{};
    std::size_t leaked = 99;
    const Loaded r = Load(bytes, pool, true, 48000, 256, 2, &reserved, &built, &leaked);
    SND_CHECK_EQ(r.state, SND_BANK_STATE_FAILED);
    SND_CHECK_EQ(r.result, SND_ERR_INVALID_ARGUMENT);
    SND_CHECK_EQ(pool.reserveCalls, 0u);
    SND_CHECK_EQ(leaked, 0u);
  }
  {  // reserve refuses
    Pool pool;
    pool.refuseReserve = true;
    bf::DspPoolNeed reserved{}, built{};
    std::size_t leaked = 99;
    const Loaded r = Load(bytes, pool, true, 48000, 256, 2, &reserved, &built, &leaked);
    SND_CHECK_EQ(r.state, SND_BANK_STATE_FAILED);
    SND_CHECK_EQ(r.result, SND_ERR_OUT_OF_BUDGET);
    SND_CHECK_EQ(pool.bytes, 0u);
    SND_CHECK_EQ(leaked, 0u);
  }
}
