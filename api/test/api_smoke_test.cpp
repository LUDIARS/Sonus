// Stage-0 tests of the api module: every C function links, the C++ wrapper
// forwards to it, and the pure contract functions behave as specified.
#include <cstring>

#include "snd/api/api_module.hpp"
#include "snd/api/compose.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/mocks/null_output_device.hpp"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

using namespace snd::literals;

SND_TEST(Api, ModuleLinks) { SND_CHECK(std::strcmp(snd::api::ModuleName(), "api") == 0); }

SND_TEST(Api, ContractVersionMatchesHeaders) {
  SND_CHECK_EQ(snd::ContractVersion(), snd::kContractVersion);
}

SND_TEST(Api, CompositionRootLinksAllRuntimeModules) {
  unsigned n = 0;
  const char* const* names = snd::compose::LinkedModules(&n);
  SND_CHECK_EQ(n, 12u);
  SND_CHECK(std::strcmp(names[0], "base") == 0);
}

SND_TEST(Api, CreateValidatesArgumentsAndIsNotImplementedYet) {
  snd::mock::NullOutputDevice device;
  snd::mock::MemoryFileSystem fs;
  snd::Config cfg;
  cfg.output.device = &device;
  snd::System* sys = reinterpret_cast<snd::System*>(&cfg);  // must be reset to null
  const snd::Result r = snd::Create(cfg, nullptr, &fs, &sys);
  SND_CHECK(r == snd::Result::NotImplemented || r == snd::Result::Ok);
  if (r != snd::Result::Ok) SND_CHECK(sys == nullptr);
  snd::Destroy(sys);

  SND_CHECK_EQ(snd_create(nullptr, nullptr, nullptr, &sys), SND_ERR_INVALID_ARGUMENT);
  snd_config bad;
  snd_config_init(&bad);
  bad.struct_size = 4;
  SND_CHECK_EQ(snd_create(&bad, nullptr, nullptr, &sys), SND_ERR_VERSION_MISMATCH);
}

SND_TEST(Api, PlaybackTransportHandleIsDerived) {
  const snd::PlaybackHandle p{SND_HANDLE_MAKE(12u, 34u)};
  const snd::TransportHandle t = snd::GetPlaybackTransport(p);
  SND_CHECK_EQ(t.Slot(), 12u | SND_TRANSPORT_PLAYBACK_BIT);
  SND_CHECK_EQ(t.Generation(), 34u);
  SND_CHECK(!snd::GetPlaybackTransport(snd::PlaybackHandle{}).IsValid());
}

SND_TEST(Api, WrapperCallsCompile) {
  // Mirrors design.md 13.2 against the stubs (sys == nullptr: every call is a no-op).
  snd::System* sys = nullptr;
  const snd::EmitterHandle e = snd::CreateEmitter(sys);
  snd::SetTransform(sys, e, snd::Vec3{0, 0, 0}, snd::Vec3{0, 0, 1}, snd::Vec3{0, 0, 0});
  const snd::PlaybackHandle p = snd::Play(sys, "SE/Explosion"_id, e);
  snd::SetParam(sys, p, "Size"_id, 0.8f);
  const snd::TransportHandle musicTransport = snd::GetPlaybackTransport(p);
  snd::Play(sys, "Music/Stinger"_id, snd::kNoEmitter, snd::When::NextBar(musicTransport));
  snd::SetMusicState(sys, p, "Battle"_id);
  snd::SynthDesc sd{};
  sd.callback = [](void*, float* const*, std::uint32_t, std::uint32_t, const snd::RenderContext&) {};
  sd.channels = 1;
  sd.mode = snd::SynthMode::Buffered;
  snd::PlaySynth(sys, sd, e);
  SND_CHECK(snd::GetPlaybackState(sys, p) == snd::PlaybackState::None);
  SND_CHECK(snd::Update(sys) != snd::Result::Ok);
}
