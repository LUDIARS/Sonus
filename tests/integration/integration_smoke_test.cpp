// Stage-0 placeholder for integration tests: the full library and the mocks
// link into one executable. Stage 2 replaces this with golden / determinism tests
// driven by NullOutputDevice::Pump().
#include "snd/api/compose.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/mocks/null_output_device.hpp"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

SND_TEST(Integration, FullLibraryAndMocksLink) {
  snd::mock::NullOutputDevice device;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  snd::Config cfg;
  cfg.output.device = &device;
  cfg.jobs.external = &jobs;
  snd::System* sys = nullptr;
  const snd::Result r = snd::Create(cfg, nullptr, &fs, &sys);
  SND_CHECK(r == snd::Result::Ok || r == snd::Result::NotImplemented);
  snd::Destroy(sys);
  unsigned n = 0;
  snd::compose::LinkedModules(&n);
  SND_CHECK(n > 0u);
}
