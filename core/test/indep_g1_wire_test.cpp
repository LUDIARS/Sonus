// B-1: verify the recorded bytes and independently replay the documented
// representation. Unequal x/y/z half extents make axis swaps observable.
#include <array>
#include <bit>
#include <memory>

#include "indep_g1_scene.hpp"
#include "indep_g2_fixture.hpp"

namespace {
using namespace indep_g1;
namespace cmd = snd::cmd;

struct Record {
  cmd::CommandType type{};
  cmd::SetParam param{};
  cmd::SetSwitch sw{};
};

struct Capture {
  void Read(const std::uint8_t* bytes, std::size_t size) noexcept {
    for (std::size_t offset = 0; offset < size;) {
      cmd::CommandHeader header;
      if (size - offset < sizeof(header)) { invalid = true; return; }
      std::memcpy(&header, bytes + offset, sizeof(header));
      if (header.sizeBytes < sizeof(header) || header.sizeBytes > size - offset) { invalid = true; return; }
      const auto type = static_cast<cmd::CommandType>(header.type);
      if (type == cmd::CommandType::SetParam || type == cmd::CommandType::SetSwitch) {
        if (count == records.size()) { invalid = true; return; }
        auto& record = records[count++];
        record.type = type;
        const auto* payload = bytes + offset + sizeof(header);
        if (type == cmd::CommandType::SetParam && header.sizeBytes >= sizeof(header) + sizeof(record.param))
          std::memcpy(&record.param, payload, sizeof(record.param));
        else if (type == cmd::CommandType::SetSwitch && header.sizeBytes >= sizeof(header) + sizeof(record.sw))
          std::memcpy(&record.sw, payload, sizeof(record.sw));
        else invalid = true;
      }
      offset += header.sizeBytes;
    }
  }
  std::array<Record, 8> records{};
  std::size_t count{};
  bool invalid{};
};

SND_TEST(IndepG1Wire, Defect_ShapeZoneAndClearRecordThePublishedWireFormat) {
  auto cfg = Config();
  Capture capture;
  snd::mock::DeterministicJobSystem jobs;
  snd::mock::MemoryFileSystem fs;
  snd::source::SourceKindSet kinds{};
  snd::core::CoreDesc desc{};
  desc.config = &cfg;
  desc.allocator = &snd::base::DefaultAllocator();
  desc.jobs = &jobs;
  desc.sources = &kinds;
  desc.bankFileSystem = &fs;
  desc.liveTap.user = &capture;
  desc.liveTap.arena = [](void* user, const snd::core::LiveArenaView& view) noexcept {
    auto& c = *static_cast<Capture*>(user);
    c.Read(view.normal, view.normalBytes);
    c.Read(view.reserve, view.reserveBytes);
  };
  snd::core::System* ptr{};
  SND_REQUIRE_EQ(snd::core::System::Create(desc, &ptr), SND_OK);
  std::unique_ptr<snd::core::System, decltype(&snd::core::System::Destroy)> sys(ptr, &snd::core::System::Destroy);
  const auto emitter = sys->CreateEmitter();
  const snd_vec3 extent{2.5f, 1.25f, 4.75f};
  SND_REQUIRE_EQ(sys->SetEmitterShape(emitter, SND_EMITTER_SHAPE_BOX, &extent), SND_OK);
  SND_REQUIRE_EQ(sys->SetReverbZone(emitter, kWet, 0.625f, 3.5f), SND_OK);
  SND_REQUIRE_EQ(sys->SetReverbZone(emitter, SND_ID_NONE, 0, 0), SND_OK);
  std::vector<float> block(static_cast<std::size_t>(sys->BlockFrames()) * sys->OutputChannels());
  sys->Update();
  sys->RenderBlock(block.data());
  SND_REQUIRE(!capture.invalid);
  // Check the records even if the implementation emits an extra value on clear.
  SND_CHECK_EQ(capture.count, 4u);
  SND_REQUIRE(capture.count >= 4);
  const auto& shape = capture.records[0];
  const auto& dest = capture.records[1];
  const auto& amount = capture.records[2];
  const auto& clear = capture.records[3];
  SND_CHECK_EQ(shape.type, cmd::CommandType::SetParam);
  SND_CHECK_EQ(shape.param.reserved[0], 2u);
  SND_CHECK_EQ(shape.param.reserved[1], SND_EMITTER_SHAPE_BOX);
  SND_CHECK_EQ(shape.param.scope, static_cast<std::uint8_t>(snd::ParamScope::Emitter));
  SND_CHECK_EQ(shape.param.target.Value(), emitter.value);
  SND_CHECK_EQ(shape.param.value, extent.x);
  SND_CHECK_EQ(shape.param.param, std::bit_cast<std::uint32_t>(extent.y));
  SND_CHECK_EQ(shape.param.reserved2, std::bit_cast<std::uint32_t>(extent.z));
  SND_CHECK_EQ(dest.type, cmd::CommandType::SetSwitch);
  SND_CHECK_EQ(dest.sw.reserved[0], 2u);
  SND_CHECK_EQ(dest.sw.scope, shape.param.scope);
  SND_CHECK_EQ(dest.sw.target.Value(), emitter.value);
  SND_CHECK_EQ(dest.sw.value, kWet);
  SND_CHECK_EQ(amount.type, cmd::CommandType::SetParam);
  SND_CHECK_EQ(amount.param.reserved[0], 3u);
  SND_CHECK_EQ(amount.param.scope, shape.param.scope);
  SND_CHECK_EQ(amount.param.target.Value(), emitter.value);
  SND_CHECK_EQ(amount.param.value, 0.625f);
  SND_CHECK_EQ(amount.param.reserved2, std::bit_cast<std::uint32_t>(3.5f));
  SND_CHECK_EQ(clear.type, cmd::CommandType::SetSwitch);
  SND_CHECK_EQ(clear.sw.reserved[0], 2u);
  SND_CHECK_EQ(clear.sw.scope, shape.param.scope);
  SND_CHECK_EQ(clear.sw.target.Value(), emitter.value);
  SND_CHECK_EQ(clear.sw.value, SND_ID_NONE);
}

SND_TEST(IndepG1Wire, Defect_ReplayedShapeUsesTheDocumentedHalfExtentAxes) {
  auto wave = [](bool replay) {
    g2::RawFx rig(Config());
    g2::FxBank bank;
    bank.Event("Tone", 300).flags = bf::kEvent3D;
    rig.Load("wire", bank.Build("Wire"));
    auto listener = Listener();
    SND_CHECK_EQ(rig.sys->SetListener(0, &listener), SND_OK);
    auto emitter = rig.sys->CreateEmitter();
    const snd_vec3 pos{4, 0, 0}, extent{2.5f, 1.25f, 4.75f};
    SND_CHECK_EQ(rig.sys->SetTransform(emitter, &pos, &kFront, nullptr), SND_OK);
    rig.Step();  // publish the emitter before submitting replay bytes
    if (replay) {
      cmd::SetParam param{};
      param.scope = static_cast<std::uint8_t>(snd::ParamScope::Emitter);
      param.target = snd::HandleRef::FromValue(emitter.value);
      param.reserved[0] = 2;
      param.reserved[1] = SND_EMITTER_SHAPE_LINE;
      param.value = extent.x;
      param.param = std::bit_cast<std::uint32_t>(extent.y);
      param.reserved2 = std::bit_cast<std::uint32_t>(extent.z);
      rig.Queue(cmd::CommandType::SetParam, param);
    } else {
      SND_CHECK_EQ(rig.sys->SetEmitterShape(emitter, SND_EMITTER_SHAPE_LINE, &extent), SND_OK);
    }
    rig.Step();
    rig.sys->Play(kEvent, emitter, nullptr);
    rig.Steps(6);
    return rig.out;
  };
  const auto reference = wave(false);
  SND_REQUIRE(std::any_of(reference.begin(), reference.end(), [](float v) { return v > 0.01f; }));
  SND_CHECK(reference == wave(true));
}
}  // namespace
