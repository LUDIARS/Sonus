// ALSA device enumeration (D-64): snd_device_name_hint over the "pcm"
// interface; entries whose IOID is unset (both directions) or "Output".
#include <alsa/asoundlib.h>

#include <cstdlib>
#include <cstring>

#include "backends.hpp"
#include "common/owned_string.hpp"

namespace snd::platform::detail {

std::uint32_t EnumerateAlsaDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept {
  void** hints = nullptr;
  if (snd_device_name_hint(-1, "pcm", &hints) < 0 || hints == nullptr) return 0;
  std::uint32_t count = 0;
  for (void** h = hints; *h != nullptr; ++h) {
    char* name = snd_device_name_get_hint(*h, "NAME");
    char* desc = snd_device_name_get_hint(*h, "DESC");
    char* ioid = snd_device_name_get_hint(*h, "IOID");
    const bool output = ioid == nullptr || std::strcmp(ioid, "Output") == 0;
    if (name != nullptr && output) {
      if (count < capacity) {
        OutputDeviceInfo& info = out[count];
        info = OutputDeviceInfo{};
        CopyTruncatedUtf8(info.id, sizeof(info.id), name);
        CopyTruncatedUtf8(info.name, sizeof(info.name), desc != nullptr ? desc : name);
        for (char* c = info.name; *c != '\0'; ++c) {
          if (*c == '\n') *c = ' ';  // DESC is multi-line ("card\ndevice")
        }
        info.isDefault = std::strcmp(name, "default") == 0;
      }
      ++count;
    }
    std::free(name);
    std::free(desc);
    std::free(ioid);
  }
  snd_device_name_free_hint(hints);
  return count;
}

}  // namespace snd::platform::detail
