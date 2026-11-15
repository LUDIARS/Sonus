// Command binary representation (design.md 2.1).
#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/internal/commands.hpp"
#include "snd/internal/notify.hpp"
#include "snd/test/test.hpp"

namespace cmd = snd::cmd;

namespace {
template <class P>
void Append(std::vector<std::uint8_t>& buf, const P& payload, std::uint32_t seq) {
  const std::uint32_t size = cmd::CommandTraits<P>::kSize;
  const std::size_t at = buf.size();
  buf.resize(at + size, 0);
  cmd::CommandHeader h{static_cast<std::uint16_t>(cmd::CommandTraits<P>::kType),
                       static_cast<std::uint16_t>(size), seq};
  std::memcpy(buf.data() + at, &h, sizeof(h));
  if constexpr (!std::is_empty_v<P>) std::memcpy(buf.data() + at + sizeof(h), &payload, sizeof(P));
}
}  // namespace

SND_TEST(Commands, InfoTableIsConsistent) {
  for (std::uint16_t t = 1; t < static_cast<std::uint16_t>(cmd::CommandType::Count); ++t) {
    const cmd::CommandInfo* info = cmd::FindCommandInfo(t);
    SND_REQUIRE(info != nullptr);
    SND_CHECK_EQ(static_cast<std::uint16_t>(info->type), t);
    SND_CHECK_EQ(info->wireSize % cmd::kAlign, 0u);
    SND_CHECK(info->wireSize >= sizeof(cmd::CommandHeader));
    SND_CHECK(info->wireSize <= cmd::kMaxCommandBytes);
  }
  SND_CHECK(cmd::FindCommandInfo(0) == nullptr);
  SND_CHECK(cmd::FindCommandInfo(static_cast<std::uint16_t>(cmd::CommandType::Count)) == nullptr);
}

SND_TEST(Commands, DeliveryClassesFollowTheDesign) {
  // 2.1: stop, parameter and release commands must be delivered.
  static_assert(cmd::CommandTraits<cmd::Stop>::kDelivery == cmd::Delivery::MustDeliver);
  static_assert(cmd::CommandTraits<cmd::SetParam>::kDelivery == cmd::Delivery::MustDeliver);
  static_assert(cmd::CommandTraits<cmd::SetSwitch>::kDelivery == cmd::Delivery::MustDeliver);
  static_assert(cmd::CommandTraits<cmd::DestroyEmitter>::kDelivery == cmd::Delivery::MustDeliver);
  static_assert(cmd::CommandTraits<cmd::DestroyTransport>::kDelivery == cmd::Delivery::MustDeliver);
  static_assert(cmd::CommandTraits<cmd::DestroyUserBuffer>::kDelivery == cmd::Delivery::MustDeliver);
  static_assert(cmd::CommandTraits<cmd::BankUnload>::kDelivery == cmd::Delivery::MustDeliver);
  // Play requests are the ones rejected first.
  static_assert(cmd::CommandTraits<cmd::Play>::kDelivery == cmd::Delivery::Play);
  static_assert(cmd::CommandTraits<cmd::BankRegister>::kOrigin == cmd::Origin::Loader);
  SND_CHECK(true);
}

SND_TEST(Commands, StreamRoundTripsAndRejectsGarbage) {
  std::vector<std::uint8_t> buf;
  cmd::Play play{};
  play.playback = snd::HandleRef{3, 1};
  play.event = 0xABCDu;
  Append(buf, play, 1);
  cmd::SetParam sp{};
  sp.scope = static_cast<std::uint8_t>(snd::ParamScope::Playback);
  sp.param = 7;
  sp.target = play.playback;
  sp.value = 0.25f;
  Append(buf, sp, 2);
  Append(buf, cmd::Suspend{}, 3);

  std::vector<std::uint16_t> types;
  float seenValue = 0.0f;
  const bool ok = cmd::ForEachCommand(buf.data(), buf.data() + buf.size(),
                                      [&](const cmd::CommandHeader& h, const std::uint8_t* payload) {
                                        types.push_back(h.type);
                                        if (h.type == static_cast<std::uint16_t>(cmd::CommandType::SetParam)) {
                                          cmd::SetParam p;
                                          std::memcpy(&p, payload, sizeof(p));
                                          seenValue = p.value;
                                        }
                                      });
  SND_CHECK(ok);
  SND_REQUIRE_EQ(types.size(), 3u);
  SND_CHECK_EQ(types[0], static_cast<std::uint16_t>(cmd::CommandType::Play));
  SND_CHECK_EQ(seenValue, 0.25f);
  SND_CHECK_EQ(buf.size(), 80u + 32u + 8u);

  std::vector<std::uint8_t> bad = buf;
  bad[2] = 9;  // wrong size of the first command
  SND_CHECK(!cmd::ForEachCommand(bad.data(), bad.data() + bad.size(),
                                 [](const cmd::CommandHeader&, const std::uint8_t*) {}));
  std::vector<std::uint8_t> truncated(buf.begin(), buf.begin() + 40);
  SND_CHECK(!cmd::ForEachCommand(truncated.data(), truncated.data() + truncated.size(),
                                 [](const cmd::CommandHeader&, const std::uint8_t*) {}));
}

SND_TEST(Notify, RecordLayouts) {
  static_assert(sizeof(snd::notify::EventRecord) == 48);
  static_assert(sizeof(snd::notify::EndRecord) == 8);
  static_assert(sizeof(snd::cmd::ArenaHeader) == 64);
  SND_CHECK(true);
}
