#include <array>
#include <atomic>
#include <thread>
#include "indep_src2_util.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace indep2;

SND_TEST(IndepSrc2, ConcurrentChunksRetainTheirWaveformsAndBankViews) {
  std::vector<float> pcm(160);
  for (unsigned i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<float>(std::sin(double(i)));
  auto a = Pcm(pcm, 48000);
  src::GranularParams g{&a, 9000, 1, 0.4f, 500, src::GrainWindow::Triangle, 0.4f};
  auto bytes = Program({{PatchOp::WhiteNoise}, {PatchOp::Output, 0}});
  auto p = Patch(bytes);
  for (auto kind : {snd::SourceKind::Granular, snd::SourceKind::Patch}) {
    const void* params = kind == snd::SourceKind::Patch ? static_cast<const void*>(&p) : &g;
    Driver d(kind, {});
    std::array<Driver::Voice, 2> workers, refs;
    std::array<std::vector<float>, 2> result;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::array<std::thread, 2> threads;
    for (unsigned j = 0; j < 2; ++j) {
      SND_REQUIRE(d.Start(workers[j], params, 1, 90 + j));
      SND_REQUIRE(d.Start(refs[j], params, 1, 90 + j));
    }
    for (unsigned j = 0; j < 2; ++j) {
      threads[j] = std::thread([&, j] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 256; ++i) {
          auto samples = Item(d, workers[j], 67);
          result[j].insert(result[j].end(), samples.begin(), samples.end());
        }
      });
    }
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    bool valid = true;
    for (unsigned i = 0; i < 1800; ++i) {
      Driver::Voice other;
      if (!d.Start(other, params, 1, i)) {
        valid = false;
        break;
      }
      d.Control(other, src::ControlOp::Seek, i * 33);
      {
        snd::rt::RtScope rt;
        d.Kind().ScheduleBackground({});
        // Unregistering notifies the cache; resident data remains held by the playing bank.
        src::Runtime::BankUnregistered(&d.runtime, 0);
      }
      d.Release(other);
    }
    for (auto& thread : threads) thread.join();
    SND_CHECK(valid);
    for (unsigned j = 0; j < 2; ++j) {
      d.Render(refs[j], 256 * 67, {}, 139);
      SND_CHECK(result[j] == refs[j].out[0]);
    }
    SND_CHECK(result[0] != result[1]);
    SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  }
}
