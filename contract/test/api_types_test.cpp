// Public API types: C/C++ layout identity, config defaults, conversions.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "snd/physics.hpp"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

extern "C" int snd_contract_c_check(void);  // c_api_check.c

SND_TEST(ApiTypes, CTranslationUnitAgrees) { SND_CHECK_EQ(snd_contract_c_check(), 0); }

SND_TEST(ApiTypes, ConfigDefaultsMatchBetweenCAndCpp) {
  snd_config c;
  snd_config_init(&c);
  snd::Config cpp;
  SND_CHECK_EQ(c.struct_size, sizeof(snd_config));
  SND_CHECK_EQ(c.contract_version, SND_CONTRACT_VERSION);
  // Every scalar field, generated from the same X-macro list.
#define SND_CMP_TOP(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.pn) == c.cn);
#define SND_CMP_MEM(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.memory.pn) == c.memory.cn);
#define SND_CMP_CMD(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.commands.pn) == c.commands.cn);
#define SND_CMP_JOB(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.jobs.pn) == c.jobs.cn);
#define SND_CMP_TUN(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.tuning.pn) == c.tuning.cn);
#define SND_CMP_DEV(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.devReserve.pn) == c.dev_reserve.cn);
#define SND_CMP_LL(cn, pn, ct, pt, d) SND_CHECK(static_cast<ct>(cpp.liveLink.pn) == c.livelink.cn);
  SND_CONFIG_TOP(SND_CMP_TOP)
  SND_CONFIG_MEMORY(SND_CMP_MEM)
  SND_CONFIG_COMMANDS(SND_CMP_CMD)
  SND_CONFIG_JOBS(SND_CMP_JOB)
  SND_CONFIG_TUNING(SND_CMP_TUN)
  SND_CONFIG_DEV_RESERVE(SND_CMP_DEV)
  SND_CONFIG_LIVELINK(SND_CMP_LL)
#undef SND_CMP_TOP
#undef SND_CMP_MEM
#undef SND_CMP_CMD
#undef SND_CMP_JOB
#undef SND_CMP_TUN
#undef SND_CMP_DEV
#undef SND_CMP_LL
  // Design defaults (13.2, 2.2, 2.3, 4.4, 3.1).
  SND_CHECK_EQ(cpp.sampleRate, 48000u);
  SND_CHECK_EQ(cpp.blockSize, 256u);
  SND_CHECK_EQ(cpp.renderAheadBlocks, 2u);
  SND_CHECK_EQ(cpp.commands.arenasPerThread, 3u);
  SND_CHECK_EQ(cpp.tuning.rtSpinMicroseconds, 50u);
  SND_CHECK_EQ(cpp.fadeReserveVoices, 8u);
  SND_CHECK_EQ(cpp.maxListeners, SND_MAX_LISTENERS);
  SND_CHECK(c.jobs.external == nullptr && c.output.device == nullptr);
}

SND_TEST(ApiTypes, CppConfigConvertsToC) {
  snd::Config cpp;
  cpp.sampleRate = 44100;
  cpp.memory.streamPoolBytes = 8u << 20;
  cpp.jobs.realtimeWorkers = 3;
  cpp.tuning.stealHysteresisDb = 4.5f;
  cpp.listenerMode = snd::ListenerMode::Split;
  cpp.output.deviceIndex = 2;
  snd_config c;
  snd::detail::ToC(cpp, &c);
  SND_CHECK_EQ(c.sample_rate, 44100u);
  SND_CHECK_EQ(c.memory.stream_pool_bytes, 8ull << 20);
  SND_CHECK_EQ(c.jobs.realtime_workers, 3u);
  SND_CHECK_EQ(c.tuning.steal_hysteresis_db, 4.5f);
  SND_CHECK_EQ(c.listener_mode, SND_LISTENER_SPLIT);
  SND_CHECK_EQ(c.output.device_index, 2u);
}

