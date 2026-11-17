// Composition root of the `sonus` library (api/compose/compose.cpp, CONTRACTS.md D-03).
#ifndef SND_API_COMPOSE_HPP
#define SND_API_COMPOSE_HPP

#include "snd/ext/allocator.hpp"
#include "snd/internal/source_if.hpp"

namespace snd::io {
class IoScheduler;
class StreamPool;
}  // namespace snd::io

namespace snd::compose {

// The source module's kinds (Sample, Stream) for core (CoreDesc::sources),
// stage 2. snd_create() makes one per system: Init before core's Create,
// Destroy after core's Destroy. Without an IO scheduler (and its stream pool)
// the Stream kind is absent (stream voices end UNSUPPORTED).
class SourceKinds {
 public:
  SourceKinds() noexcept = default;
  ~SourceKinds() { Destroy(); }
  SourceKinds(const SourceKinds&) = delete;
  SourceKinds& operator=(const SourceKinds&) = delete;

  bool Init(IAllocator& allocator, io::IoScheduler* io, io::StreamPool* streamPool) noexcept;
  void Destroy() noexcept;
  // nullptr before Init.
  const source::SourceKindSet* Kinds() const noexcept;

 private:
  void* runtime_ = nullptr;  // snd::source::Runtime
  IAllocator* alloc_ = nullptr;
};

// Names of the runtime modules linked into `sonus`, in dependency order.
const char* const* LinkedModules(unsigned* count) noexcept;

}  // namespace snd::compose

#endif  // SND_API_COMPOSE_HPP
