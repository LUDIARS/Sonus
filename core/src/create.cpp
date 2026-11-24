// System::Create / Destroy: every byte of the runtime is allocated here
// (design 3.3). After Create the audio-thread path never allocates.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <thread>

#include "core_impl.hpp"
#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"

namespace snd::core {

bool TlsRegister(const Impl& im, std::uint32_t producer) noexcept;
void TlsUnregister(const Impl& im) noexcept;
bool PostJob(void* ctx, const JobDesc& job, JobLane lane) noexcept;
bool PostIo(void* ctx, const source::IoReadRequest& req) noexcept;
ICodec* FindCodec(void* ctx, std::uint32_t fourcc) noexcept;
source::UserBufferControl* UserBufferOf(void* ctx, HandleRef buffer) noexcept;
const std::uint8_t* MusicBankChunk(void* ctx, std::uint16_t bankSlot, std::uint32_t fourcc,
                                   std::uint64_t* bytes) noexcept;
std::uint32_t MusicPrimeFrames(void* ctx, ClipRef clip, std::uint64_t startFrame) noexcept;

namespace {

std::atomic<std::uint64_t> gSerial{0};

// Logged allocation of `count` value-initialised T.
template <class T>
T* Arr(Impl& im, std::size_t count, MemTag tag) noexcept {
  if (count == 0u) count = 1u;
  if (im.allocationCount >= im.allocationCap) return nullptr;
  T* p = base::AllocateArray<T>(*im.alloc, count, tag);
  if (p == nullptr) return nullptr;
  im.allocations[im.allocationCount++] = Impl::Allocation{p, tag, count * sizeof(T)};
  return p;
}

bool IsPow2(std::uint32_t v) noexcept { return v != 0u && (v & (v - 1u)) == 0u; }

snd_result Validate(const snd_config& c) noexcept {
  if (c.sample_rate < 8000u || c.sample_rate > 384000u) return SND_ERR_INVALID_ARGUMENT;
  if (!IsPow2(c.block_size) || c.block_size < 64u || c.block_size > 1024u) return SND_ERR_INVALID_ARGUMENT;
  if (c.output_channels > kMaxChannels) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_real_voices == 0u || c.max_virtual_voices < c.max_real_voices) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_playbacks == 0u || c.max_emitters == 0u) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_listeners == 0u || c.max_listeners > SND_MAX_LISTENERS) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_transports == 0u || c.max_reservations == 0u || c.max_streams == 0u) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_banks == 0u || c.max_banks >= 0xFFFFu || c.max_user_buffers == 0u) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_global_params == 0u || c.event_ring_capacity == 0u) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_registered_threads == 0u || c.max_registered_threads > 64u) return SND_ERR_INVALID_ARGUMENT;
  if (c.max_voice_channels == 0u || c.max_voice_channels > kMaxChannels) return SND_ERR_INVALID_ARGUMENT;
  if (!std::isfinite(c.max_pitch_ratio) || c.max_pitch_ratio < 1.0f || c.max_pitch_ratio > SND_MAX_PITCH_RATIO) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if (c.commands.arenas_per_thread < 2u || c.commands.arena_bytes < 256u) return SND_ERR_INVALID_ARGUMENT;
  if (c.commands.arena_bytes > (1u << 30) || c.commands.reserve_bytes > (1u << 30)) return SND_ERR_INVALID_ARGUMENT;
  if (c.jobs.voices_per_chunk == 0u || c.jobs.voices_per_chunk > 1024u) return SND_ERR_INVALID_ARGUMENT;
  if (c.jobs.realtime_workers > 64u) return SND_ERR_INVALID_ARGUMENT;
  if (c.memory.frame_arena_bytes < 4096u || c.memory.thread_arena_bytes < 4096u) return SND_ERR_INVALID_ARGUMENT;
  if (!std::isfinite(c.tuning.short_fade_ms) || c.tuning.short_fade_ms < 0.0f) return SND_ERR_INVALID_ARGUMENT;
  if (!std::isfinite(c.tuning.steal_hysteresis_db) || c.tuning.steal_hysteresis_db < 0.0f) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if (!std::isfinite(c.doppler_factor) || c.doppler_factor < 0 || c.doppler_factor > 16 ||
      !std::isfinite(c.speed_of_sound) || c.speed_of_sound < 1 || c.speed_of_sound > 100000) return SND_ERR_INVALID_ARGUMENT;
  if (c.listener_mode > SND_LISTENER_SPLIT) return SND_ERR_INVALID_ARGUMENT;
  // Stage 3 (D-132): voice effects.
  if (c.voice_effect_chain > kMaxVoiceEffectChain || c.voice_effect_bytes > (64u << 20) ||
      c.voice_effect_settings == 0u || c.voice_effect_settings > 0xFFFFu) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return SND_OK;
}

