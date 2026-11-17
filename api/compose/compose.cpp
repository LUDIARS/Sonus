// Composition root of the full runtime library `sonus` (CONTRACTS.md D-03).
//
// snd_create() (api) needs objects from modules that api may not depend on
// at compile time (12.1): the source kinds (snd::source::SourceKindSet), the
// music system (snd::music::IMusicSystem), the default file system (io), the
// platform output device and the live link. This translation unit is the only
// place that sees all of them; it hands them to api/core through the
// contract interfaces. Stage 0 only references every module so the umbrella
// library links completely.
#include <new>

#include "snd/api/api_module.hpp"
#include "snd/api/compose.hpp"
#include "snd/bank/bank_module.hpp"
#include "snd/base/base_module.hpp"
#include "snd/codec/codec_module.hpp"
#include "snd/core/core_module.hpp"
#include "snd/dsp/dsp_module.hpp"
#include "snd/io/io_module.hpp"
#include "snd/jobs/jobs_module.hpp"
#include "snd/livelink/livelink_module.hpp"
#include "snd/music/music_module.hpp"
#include "snd/platform/platform_module.hpp"
#include "snd/source/runtime.hpp"
#include "snd/source/source_module.hpp"

namespace snd::compose {

// Names of the modules linked into this build, in dependency order.
const char* const* LinkedModules(unsigned* count) noexcept {
  static const char* const kModules[] = {
      base::ModuleName(),  platform::ModuleName(), jobs::ModuleName(),   io::ModuleName(),
      codec::ModuleName(), dsp::ModuleName(),      bank::ModuleName(),   core::ModuleName(),
      source::ModuleName(), music::ModuleName(),   api::ModuleName(),    livelink::ModuleName(),
  };
  if (count != nullptr) *count = static_cast<unsigned>(sizeof(kModules) / sizeof(kModules[0]));
  return kModules;
}

bool SourceKinds::Init(IAllocator& allocator, io::IoScheduler* io, io::StreamPool* streamPool,
                       std::uint32_t maxVoiceChannels) noexcept {
  Destroy();
  void* mem = allocator.Allocate(sizeof(source::Runtime), 64, MemTag::Misc);
  if (mem == nullptr) return false;
  auto* rt = new (mem) source::Runtime();
  source::RuntimeDesc d;
  d.allocator = &allocator;
  d.io = io;
  d.streamPool = streamPool;
  d.maxVoiceChannels = maxVoiceChannels;
  if (!rt->Init(d)) {
    rt->~Runtime();
    allocator.Free(mem, MemTag::Misc);
    return false;
  }
  runtime_ = rt;
  alloc_ = &allocator;
  return true;
}

void SourceKinds::Destroy() noexcept {
  if (runtime_ == nullptr) return;
  auto* rt = static_cast<source::Runtime*>(runtime_);
  rt->~Runtime();
  alloc_->Free(rt, MemTag::Misc);
  runtime_ = nullptr;
  alloc_ = nullptr;
}

void SourceKinds::BankUnregistered(void* user, std::uint16_t bankSlot) noexcept {
  source::Runtime::BankUnregistered(user, bankSlot);
}

const source::SourceKindSet* SourceKinds::Kinds() const noexcept {
  return runtime_ != nullptr ? &static_cast<const source::Runtime*>(runtime_)->Kinds() : nullptr;
}

}  // namespace snd::compose