SND_TEST(ApiTypes, HandlesPackSlotAndGeneration) {
  const std::uint64_t v = SND_HANDLE_MAKE(5u, 9u);
  SND_CHECK_EQ(SND_HANDLE_SLOT(v), 5u);
  SND_CHECK_EQ(SND_HANDLE_GENERATION(v), 9u);
  snd::PlaybackHandle h{v};
  SND_CHECK(h.IsValid());
  SND_CHECK_EQ(h.Slot(), 5u);
  SND_CHECK_EQ(h.Generation(), 9u);
  SND_CHECK(!snd::PlaybackHandle{}.IsValid());
  SND_CHECK(!snd::kNoEmitter.IsValid());
  SND_CHECK(snd::PlaybackHandle::FromC(h.ToC()) == h);
  static_assert(std::is_trivially_copyable_v<snd::PlaybackHandle>);
  static_assert(sizeof(snd::EmitterHandle) == 8);
}

SND_TEST(ApiTypes, WhenFactoriesFillEveryKind) {
  const snd::TransportHandle t{SND_HANDLE_MAKE(1u, 1u)};
  SND_CHECK(snd::When().Kind() == snd::WhenKind::Immediate);
  const snd::When bar = snd::When::NextBar(t);
  SND_CHECK_EQ(bar.ToC().kind, SND_WHEN_NEXT_BAR);
  SND_CHECK_EQ(bar.ToC().transport.value, t.value);
  SND_CHECK_EQ(snd::When::BarsAfter(t, 4).ToC().count, 4u);
  SND_CHECK_EQ(snd::When::Grid(t, 16, -240).ToC().offset_ticks, -240);
  SND_CHECK_EQ(snd::When::NextMarker(t, 77u).ToC().value, 77u);
  SND_CHECK_EQ(snd::When::SegmentEnd(t).ToC().kind, SND_WHEN_SEGMENT_END);
  SND_CHECK_EQ(snd::When::AtClockSample(123456u).ToC().value, 123456u);
  SND_CHECK_EQ(snd::When::AfterMicroseconds(5000u).ToC().kind, SND_WHEN_DELAY_US);
  SND_CHECK_EQ(snd::When::NextBeat(t).WithPickupAlign().ToC().flags, SND_WHEN_FLAG_PICKUP_ALIGN);
  const snd::When beat = snd::When::NextBeat(t);
  for (std::uint8_t r : beat.ToC().reserved) SND_CHECK_EQ(r, 0u);
}

SND_TEST(ApiTypes, EnumValuesMirrorC) {
  static_assert(static_cast<int>(snd::Result::NotReady) == SND_ERR_NOT_READY);
  static_assert(static_cast<unsigned>(snd::PlaybackState::Ended) == SND_PLAYBACK_STATE_ENDED);
  static_assert(static_cast<unsigned>(snd::EndReason::NotLoaded) == SND_END_NOT_LOADED);
  static_assert(static_cast<unsigned>(snd::MemTag::Count) == SND_MEM_TAG_COUNT);
  static_assert(static_cast<unsigned>(snd::phys::ContactKind::Break) == SND_CONTACT_BREAK);
  SND_CHECK(true);
}

namespace {
struct CountingAllocator final : snd::IAllocator {
  int allocs = 0, frees = 0;
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag) override {
    ++allocs;
    return std::aligned_alloc(align, (bytes + align - 1) / align * align);
  }
  void Free(void* p, snd::MemTag) override {
    ++frees;
    std::free(p);
  }
};
}  // namespace

SND_TEST(ApiTypes, BridgesRoundTrip) {
  CountingAllocator a;
  snd_allocator c = snd::ToC(a);
  void* p = c.allocate(c.user, 100, 64, SND_MEM_BANK);
  SND_CHECK(p != nullptr && reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
  snd::CAllocator back(c);
  back.Free(p, snd::MemTag::Bank);
  SND_CHECK_EQ(a.allocs, 1);
  SND_CHECK_EQ(a.frees, 1);
}

SND_TEST(ApiTypes, PodSizes) {
  static_assert(sizeof(snd_when) == 32);
  static_assert(sizeof(snd_contact) == 64);
  static_assert(sizeof(snd_vec3) == 12);
  static_assert(std::is_trivially_copyable_v<snd_config>);
  static_assert(std::is_standard_layout_v<snd_config>);
  static_assert(offsetof(snd_config, struct_size) == 0);
  SND_CHECK(true);
}