void ComputeCfg(const snd_config& c, const CoreLimits& lim, Cfg& g) noexcept {
  g = Cfg{};
  g.sampleRate = c.sample_rate;
  g.blockFrames = c.block_size;
  g.outChannels = c.output_channels == 0u ? 2u : c.output_channels;
  g.maxReal = c.max_real_voices;
  g.fadeReserve = c.fade_reserve_voices;
  g.realTotal = g.maxReal + g.fadeReserve;
  g.maxVirtual = c.max_virtual_voices;
  g.maxPlaybacks = c.max_playbacks;
  g.maxEmitters = c.max_emitters;
  g.maxListeners = c.max_listeners;
  g.maxTransports = c.max_transports;
  g.maxReservations = c.max_reservations;
  g.maxStreams = c.max_streams;
  g.maxBanks = c.max_banks;
  g.maxUserBuffers = c.max_user_buffers;
  g.maxGlobalParams = c.max_global_params;
  g.paramOverflowBlocks = c.param_overflow_blocks;
  g.emitterParamRows = g.maxEmitters * kParamInlineSlots + g.paramOverflowBlocks * kParamOverflowBlockEntries;
  g.playbackParamRows = g.maxPlaybacks * kParamInlineSlots + g.paramOverflowBlocks * kParamOverflowBlockEntries;
  g.voicesPerChunk = c.jobs.voices_per_chunk;
  g.maxChunks = (g.realTotal + g.voicesPerChunk - 1u) / g.voicesPerChunk;
  g.busCap = lim.maxBuses + c.dev_reserve.buses;
  // Dry path plus current sends and any old sends being ramped out this block.
  g.segmentsPerChunk = std::min(g.busCap + 1u, g.voicesPerChunk * (1u + 2u * kMaxAuxSends));
  g.catCap = lim.maxCategories + 1u;
  g.snapCap = lim.maxSnapshots;
  g.groupCap = lim.maxLimitGroups;
  g.paramDefCap = lim.maxParamDefs;
  g.duckCap = lim.maxDuckRules;
  g.arenaBytes = (c.commands.arena_bytes + 7u) & ~7u;
  g.reserveBytes = (c.commands.reserve_bytes + 7u) & ~7u;
  g.arenasPerThread = c.commands.arenas_per_thread;
  g.maxThreads = c.max_registered_threads;
  g.criticalPriority = c.commands.critical_priority;
  g.overflowParamSlots = c.commands.overflow_param_slots;
  g.eventRingCapacity = c.event_ring_capacity;
  g.shortFadeSeconds = c.tuning.short_fade_ms * 0.001f;
  g.shortFadeFrames = static_cast<std::uint32_t>(std::lround(c.tuning.short_fade_ms * 0.001 * c.sample_rate));
  g.maxVoiceChannels = c.max_voice_channels;
  g.maxVoicesPerPlay = lim.maxVoicesPerPlay;
  g.extrapolateFrames = static_cast<std::uint32_t>(std::uint64_t{c.tuning.emitter_extrapolate_ms} * c.sample_rate / 1000u);
  g.loadTargetPercent = c.tuning.load_target_percent;
  g.lodDownBlocks = c.tuning.lod_down_blocks;
  g.lodUpBlocks = c.tuning.lod_up_blocks;
  g.maxPitchRatio = c.max_pitch_ratio;
  g.stealHysteresisDb = c.tuning.steal_hysteresis_db;
  g.listenerMode = c.listener_mode;
  g.seed = c.seed;
  g.resamplerSlotBytes =
      base::AlignUp(dsp::ResamplerStateBytes(dsp::ResamplerQuality::Sinc, c.max_voice_channels, c.max_pitch_ratio),
                    kBufferAlignment);
}

}  // namespace

// Stage 3 (D-132): the voice-effect pool and its tables (create time only).
bool InitVoiceFx(Impl& im) noexcept {
  const Cfg& g = im.cfg;
  const snd_config& c = im.config;
  VoiceFxPriv& x = im.vfx;
  x.chain = c.voice_effect_chain;
  x.slotCount = c.max_voice_effects < g.realTotal ? c.max_voice_effects : g.realTotal;
  x.slotBytes = (c.voice_effect_bytes + 63u) & ~63u;
  x.settings = c.voice_effect_settings;
  if (x.chain == 0u) {
    x.slotCount = 0u;
    return true;  // disabled: nothing configured, nothing allocated
  }
  x.events = Arr<VfxEventEntry>(im, x.settings, MemTag::Tables);
  x.playbacks = Arr<VfxPlaybackEntry>(im, x.settings, MemTag::Tables);
  x.slots = Arr<VfxSlot>(im, x.slotCount, MemTag::Tables);
  x.ofReal = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  x.triedAssign = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  if (x.slotCount != 0u && x.slotBytes != 0u) {
    if (im.allocationCount >= im.allocationCap) return false;
    const std::size_t bytes = std::size_t{x.slotCount} * x.slotBytes;
    x.memory = static_cast<std::uint8_t*>(im.alloc->Allocate(bytes, 64, MemTag::DspPool));
    if (x.memory == nullptr) return false;
    im.allocations[im.allocationCount++] = Impl::Allocation{x.memory, MemTag::DspPool, bytes};
  } else {
    x.slotCount = 0u;
  }
  if (x.events == nullptr || x.playbacks == nullptr || x.slots == nullptr || x.ofReal == nullptr ||
      x.triedAssign == nullptr) {
    return false;
  }
  for (std::uint32_t r = 0; r < g.realTotal; ++r) x.triedAssign[r] = 0xFFFFFFFFu;
  return true;
}

