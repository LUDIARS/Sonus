// Fixed-capacity open-addressing map Id -> u32 (core-private indexes: bus /
// category / snapshot / limit-group / parameter ids, A1-16). Linear probing
// with backward-shift deletion (no tombstones). Keys are non-zero ids.
// Memory is bound at Create; every operation is RT-safe and O(1) expected.
#ifndef SND_CORE_SRC_ID_MAP_HPP
#define SND_CORE_SRC_ID_MAP_HPP

#include <cstdint>

#include "snd/base/hash.hpp"
#include "snd/id.hpp"

namespace snd::core {

class IdMap {
 public:
  static constexpr std::uint32_t kNotFound = 0xFFFFFFFFu;

  // `storage` holds 2 * capacity u32 (capacity: power of two >= 2 * live entries).
  void Bind(std::uint32_t* storage, std::uint32_t capacity) noexcept {
    keys_ = storage;
    values_ = storage + capacity;
    mask_ = capacity - 1u;
    count_ = 0;
    for (std::uint32_t i = 0; i < capacity; ++i) keys_[i] = 0u;
  }
  static std::uint32_t CapacityFor(std::uint32_t entries) noexcept {
    std::uint32_t c = 16;
    while (c < entries * 2u) c <<= 1u;
    return c;
  }

  std::uint32_t Find(Id key) const noexcept {
    if (key == 0u || keys_ == nullptr) return kNotFound;
    for (std::uint32_t i = Home(key);; i = (i + 1u) & mask_) {
      if (keys_[i] == key) return values_[i];
      if (keys_[i] == 0u) return kNotFound;
    }
  }
  // Inserts or overwrites. false when full (more than capacity / 2 entries).
  bool Set(Id key, std::uint32_t value) noexcept {
    if (key == 0u || keys_ == nullptr) return false;
    for (std::uint32_t i = Home(key);; i = (i + 1u) & mask_) {
      if (keys_[i] == key) {
        values_[i] = value;
        return true;
      }
      if (keys_[i] == 0u) {
        if ((count_ + 1u) * 2u > mask_ + 1u) return false;
        keys_[i] = key;
        values_[i] = value;
        ++count_;
        return true;
      }
    }
  }
  void Erase(Id key) noexcept {
    if (key == 0u || keys_ == nullptr) return;
    std::uint32_t i = Home(key);
    for (;; i = (i + 1u) & mask_) {
      if (keys_[i] == 0u) return;
      if (keys_[i] == key) break;
    }
    // Backward-shift deletion.
    std::uint32_t hole = i;
    for (std::uint32_t j = (i + 1u) & mask_; keys_[j] != 0u; j = (j + 1u) & mask_) {
      const std::uint32_t h = Home(keys_[j]);
      // Move j into the hole if its home is not in (hole, j] (cyclically).
      const bool between = hole <= j ? (h > hole && h <= j) : (h > hole || h <= j);
      if (!between) {
        keys_[hole] = keys_[j];
        values_[hole] = values_[j];
        hole = j;
      }
    }
    keys_[hole] = 0u;
    --count_;
  }
  std::uint32_t Count() const noexcept { return count_; }

 private:
  std::uint32_t Home(Id key) const noexcept { return base::MixU32(key) & mask_; }
  std::uint32_t* keys_ = nullptr;
  std::uint32_t* values_ = nullptr;
  std::uint32_t mask_ = 0;
  std::uint32_t count_ = 0;
};

}  // namespace snd::core

#endif  // SND_CORE_SRC_ID_MAP_HPP
