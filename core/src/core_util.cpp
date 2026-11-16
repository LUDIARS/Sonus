// Shared helpers: event records for Publish, bank / event lookup, curves,
// and the thread-local registration of producer threads.
#include <cmath>

#include "core_impl.hpp"

namespace snd::core {

void PostEvent(Impl& im, notify::EventType type, std::uint32_t a, std::uint64_t object, std::uint16_t detail,
               std::uint32_t b) noexcept {
  if (im.eventCount >= im.eventCap) {
    ++im.stats.dropped_events;
    return;
  }
  notify::EventRecord& r = im.events[im.eventCount++];
  r = notify::EventRecord{};
  r.type = static_cast<std::uint16_t>(type);
  r.detail = detail;
  r.a = a;
  r.object = object;
  r.clock = im.clock;
  r.b = b;
}

const BankRecord* RegisteredBank(const Impl& im, std::uint16_t bankSlot) noexcept {
  if (bankSlot >= im.cfg.maxBanks) return nullptr;
  const BankAudio& b = im.ap.banks[bankSlot];
  return (b.state == BankAudioState::Registered || b.state == BankAudioState::Unloading) ? b.rec : nullptr;
}

namespace {
std::uint32_t FindEventRow(const BankRecord& r, Id id) noexcept {
  std::uint32_t lo = 0, hi = r.eventCount;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2u;
    if (r.eventIds[mid] < id) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return (lo < r.eventCount && r.eventIds[lo] == id) ? r.eventRows[lo] : bf::kNone;
}
}  // namespace

// Language banks hold the same event ids (10.4): the bank of the current
// language wins, then banks without a language, then any; ties by registration order.
EventRef ResolveEvent(const Impl& im, Id event) noexcept {
  EventRef best{kNoBank, 0, 0};
  int bestRank = -1;
  std::uint32_t bestOrder = 0;
  for (int pass = 0; pass < 2 && bestRank < 0; ++pass) {
    for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
      const BankAudio& b = im.ap.banks[s];
      if (b.state != BankAudioState::Registered || b.rec == nullptr) continue;  // unloading banks: no new plays
      Id id = event;
      if (pass == 1) {  // renamed objects (ALIS, 10.1)
        id = b.rec->view->ResolveAlias(event);
        if (id == 0u) continue;
      }
      const std::uint32_t row = FindEventRow(*b.rec, id);
      if (row == bf::kNone) continue;
      int rank = 0;
      if (b.rec->languageId == 0u) {
        rank = 1;
      } else if (b.rec->languageId == im.ap.language) {
        rank = 2;
      }
      if (rank > bestRank || (rank == bestRank && b.order < bestOrder)) {
        bestRank = rank;
        bestOrder = b.order;
        best = EventRef{static_cast<std::uint16_t>(s), 0, row};
      }
    }
  }
  if (bestRank >= 0) {
    const bk::BankView& v = *im.ap.banks[best.bankSlot].rec->view;
    if ((v.Column<bk::col::EVNT_flags>()[best.eventIndex] & bf::kEventMusic) != 0u) best.flags |= kEventRefMusic;
  }
  return best;
}

float EvaluateCurve(const bk::BankView& v, std::uint32_t curveRow, float x) noexcept {
  if (curveRow >= v.Rows(bf::chunk::CURV)) return 0.0f;
  const std::uint32_t first = v.Column<bk::col::CURV_pointFirst>()[curveRow];
  const std::uint32_t count = v.Column<bk::col::CURV_pointCount>()[curveRow];
  if (count == 0u) return 0.0f;
  const auto px = v.Column<bk::col::CPNT_x>();
  const auto py = v.Column<bk::col::CPNT_y>();
  const auto ps = v.Column<bk::col::CPNT_shape>();
  if (x <= px[first] || count == 1u) return py[first];
  for (std::uint32_t i = 1; i < count; ++i) {
    const std::uint32_t r = first + i;
    if (x <= px[r]) {
      const float x0 = px[r - 1u], x1 = px[r];
      const float y0 = py[r - 1u], y1 = py[r];
      float tt = x1 > x0 ? (x - x0) / (x1 - x0) : 1.0f;
      const float shape = ps[r - 1u];
      if (shape > 0.0f && shape != 1.0f) tt = std::pow(tt, shape);
      return y0 + (y1 - y0) * tt;
    }
  }
  return py[first + count - 1u];
}

// ---------------------------------------------------------------------------
// Thread registration (thread-local, per System serial)
// ---------------------------------------------------------------------------
namespace {
struct TlsReg {
  std::uint64_t serial;
  std::uint32_t producer;
  std::uint32_t reserved;
};
constexpr std::uint32_t kTlsEntries = 16;
thread_local TlsReg tRegs[kTlsEntries];
}  // namespace

std::uint32_t CurrentProducer(const Impl& im) noexcept {
  for (const TlsReg& r : tRegs) {
    if (r.serial == im.serial) return r.producer;
  }
  return kNoSlot;
}
bool IsOwnerThread(const Impl& im) noexcept { return CurrentProducer(im) == 0u; }

bool TlsRegister(const Impl& im, std::uint32_t producer) noexcept {
  for (TlsReg& r : tRegs) {
    if (r.serial == 0u) {
      r.serial = im.serial;
      r.producer = producer;
      return true;
    }
  }
  return false;
}
void TlsUnregister(const Impl& im) noexcept {
  for (TlsReg& r : tRegs) {
    if (r.serial == im.serial) r = TlsReg{};
  }
}

}  // namespace snd::core