namespace {

// DSP hooks for the bank manager (D-54).
bool DspCompute(void* user, const snd_effect_desc* descs, std::uint32_t count, std::uint32_t sampleRate,
                bf::DspPoolNeed* out) {
  const Impl& im = *static_cast<const Impl*>(user);
  std::uint32_t unknown = 0;
  *out = dsp::ComputeDspPoolNeed(descs, count, sampleRate, im.config.plugins.effects,
                                 im.config.plugins.effect_count, &unknown);
  return true;
}
bool DspReserve(void* user, const bf::DspPoolNeed& need) {
  Impl& im = *static_cast<Impl*>(user);
  im.dspReserved.fetch_add(need.instanceBytes, std::memory_order_relaxed);
  return true;  // instance memory is allocated per bank at registration (MemTag::DspPool)
}
void DspRelease(void* user, const bf::DspPoolNeed& need) {
  Impl& im = *static_cast<Impl*>(user);
  im.dspReserved.fetch_sub(need.instanceBytes, std::memory_order_relaxed);
}

bool InitTables(Impl& im) noexcept {
  using schema::AtomicU64;
  using schema::F32;
  using schema::U32;
  using schema::U64;
  using schema::U8;
  const Cfg& g = im.cfg;
  std::uint32_t caps[static_cast<std::size_t>(schema::TableId::Count)] = {};
  caps[static_cast<int>(schema::TableId::Playback)] = g.maxPlaybacks;
  caps[static_cast<int>(schema::TableId::Voice)] = g.maxVirtual;
  caps[static_cast<int>(schema::TableId::RealVoice)] = g.realTotal;
  caps[static_cast<int>(schema::TableId::Emitter)] = g.maxEmitters;
  caps[static_cast<int>(schema::TableId::Listener)] = g.maxListeners;
  caps[static_cast<int>(schema::TableId::Spatial)] = g.maxEmitters * g.maxListeners;
  caps[static_cast<int>(schema::TableId::ParamGlobal)] = g.maxGlobalParams;
  caps[static_cast<int>(schema::TableId::ParamEmitter)] = g.emitterParamRows;
  caps[static_cast<int>(schema::TableId::ParamPlayback)] = g.playbackParamRows;
  caps[static_cast<int>(schema::TableId::Bus)] = g.busCap;
  caps[static_cast<int>(schema::TableId::Stream)] = g.maxStreams;
  caps[static_cast<int>(schema::TableId::Transport)] = g.maxTransports;
  caps[static_cast<int>(schema::TableId::Reservation)] = g.maxReservations;
  caps[static_cast<int>(schema::TableId::Category)] = g.catCap;
  caps[static_cast<int>(schema::TableId::Snapshot)] = g.snapCap;
  schema::Tables& t = im.tables;
  bool ok = true;
#define SND_BIND_ROWS(T, idx, cr, de, cap)                                                                \
  {                                                                                                       \
    const std::uint32_t n = caps[static_cast<int>(schema::TableId::T)];                                   \
    const std::size_t bytes = base::rows::StorageBytes(n, schema::Indexing::idx);                         \
    std::uint8_t* mem = bytes != 0u ? Arr<std::uint8_t>(im, bytes, MemTag::Tables) : nullptr;             \
    if (mem == nullptr) {                                                                                 \
      ok = false;                                                                                         \
    } else {                                                                                              \
      base::rows::Bind(t.T.rows, n, schema::Indexing::idx, mem);                                          \
    }                                                                                                     \
  }
  SND_SCHEMA_TABLES(SND_BIND_ROWS)
#undef SND_BIND_ROWS
#define SND_ALLOC_COLUMN(T, name, type, temp, writer)                                       \
  if (ok) {                                                                                 \
    t.T.col.name = Arr<type>(im, caps[static_cast<int>(schema::TableId::T)], MemTag::Tables); \
    ok = t.T.col.name != nullptr;                                                           \
  }
  SND_SCHEMA_ALL_COLUMNS(SND_ALLOC_COLUMN)
#undef SND_ALLOC_COLUMN
  return ok;
}

bool InitProducers(Impl& im) noexcept {
  const Cfg& g = im.cfg;
  im.producers = Arr<Producer>(im, g.maxThreads, MemTag::CommandArena);
  if (im.producers == nullptr) return false;
  // Keep the normal reserve budget intact. Only an overflow lifecycle command uses the extra header.
  const std::size_t arenaSize = base::AlignUp(sizeof(cmd::ArenaHeader) + g.arenaBytes + g.reserveBytes +
                                               sizeof(cmd::CommandHeader), alignof(cmd::ArenaHeader));
  std::size_t producerBytes = 0;
  if (!base::CheckedMul(arenaSize, g.arenasPerThread, &producerBytes)) return false;
  const std::uint32_t pbWords = base::notify::BitWords(g.maxPlaybacks);
  const std::uint32_t emWords = base::notify::BitWords(g.maxEmitters);
  const std::uint32_t trWords = base::notify::BitWords(g.maxTransports);
  const std::uint32_t ubWords = base::notify::BitWords(g.maxUserBuffers);
  const std::uint32_t bkWords = base::notify::BitWords(g.maxBanks);
  for (std::uint32_t p = 0; p < g.maxThreads; ++p) {
    Producer& pr = im.producers[p];
    pr.index = p;
    pr.arenas = Arr<ArenaView>(im, g.arenasPerThread, MemTag::CommandArena);
    std::uint8_t* mem = Arr<std::uint8_t>(im, producerBytes, MemTag::CommandArena);
    if (pr.arenas == nullptr || mem == nullptr || !pr.cycle.Init(*im.alloc, g.arenasPerThread)) return false;
    for (std::uint32_t a = 0; a < g.arenasPerThread; ++a) {
      ArenaView& av = pr.arenas[a];
      std::uint8_t* base = mem + arenaSize * a;
      av.header = new (base) cmd::ArenaHeader{};
      av.header->producer = p;
      av.header->arenaIndex = a;
      av.normal = base + 64u;
      av.reserve = av.normal + g.arenaBytes;
      cmd::OverflowFallback& fb = av.fallback;
      fb.stopPlayback = Arr<std::uint64_t>(im, pbWords, MemTag::CommandArena);
      fb.destroyEmitter = Arr<std::uint64_t>(im, emWords, MemTag::CommandArena);
      fb.destroyTransport = Arr<std::uint64_t>(im, trWords, MemTag::CommandArena);
      fb.destroyUserBuffer = Arr<std::uint64_t>(im, ubWords, MemTag::CommandArena);
      fb.unloadBank = Arr<std::uint64_t>(im, bkWords, MemTag::CommandArena);
      fb.params = Arr<cmd::OverflowParam>(im, g.overflowParamSlots, MemTag::CommandArena);
      fb.paramCapacity = g.overflowParamSlots;
      if (fb.stopPlayback == nullptr || fb.destroyEmitter == nullptr || fb.destroyTransport == nullptr ||
          fb.destroyUserBuffer == nullptr || fb.unloadBank == nullptr || fb.params == nullptr) {
        return false;
      }
    }
  }
  return true;
}

bool InitRenderSet(Impl& im, RenderSet& s) noexcept {
  const Cfg& g = im.cfg;
  s.items = Arr<source::RenderItem>(im, g.realTotal, MemTag::Tables);
  s.inputs = Arr<ItemInput>(im, g.realTotal, MemTag::Tables);
  s.chunks = Arr<ChunkInfo>(im, g.maxChunks, MemTag::Tables);
  s.segs = Arr<ChunkSeg>(im, std::size_t{g.maxChunks} * g.segmentsPerChunk, MemTag::Tables);
  s.accum = Arr<float>(im, std::size_t{g.maxChunks} * g.segmentsPerChunk * kMaxChannels * g.blockFrames,
                       MemTag::Tables);
  return s.items != nullptr && s.inputs != nullptr && s.chunks != nullptr && s.segs != nullptr &&
         s.accum != nullptr;
}

snd_result CreateImpl(Impl& im) noexcept {
  const Cfg& g = im.cfg;
  const CoreLimits& lim = im.desc.limits;
  IAllocator& a = *im.alloc;
  if (!InitTables(im) || !InitProducers(im)) return SND_ERR_OUT_OF_BUDGET;
  // Game side.
  if (!im.playbackSlots.Init(a, g.maxPlaybacks) || !im.emitterSlots.Init(a, g.maxEmitters) ||
      !im.transportSlots.Init(a, g.maxTransports) || !im.userBufferSlots.Init(a, g.maxUserBuffers) ||
      !im.rejected.Init(a, g.maxPlaybacks, MemTag::CommandArena)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  im.oneShot = Arr<std::atomic<std::uint8_t>>(im, g.maxPlaybacks, MemTag::Tables);
  std::uint32_t prio = 16;
  while (prio < lim.eventPriorityEntries) prio <<= 1u;
  im.eventPriority = Arr<std::atomic<std::uint64_t>>(im, prio, MemTag::Tables);
  im.eventPriorityMask = prio - 1u;
  im.userBuffers = Arr<source::UserBufferControl>(im, g.maxUserBuffers, MemTag::Tables);
  im.languagePlan = Arr<bk::LanguageSwitch>(im, kMaxLanguageSwitches, MemTag::Misc);
  im.switches = Arr<Impl::PendingSwitch>(im, kMaxLanguageSwitches, MemTag::Misc);
  im.draining = Arr<HandleRef>(im, kMaxLanguageSwitches, MemTag::Misc);
  if (im.draining == nullptr) return SND_ERR_OUT_OF_BUDGET;
  im.pendingRegs = Arr<cmd::BankRegister>(im, g.maxBanks, MemTag::Misc);
  im.records = Arr<BankRecord*>(im, g.maxBanks, MemTag::Misc);
  im.releaseCandidates = Arr<Impl::ReleaseCandidate>(im, g.maxBanks, MemTag::Misc);
  if (im.releaseCandidates == nullptr) return SND_ERR_OUT_OF_BUDGET;
  if (im.oneShot == nullptr || im.eventPriority == nullptr || im.userBuffers == nullptr ||
      im.languagePlan == nullptr || im.switches == nullptr || im.pendingRegs == nullptr || im.records == nullptr) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  // Notifications.
  im.bankStates = Arr<notify::BankSlotState>(im, g.maxBanks, MemTag::Notify);
  im.eventRing = Arr<notify::EventRing>(im, 1, MemTag::Notify);
  im.triple = Arr<notify::SnapshotTripleBuffer>(im, 1, MemTag::Notify);
  if (im.bankStates == nullptr || im.eventRing == nullptr || im.triple == nullptr ||
      !base::notify::InitEndFlags(im.endFlags, a, g.maxPlaybacks) ||
      !base::notify::InitReleaseFlags(im.emitterRelease, a, g.maxEmitters) ||
      !base::notify::InitReleaseFlags(im.transportRelease, a, g.maxTransports) ||
      !base::notify::InitReleaseFlags(im.userBufferRelease, a, g.maxUserBuffers) ||
      !base::notify::InitEventRing(*im.eventRing, a, g.eventRingCapacity) ||
      !base::notify::InitTripleBuffer(*im.triple, a, g.maxPlaybacks, g.maxTransports, g.busCap) ||
      !im.bankEvents.Init(a, g.maxBanks * 2u, MemTag::Notify)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  // Services.
  const std::uint32_t q = lim.serviceQueueEntries < 16u ? 16u : lim.serviceQueueEntries;
  // Every accepted job is tracked from postJob on (bank epoch): 4 queues' worth of
  // jobs may be queued or in the job system at once; beyond that postJob refuses.
  im.jobWraps = Arr<JobWrap>(im, q * 4u, MemTag::Misc);
  if (im.jobWraps == nullptr || !im.jobQueue.Init(a, q, MemTag::Misc) || !im.ioQueue.Init(a, q, MemTag::Misc) ||
      !im.jobWrapFree.Init(a, q * 4u, MemTag::Misc)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  im.servicesReady = true;
  im.jobsInFlight[0].store(0, std::memory_order_relaxed);
  im.jobsInFlight[1].store(0, std::memory_order_relaxed);
  im.services.allocator = im.alloc;
  im.services.tables = &im.tables;
  im.services.postJob = &PostJob;
  im.services.jobCtx = &im;
  im.services.postIo = &PostIo;
  im.services.ioCtx = &im;
  im.services.findCodec = &FindCodec;
  im.services.codecCtx = &im;
  im.services.userBuffer = &UserBufferOf;
  im.services.userBufferCtx = &im;
  // Audio side.
  if (!im.frameArena.Init(a, im.config.memory.frame_arena_bytes, MemTag::FrameArena)) return SND_ERR_OUT_OF_BUDGET;
  im.eventCap = g.eventRingCapacity;
  im.events = Arr<notify::EventRecord>(im, im.eventCap, MemTag::Notify);
  im.ended = Arr<EndedPlayback>(im, g.maxPlaybacks, MemTag::Tables);
  im.releasedEmitters = Arr<Released>(im, g.maxEmitters, MemTag::Tables);
  im.releasedUserBuffers = Arr<Released>(im, g.maxUserBuffers, MemTag::Tables);
  im.fire.entries = Arr<music::FireEntry>(im, g.maxReservations, MemTag::Tables);
  im.fire.capacity = g.maxReservations;
  // Definitions and id maps.
  im.busDefs = Arr<BusDef>(im, g.busCap, MemTag::Tables);
  im.catDefs = Arr<CatDef>(im, g.catCap, MemTag::Tables);
  im.snapDefs = Arr<SnapDef>(im, g.snapCap, MemTag::Tables);
  im.groupDefs = Arr<GroupDef>(im, g.groupCap, MemTag::Tables);
  im.paramDefs = Arr<ParamDef>(im, g.paramDefCap, MemTag::Tables);
  im.duckRules = Arr<DuckRule>(im, g.duckCap, MemTag::Tables);
  struct MapInit {
    IdMap* map;
    std::uint32_t entries;
  };
  const MapInit maps[] = {{&im.busMap, g.busCap},     {&im.catMap, g.catCap},          {&im.snapMap, g.snapCap},
                          {&im.groupMap, g.groupCap}, {&im.paramDefMap, g.paramDefCap}, {&im.ap.globalParams, g.maxGlobalParams}};
  for (const MapInit& m : maps) {
    const std::uint32_t cap = IdMap::CapacityFor(m.entries);
    std::uint32_t* st = Arr<std::uint32_t>(im, std::size_t{cap} * 2u, MemTag::Tables);
    if (st == nullptr) return SND_ERR_OUT_OF_BUDGET;
    m.map->Bind(st, cap);
  }
  if (im.events == nullptr || im.ended == nullptr || im.releasedEmitters == nullptr ||
      im.releasedUserBuffers == nullptr || im.fire.entries == nullptr || im.busDefs == nullptr ||
      im.catDefs == nullptr || im.snapDefs == nullptr || im.groupDefs == nullptr || im.paramDefs == nullptr ||
      im.duckRules == nullptr || !im.busFree.Init(a, g.busCap, MemTag::Tables) ||
      !im.catFree.Init(a, g.catCap, MemTag::Tables) || !im.snapFree.Init(a, g.snapCap, MemTag::Tables) ||
      !im.groupFree.Init(a, g.groupCap, MemTag::Tables) || !im.paramDefFree.Init(a, g.paramDefCap, MemTag::Tables) ||
      !im.duckFree.Init(a, g.duckCap, MemTag::Tables)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  // ApplyCommands.
  ApplyPriv& ap = im.ap;
  ap.emitterChain = Arr<ParamChain>(im, g.maxEmitters, MemTag::Tables);
  ap.playbackChain = Arr<ParamChain>(im, g.maxPlaybacks, MemTag::Tables);
  ap.emitterBlockNext = Arr<std::uint32_t>(im, g.paramOverflowBlocks, MemTag::Tables);
  ap.playbackBlockNext = Arr<std::uint32_t>(im, g.paramOverflowBlocks, MemTag::Tables);
  ap.dynamic = Arr<DynamicDesc>(im, g.maxPlaybacks, MemTag::Tables);
  ap.playbackVolume = Arr<float>(im, g.maxPlaybacks, MemTag::Tables);
  ap.playbackPitch = Arr<float>(im, g.maxPlaybacks, MemTag::Tables);
  if (ap.playbackVolume == nullptr || ap.playbackPitch == nullptr) return SND_ERR_OUT_OF_BUDGET;
  ap.implicitEmitter = Arr<std::uint32_t>(im, g.maxPlaybacks, MemTag::Tables);
  ap.implicitOwnerGen = Arr<std::uint32_t>(im, g.maxEmitters, MemTag::Tables);
  ap.userBufferRefs = Arr<std::uint32_t>(im, g.maxUserBuffers, MemTag::Tables);
  ap.userBufferDestroy = Arr<std::uint32_t>(im, g.maxUserBuffers, MemTag::Tables);
  ap.transportCreated = Arr<std::uint32_t>(im, g.maxTransports, MemTag::Tables);
  ap.userBufferReleased = Arr<std::uint32_t>(im, g.maxUserBuffers, MemTag::Tables);
  if (ap.userBufferReleased == nullptr) return SND_ERR_OUT_OF_BUDGET;
  if (ap.transportCreated == nullptr) return SND_ERR_OUT_OF_BUDGET;
  ap.banks = Arr<BankAudio>(im, g.maxBanks, MemTag::Tables);
  ap.overrides = Arr<LiveOverride>(im, lim.maxLiveOverrides, MemTag::Tables);
  if (ap.emitterChain == nullptr || ap.playbackChain == nullptr || ap.emitterBlockNext == nullptr ||
      ap.playbackBlockNext == nullptr || ap.dynamic == nullptr || ap.implicitEmitter == nullptr ||
      ap.implicitOwnerGen == nullptr || ap.userBufferRefs == nullptr || ap.userBufferDestroy == nullptr ||
      ap.banks == nullptr || ap.overrides == nullptr || !ap.globalFree.Init(a, g.maxGlobalParams, MemTag::Tables)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  if (g.paramOverflowBlocks > 0u && (!ap.emitterBlocks.Init(a, g.paramOverflowBlocks, MemTag::Tables) ||
                                     !ap.playbackBlocks.Init(a, g.paramOverflowBlocks, MemTag::Tables))) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  for (std::uint32_t i = 0; i < g.maxPlaybacks; ++i) {
    ap.implicitEmitter[i] = kNoSlot;
    ap.playbackChain[i] = ParamChain{};
    ap.playbackVolume[i] = 1.0f;
    ap.playbackPitch[i] = 1.0f;
  }
  for (std::uint32_t i = 0; i < g.maxEmitters; ++i) ap.emitterChain[i] = ParamChain{};
  // RunEventLogic.
  EventPriv& ev = im.ev;
  ev.pb = Arr<PlaybackPriv>(im, g.maxPlaybacks, MemTag::Tables);
  ev.vs = Arr<VoiceStatic>(im, g.maxVirtual, MemTag::Tables);
  ev.groupLive = Arr<std::uint32_t>(im, g.groupCap, MemTag::Tables);
  ev.releaseSlots = Arr<SlotIndex>(im, g.maxVirtual, MemTag::Tables);
  ev.controls = Arr<source::SourceControlOp>(im, g.maxVirtual, MemTag::Tables);
  ev.controlKind = Arr<std::uint32_t>(im, g.maxVirtual, MemTag::Tables);
  ev.musicInputCap = g.maxPlaybacks + g.maxReservations;
  ev.musicInputs = Arr<music::MusicInput>(im, ev.musicInputCap, MemTag::Tables);
  ev.musicRequestCap = 256;
  ev.musicRequests = Arr<music::MusicVoiceRequest>(im, ev.musicRequestCap, MemTag::Tables);
  ev.musicResults = Arr<music::MusicVoiceResult>(im, ev.musicRequestCap, MemTag::Tables);
  ev.deferredCap = 256;
  ev.deferred = Arr<Deferred>(im, ev.deferredCap, MemTag::Tables);
  ev.releaseKind = Arr<std::uint32_t>(im, g.maxVirtual, MemTag::Tables);
  if (ev.deferred == nullptr || ev.releaseKind == nullptr) return SND_ERR_OUT_OF_BUDGET;
  if (ev.pb == nullptr || ev.vs == nullptr || ev.groupLive == nullptr || ev.releaseSlots == nullptr ||
      ev.controls == nullptr || ev.controlKind == nullptr || ev.musicInputs == nullptr ||
      ev.musicRequests == nullptr || ev.musicResults == nullptr || !ev.voiceFree.Init(a, g.maxVirtual, MemTag::Tables)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count); ++k) {
    ev.start[k].req = Arr<source::SourceStartRequest>(im, g.maxVirtual, MemTag::Tables);
    ev.start[k].res = Arr<source::SourceStartResult>(im, g.maxVirtual, MemTag::Tables);
    if (ev.start[k].req == nullptr || ev.start[k].res == nullptr) return SND_ERR_OUT_OF_BUDGET;
  }
  // EvaluateDspParams, SelectVoices.
  im.dp.duckDb = Arr<float>(im, g.duckCap, MemTag::Tables);
  im.dp.catPlaying = Arr<std::uint32_t>(im, g.catCap, MemTag::Tables);
  im.dp.catGainPrev = Arr<float>(im, g.catCap, MemTag::Tables);
  im.dp.mix = Arr<VoiceSpatialMix>(im, g.maxVirtual, MemTag::Tables);
  if (!im.dp.mix) return SND_ERR_OUT_OF_BUDGET;
  im.dp.pan = Arr<float>(im, g.maxVirtual, MemTag::Tables);
  if (im.dp.pan == nullptr) return SND_ERR_OUT_OF_BUDGET;
  im.sel.voiceGen = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  im.sel.fadeRemaining = Arr<std::int32_t>(im, g.realTotal, MemTag::Tables);
  if (im.dp.duckDb == nullptr || im.dp.catPlaying == nullptr || im.dp.catGainPrev == nullptr ||
      im.sel.voiceGen == nullptr || im.sel.fadeRemaining == nullptr ||
      !im.sel.realFree.Init(a, g.realTotal, MemTag::Tables)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  // Render.
  RenderPriv& rd = im.rd;
  rd.paths = Arr<PathHistory>(im, g.realTotal, MemTag::Tables);
  rd.sendInput = Arr<float>(im, std::size_t{g.realTotal} * g.maxVoiceChannels * g.blockFrames, MemTag::Tables);
  if (!rd.paths || !rd.sendInput) return SND_ERR_OUT_OF_BUDGET;
  rd.resamplerPool = Arr<std::uint8_t>(im, im.cfg.resamplerSlotBytes * g.realTotal, MemTag::Tables);
  rd.lastAssign = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.stateChannels = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.fade = Arr<float>(im, g.realTotal, MemTag::Tables);
  rd.tail = Arr<float>(im, std::size_t{g.realTotal} * kMaxChannels, MemTag::Tables);
  rd.tailBus = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.tailChannels = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.tailFadeLeft = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.tailFade = Arr<float>(im, std::size_t{g.realTotal} * kMaxChannels, MemTag::Tables);
  rd.tailFadeBus = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.tailFadeChannels = Arr<std::uint32_t>(im, g.realTotal, MemTag::Tables);
  rd.busBuf = Arr<float>(im, std::size_t{g.busCap} * kMaxChannels * g.blockFrames, MemTag::Tables);
  rd.busOrder = Arr<std::uint32_t>(im, g.busCap, MemTag::Tables);
  rd.peak = Arr<dsp::PeakMeterState>(im, g.busCap, MemTag::Tables);
  rd.rms = Arr<dsp::RmsMeterState>(im, g.busCap, MemTag::Tables);
  rd.loudness = Arr<dsp::LoudnessMeterState>(im, 1, MemTag::Tables);
  rd.outBuf = Arr<float>(im, std::size_t{kMaxChannels} * g.blockFrames, MemTag::Tables);
  rd.busGen = Arr<std::uint32_t>(im, g.busCap, MemTag::Tables);
  im.lp.globalGen = Arr<std::uint32_t>(im, g.maxGlobalParams, MemTag::Tables);
  im.lp.emitterGen = Arr<std::uint32_t>(im, g.emitterParamRows, MemTag::Tables);
  im.lp.playbackGen = Arr<std::uint32_t>(im, g.playbackParamRows, MemTag::Tables);
  im.tp.seenSeq = Arr<std::uint64_t>(im, g.maxReservations, MemTag::Tables);
  im.tp.transportGen = Arr<std::uint32_t>(im, g.maxTransports, MemTag::Tables);
  if (im.tp.transportGen == nullptr || rd.busGen == nullptr || im.lp.globalGen == nullptr || im.lp.emitterGen == nullptr ||
      im.lp.playbackGen == nullptr || im.tp.seenSeq == nullptr) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  im.pub.streamStall = Arr<std::uint32_t>(im, g.maxStreams, MemTag::Tables);
  im.pub.streamGen = Arr<std::uint32_t>(im, g.maxStreams, MemTag::Tables);
  if (im.pub.streamStall == nullptr || im.pub.streamGen == nullptr) return SND_ERR_OUT_OF_BUDGET;
  im.pub.transportGen = Arr<std::uint32_t>(im, g.maxTransports, MemTag::Tables);
  im.pub.playbackGen = Arr<std::uint32_t>(im, std::size_t{g.maxPlaybacks} * 3u, MemTag::Tables);
  rd.voiceGen = Arr<std::uint32_t>(im, g.maxVirtual, MemTag::Tables);
  rd.holdBlock = Arr<std::uint64_t>(im, g.maxVirtual, MemTag::Tables);
  if (rd.holdBlock == nullptr) return SND_ERR_OUT_OF_BUDGET;
  if (rd.voiceGen == nullptr) return SND_ERR_OUT_OF_BUDGET;
  im.fifo = Arr<float>(im, std::size_t{g.blockFrames} * g.outChannels, MemTag::Tables);
  if (rd.resamplerPool == nullptr || rd.lastAssign == nullptr || rd.stateChannels == nullptr || rd.fade == nullptr ||
      rd.tail == nullptr || rd.tailBus == nullptr || rd.tailChannels == nullptr || rd.tailFadeLeft == nullptr || rd.tailFade == nullptr || rd.tailFadeBus == nullptr ||
      rd.tailFadeChannels == nullptr || rd.busBuf == nullptr ||
      rd.busOrder == nullptr || rd.peak == nullptr || rd.rms == nullptr || rd.loudness == nullptr ||
      rd.outBuf == nullptr || im.pub.transportGen == nullptr || im.pub.playbackGen == nullptr ||
      im.fifo == nullptr || !InitRenderSet(im, rd.sets[0]) || !InitRenderSet(im, rd.sets[1])) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  for (std::uint32_t r = 0; r < g.realTotal; ++r) rd.lastAssign[r] = 0xFFFFFFFFu;
  if (!InitVoiceFx(im)) return SND_ERR_OUT_OF_BUDGET;
  dsp::LoudnessMeterInit(*rd.loudness, g.outChannels, g.sampleRate);
  // RT workers (2.3).
  jobs::RtWorkerConfig rc;
  rc.workers = im.config.jobs.realtime_workers;
  rc.maxChunks = g.maxChunks;
  rc.spinMicroseconds = im.config.tuning.rt_spin_microseconds;
  rc.threadArenaBytes = im.config.memory.thread_arena_bytes;
  rc.realtimePriority = im.desc.rtWorkerRealtimePriority;
  rc.debugClaimDelayMicroseconds = im.desc.rtDebugClaimDelayMicroseconds;
  if (!im.rt.Init(a, rc)) return SND_ERR_OUT_OF_BUDGET;
  // Global category row "all categories" (D-30).
  {
    schema::Tables& t = im.tables;
    const std::uint32_t slot = im.catFree.Pop();
    const std::uint32_t d = base::rows::InsertNext(t.Category.rows, slot);
    t.Category.col.categoryId[d] = 0u;
    t.Category.col.request[d] = CategoryRequest{TimedRamp{1.0f, 1.0f, 0, 0, 0}, 0u, 0u};
    t.Category.col.flags[d] = 0u;
    t.Category.col.gain[d] = 1.0f;
    im.catDefs[slot] = CatDef{0u, 1u, kNoSlot, 0u, 0.0f};
    im.globalCategory = slot;
  }
  // Sources and music.
  if (im.desc.sources != nullptr) {
    source::SourceLimits sl{};
    sl.maxVoices = g.maxVirtual;
    sl.maxRealVoices = g.realTotal;
    sl.maxStreams = g.maxStreams;
    sl.streamChunkBytes = im.config.memory.stream_chunk_bytes;
    sl.streamPoolBytes = im.config.memory.stream_pool_bytes;
    sl.maxDecoders = im.config.memory.max_decoders;
    sl.decoderSlotBytes = im.config.memory.decoder_slot_bytes;
    sl.voiceRingFrames = im.config.memory.voice_ring_frames;
    sl.callbackAheadBlocks = im.config.tuning.callback_ahead_blocks;
    sl.pcmCacheBytes = im.config.memory.pcm_cache_bytes;
    sl.pcmCachePageBytes = im.config.memory.pcm_cache_page_bytes;
    sl.blockFrames = g.blockFrames;
    sl.sampleRate = g.sampleRate;
    sl.streamTargetAheadMs = im.config.tuning.stream_target_ahead_ms;
    for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count); ++k) {
      source::ISourceKind* kind = im.desc.sources->kinds[k];
      if (kind == nullptr) continue;
      const std::size_t bytes = kind->PoolBytes(sl);
      void* pool = bytes != 0u ? Arr<std::uint8_t>(im, bytes, MemTag::Misc) : nullptr;
      if (bytes != 0u && pool == nullptr) return SND_ERR_OUT_OF_BUDGET;
      if (!kind->Init(pool, bytes, sl, im.services)) return SND_ERR_OUT_OF_BUDGET;
      im.kinds[k] = kind;
    }
  }
  if (im.desc.music != nullptr) {
    music::MusicLimits ml{};
    ml.maxTransports = g.maxTransports;
    ml.maxReservations = g.maxReservations;
    ml.maxPlaybacks = g.maxPlaybacks;
    ml.maxVoiceRequestsPerBlock = im.ev.musicRequestCap;
    ml.sampleRate = g.sampleRate;
    ml.blockFrames = g.blockFrames;
    music::MusicServices ms{};
    ms.allocator = im.alloc;
    ms.bankChunk = &MusicBankChunk;
    ms.bankCtx = &im;
    ms.primeFrames = &MusicPrimeFrames;
    ms.primeCtx = &im;
    const std::size_t bytes = im.desc.music->PoolBytes(ml);
    void* pool = bytes != 0u ? Arr<std::uint8_t>(im, bytes, MemTag::Misc) : nullptr;
    if (bytes != 0u && pool == nullptr) return SND_ERR_OUT_OF_BUDGET;
    if (!im.desc.music->Init(pool, bytes, ml, ms)) return SND_ERR_OUT_OF_BUDGET;
    im.music = im.desc.music;
  }
  // Bank manager (10.3).
  if (im.desc.jobs != nullptr && (im.desc.bankScheduler != nullptr || im.desc.bankFileSystem != nullptr)) {
    bk::BankManagerDesc bd;
    bd.allocator = im.alloc;
    bd.ioMode = im.desc.bankScheduler != nullptr ? bk::BankIoMode::Scheduler : bk::BankIoMode::DirectFileSystem;
    bd.scheduler = im.desc.bankScheduler;
    bd.fileSystem = im.desc.bankFileSystem;
    bd.jobs = im.desc.jobs;
    bd.maxBanks = g.maxBanks;
    bd.prepare.devOverlay = im.config.livelink.enabled != 0u;
    bd.prepare.devReserve.buses = im.config.dev_reserve.buses;
    bd.prepare.devReserve.effects = im.config.dev_reserve.effects;
    bd.prepare.devReserve.events = im.config.dev_reserve.events;
    bd.prepare.devReserve.bytes = im.config.dev_reserve.bytes;
    bd.slotStates = im.bankStates;
    bd.dsp.user = &im;
    bd.dsp.reserve = &DspReserve;
    bd.dsp.release = &DspRelease;
    bd.dsp.compute = &DspCompute;
    bd.sampleRate = g.sampleRate;
    bd.blockSize = g.blockFrames;
    bd.outputChannels = g.outChannels;
    const snd_result r = bk::BankManager::Create(bd, &im.banks);
    if (r != SND_OK) return r;
  } else {
    for (std::uint32_t i = 0; i < g.maxBanks; ++i) {
      im.bankStates[i].state.store(SND_BANK_STATE_UNLOADED, std::memory_order_relaxed);
      im.bankStates[i].generation.store(1, std::memory_order_relaxed);
    }
  }
  // Owner thread = producer 0.
  im.producers[0].active.store(1u, std::memory_order_release);
  if (!TlsRegister(im, 0u)) return SND_ERR_LIMIT_REACHED;
  im.stats.block_budget_us = static_cast<float>(1e6 * g.blockFrames / g.sampleRate * g.loadTargetPercent / 100.0);
  im.outputLatency.store(im.desc.outputLatencySeconds, std::memory_order_relaxed);
  im.stats.output_latency_s = static_cast<float>(im.desc.outputLatencySeconds +
                                                 double(im.config.render_ahead_blocks) * g.blockFrames / g.sampleRate);
  return SND_OK;
}

void DestroyImpl(Impl& im) noexcept {
  im.rt.Shutdown();
  // Source kinds first (stage 2, source): their Shutdown waits for the jobs they
  // posted, which may read bank memory, so the banks must still be alive. Jobs
  // still in the service queue are submitted first (PumpServices, done by
  // System::Destroy), otherwise their done counters would never drop.
  for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count); ++k) {
    if (im.kinds[k] != nullptr) im.kinds[k]->Shutdown();
    im.kinds[k] = nullptr;
  }
  // Every job trampoline has returned before the records and the free list go away.
  while (im.trampolinesInFlight.load(std::memory_order_acquire) != 0u) std::this_thread::yield();
  if (im.banks != nullptr) {
    for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
      if (im.records != nullptr && im.records[s] != nullptr) FreeBankRecord(im, s);
    }
    bk::BankManager::Destroy(im.banks);
    im.banks = nullptr;
  } else if (im.records != nullptr) {
    for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
      if (im.records[s] != nullptr) FreeBankRecord(im, s);
    }
  }
  if (im.music != nullptr) im.music->Shutdown();
  if (im.triple != nullptr) base::notify::DestroyTripleBuffer(*im.triple, *im.alloc);
  if (im.eventRing != nullptr) base::notify::DestroyEventRing(*im.eventRing, *im.alloc);
  base::notify::DestroyEndFlags(im.endFlags, *im.alloc);
  base::notify::DestroyReleaseFlags(im.emitterRelease, *im.alloc);
  base::notify::DestroyReleaseFlags(im.transportRelease, *im.alloc);
  base::notify::DestroyReleaseFlags(im.userBufferRelease, *im.alloc);
  if (im.producers != nullptr) {
    for (std::uint32_t p = 0; p < im.cfg.maxThreads; ++p) im.producers[p].cycle.Destroy();
  }
  TlsUnregister(im);
}

}  // namespace

snd_result System::Create(const CoreDesc& desc, System** out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  if (desc.config == nullptr || desc.allocator == nullptr) return SND_ERR_INVALID_ARGUMENT;
  const snd_result v = Validate(*desc.config);
  if (v != SND_OK) return v;
  IAllocator& a = *desc.allocator;
  void* sm = a.Allocate(sizeof(System), alignof(System) < 16 ? 16 : alignof(System), MemTag::Misc);
  void* im = a.Allocate(sizeof(Impl), 64, MemTag::Misc);
  Impl::Allocation* log = static_cast<Impl::Allocation*>(a.Allocate(sizeof(Impl::Allocation) * 4096u, 16, MemTag::Misc));
  if (sm == nullptr || im == nullptr || log == nullptr) {
    if (sm != nullptr) a.Free(sm, MemTag::Misc);
    if (im != nullptr) a.Free(im, MemTag::Misc);
    if (log != nullptr) a.Free(log, MemTag::Misc);
    return SND_ERR_OUT_OF_BUDGET;
  }
  System* sys = new (sm) System();
  Impl* impl = new (im) Impl();
  sys->impl_ = impl;
  impl->desc = desc;
  impl->config = *desc.config;
  impl->desc.config = &impl->config;
  impl->alloc = &a;
  impl->allocations = log;
  impl->allocationCap = 4096u;
  impl->serial = gSerial.fetch_add(1u, std::memory_order_relaxed) + 1u;
  ComputeCfg(impl->config, desc.limits, impl->cfg);
  const snd_result r = CreateImpl(*impl);
  if (r != SND_OK) {
    Destroy(sys);
    return r;
  }
  *out = sys;
  return SND_OK;
}

void System::Destroy(System* sys) noexcept {
  if (sys == nullptr) return;
  Impl* im = sys->impl_;
  IAllocator& a = *im->alloc;
  sys->PumpServices();  // queued source jobs reach the job system (run inline if it is shut down)
  DestroyImpl(*im);
  for (std::uint32_t i = im->allocationCount; i-- > 0;) a.Free(im->allocations[i].p, im->allocations[i].tag);
  a.Free(im->allocations, MemTag::Misc);
  im->~Impl();
  a.Free(im, MemTag::Misc);
  sys->~System();
  a.Free(sys, MemTag::Misc);
}

std::uint32_t System::OutputChannels() const noexcept { return impl_->cfg.outChannels; }
void System::SetOutputLatency(double seconds) noexcept {
  if (seconds >= 0.0 && seconds < 60.0) impl_->outputLatency.store(seconds, std::memory_order_relaxed);
}
std::uint32_t System::BlockFrames() const noexcept { return impl_->cfg.blockFrames; }
std::uint32_t System::SampleRate() const noexcept { return impl_->cfg.sampleRate; }
const BlockProfile& System::LastBlock() const noexcept { return impl_->profile; }

}  // namespace snd::core
