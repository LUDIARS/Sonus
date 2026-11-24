// Banks in core (10.3): the per-bank record prepared on the owner thread at
// PopRegistration (event index, source assets, synth parameters, effect
// instances in DSP pool memory), and the audio-thread registration that
// unifies shared objects by id (A1-16: buses, categories, snapshots, voice
// limit groups, parameter definitions, ducking rules are copied into every
// bank that uses them; core keeps ONE row per id with a reference count).
//
// Definitions: the first registered bank that has an id defines it; when that
// bank goes away while others still use the id, the definition moves to the
// next registered bank that has it (its effect instances start from their
// initial state).
#include <algorithm>
#include <cstring>
#include <new>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {

constexpr schema::SystemId kSys = schema::SystemId::ApplyCommands;

std::size_t Up64(std::size_t n) noexcept { return (n + 63u) & ~static_cast<std::size_t>(63u); }

}  // namespace

// ---------------------------------------------------------------------------
// Owner thread: record preparation
// ---------------------------------------------------------------------------
bool PrepareBankRecord(Impl& im, const cmd::BankRegister& reg) noexcept {
  const bk::PreparedBank* pb = bk::AsPrepared(reg.prepared);
  if (pb == nullptr || reg.bank.slot >= im.cfg.maxBanks) return false;
  if (im.records[reg.bank.slot] != nullptr) FreeBankRecord(im, reg.bank.slot);
  const bk::BankView& v = pb->view;
  const std::uint32_t nE = v.Rows(bf::chunk::EVNT), nA = v.Rows(bf::chunk::ASST), nS = v.Rows(bf::chunk::SYNP);
  const std::uint32_t nB = v.Rows(bf::chunk::BUSS), nC = v.Rows(bf::chunk::CATG), nSn = v.Rows(bf::chunk::SNAP);
  const std::uint32_t nG = v.Rows(bf::chunk::VLGR), nD = v.Rows(bf::chunk::DUCK), nX = v.Rows(bf::chunk::EFFX);
  std::size_t off = Up64(sizeof(BankRecord));
  const std::size_t oIds = off;
  off = Up64(off + sizeof(std::uint32_t) * nE);
  const std::size_t oRows = off;
  off = Up64(off + sizeof(std::uint32_t) * nE);
  const std::size_t oAssets = off;
  off = Up64(off + sizeof(source::SourceAsset) * nA);
  const std::size_t oOsc = off;
  off = Up64(off + sizeof(source::OscillatorParams) * nS);
  const std::size_t oNoise = off;
  off = Up64(off + sizeof(source::NoiseParams) * nS);
  const std::size_t oMaps = off;
  off = Up64(off + sizeof(std::uint32_t) * (2u * nB + nC + nSn + nG + nD));
  const std::size_t oSend = off;
  off = Up64(off + sizeof(float) * v.Rows(bf::chunk::SEND));
  const std::size_t oEvents = off;
  off = Up64(off + sizeof(EventState) * nE);
  const std::size_t oFx = off;
  off = Up64(off + sizeof(EffectInstance) * nX);
  const std::size_t oParams = off;
  off = Up64(off + sizeof(float) * dsp::kMaxEffectParams * nX);
  // D-130: per effect, the controls and this block's ParamBlock values.
  const std::size_t oCtl = off;
  off = Up64(off + sizeof(EffectParamControl) * dsp::kMaxEffectParams * nX);
  const std::size_t oBlock = off;
  off = Up64(off + sizeof(float) * 2u * dsp::kMaxEffectParams * nX);
  // D-131: previous-block buffers of the sidechain (DUCK kind 0) rows.
  std::uint32_t nSc = 0;
  for (std::uint32_t k = 0; k < nD; ++k) nSc += v.Column<bk::col::DUCK_kind>()[k] == 0u ? 1u : 0u;
  const std::size_t oScIndex = off;
  off = Up64(off + sizeof(std::uint32_t) * 2u * nD);
  const std::size_t oScPrev = off;
  off = Up64(off + sizeof(float) * kMaxChannels * im.cfg.blockFrames * nSc);
  auto* mem = static_cast<std::uint8_t*>(im.alloc->Allocate(off, 64, MemTag::Tables));
  if (mem == nullptr) return false;
  std::memset(mem, 0, off);
  BankRecord& r = *new (mem) BankRecord();
  r.memory = mem;
  r.handle = reg.bank;
  r.prepared = pb;
  r.view = &pb->view;
  r.bankId = pb->bankId;
  r.languageId = pb->languageId;
  // Event index sorted by id.
  r.eventCount = nE;
  r.eventIds = reinterpret_cast<std::uint32_t*>(mem + oIds);
  r.eventRows = reinterpret_cast<std::uint32_t*>(mem + oRows);
  {
    const auto ids = v.Column<bk::col::EVNT_id>();
    for (std::uint32_t i = 0; i < nE; ++i) r.eventRows[i] = i;
    std::sort(r.eventRows, r.eventRows + nE, [&](std::uint32_t a, std::uint32_t b) {
      return ids[a] != ids[b] ? ids[a] < ids[b] : a < b;
    });
    for (std::uint32_t i = 0; i < nE; ++i) r.eventIds[i] = ids[r.eventRows[i]];
  }
  // Source assets (stable while the bank is registered).
  r.assetCount = nA;
  r.assets = reinterpret_cast<source::SourceAsset*>(mem + oAssets);
  for (std::uint32_t a = 0; a < nA; ++a) {
    source::SourceAsset& s = r.assets[a];
    s.codec.codec = v.Column<bk::col::ASST_codec>()[a];
    s.codec.channels = v.Column<bk::col::ASST_channels>()[a];
    s.codec.sample_rate = v.Column<bk::col::ASST_sampleRate>()[a];
    s.codec.frames_per_packet = v.Column<bk::col::ASST_framesPerPacket>()[a];
    s.codec.total_frames = v.Column<bk::col::ASST_totalFrames>()[a];
    s.codec.pre_skip = v.Column<bk::col::ASST_preSkip>()[a];
    s.codec.pre_roll = v.Column<bk::col::ASST_preRoll>()[a];
    s.codec.block_align = v.Column<bk::col::ASST_blockAlign>()[a];
    const bk::ByteSpan extra = v.AssetExtra(a);
    s.codec.extra = extra.data;
    s.codec.extra_bytes = static_cast<std::uint32_t>(extra.bytes);
    const std::uint32_t form = v.Column<bk::col::ASST_loadForm>()[a];
    s.form = form <= 3u ? static_cast<source::LoadForm>(form) : source::LoadForm::PcmResident;
    if (s.form != source::LoadForm::Stream) {
      const bk::ByteSpan d = v.WaveData(a);
      s.data = d.data;
      s.dataBytes = d.bytes;
    }
    const bk::ByteSpan pre = v.Preload(a);
    s.preload = pre.data;
    s.preloadBytes = static_cast<std::uint32_t>(pre.bytes);
    s.preloadFrames = v.Column<bk::col::ASST_preloadFrames>()[a];
    const RangeU32 seek = v.SeekRange(a);
    if (seek.count != 0u) {
      s.seekFrames = v.Column<bk::col::SEEK_frame>().data() + seek.first;
      s.seekOffsets = v.Column<bk::col::SEEK_byteOffset>().data() + seek.first;
      s.seekCount = seek.count;
    }
    const std::uint32_t strm = v.Column<bk::col::ASST_stream>()[a];
    s.streamFile = strm;
    if (strm != bf::kNone) {
      s.streamPath = v.StreamPath(strm);
      s.streamDataOffset = v.Column<bk::col::STRM_dataOffset>()[strm];
    }
    s.loopStart = v.Column<bk::col::ASST_loopStart>()[a];
    s.loopEnd = v.Column<bk::col::ASST_loopEnd>()[a];
    s.assetId = v.Column<bk::col::ASST_id>()[a];
    const std::uint32_t fl = v.Column<bk::col::ASST_flags>()[a];
    s.stallPolicy = (fl & bf::kAssetStallSoundPriority) != 0u ? 1u : 0u;
    s.virtualReturn = (fl & bf::kAssetVirtualKill) != 0u ? 2u : ((fl & bf::kAssetVirtualRestart) != 0u ? 1u : 0u);
    s.ioPriority = (fl & bf::kAssetPriorityMusic) != 0u ? 2u : ((fl & bf::kAssetPriorityVoice) != 0u ? 1u : 0u);
    s.bankSlot = static_cast<std::uint16_t>(reg.bank.slot);
    // Load generation (low 16 bits of the bank handle's generation): sources key
    // their caches with it so another load in the same slot never matches (source_if.hpp).
    s.reserved = static_cast<std::uint16_t>(reg.bank.generation);
  }
  // Synth parameters (SYNP).
  r.synthCount = nS;
  r.osc = reinterpret_cast<source::OscillatorParams*>(mem + oOsc);
  r.noise = reinterpret_cast<source::NoiseParams*>(mem + oNoise);
  for (std::uint32_t s = 0; s < nS; ++s) {
    r.osc[s].waveform = v.Column<bk::col::SYNP_waveform>()[s];
    r.osc[s].frequencyHz = v.Column<bk::col::SYNP_frequencyHz>()[s];
    r.osc[s].duty = v.Column<bk::col::SYNP_duty>()[s];
    r.osc[s].tablePosition = 0.0f;
    const std::uint32_t asset = v.Column<bk::col::SYNP_asset>()[s];
    r.osc[s].wavetable = asset < nA ? &r.assets[asset] : nullptr;
    r.noise[s].color = v.Column<bk::col::SYNP_noiseColor>()[s];
    r.noise[s].seed = s * 2654435761u + 1u;
  }
  // Mapping arrays (filled by ApplyCommands).
  std::uint32_t* maps = reinterpret_cast<std::uint32_t*>(mem + oMaps);
  r.sendContrib = reinterpret_cast<float*>(mem + oSend);
  r.busCount = nB;
  r.busSlot = maps;
  r.busChannels = maps + nB;
  r.catCount = nC;
  r.catSlot = maps + 2u * nB;
  r.snapCount = nSn;
  r.snapSlot = r.catSlot + nC;
  r.groupCount = nG;
  r.groupSlot = r.snapSlot + nSn;
  r.duckCount = nD;
  r.duckSlot = r.groupSlot + nG;
  // Channels per bus (D-54): own, else the parent's (parents come later), else the output's.
  for (std::uint32_t b = nB; b-- > 0;) {
    const std::uint32_t own = v.Column<bk::col::BUSS_channels>()[b];
    const std::uint32_t parent = v.Column<bk::col::BUSS_parent>()[b];
    std::uint32_t ch = own;
    if (ch == 0u) ch = parent < nB ? r.busChannels[parent] : im.cfg.outChannels;
    r.busChannels[b] = ch == 0u || ch > kMaxChannels ? im.cfg.outChannels : ch;
  }
  r.events = reinterpret_cast<EventState*>(mem + oEvents);
  // D-131: sidechain rows (memory is zero: the first "previous block" is silence).
  r.sidechainIndex = reinterpret_cast<std::uint32_t*>(mem + oScIndex);
  r.sidechainChannels = r.sidechainIndex + nD;
  r.sidechainPrev = nSc != 0u ? reinterpret_cast<float*>(mem + oScPrev) : nullptr;
  for (std::uint32_t k = 0, n = 0; k < nD; ++k) {
    r.sidechainIndex[k] = v.Column<bk::col::DUCK_kind>()[k] == 0u ? n++ : kNoSlot;
  }
  // Effect instances (3.3: DSP instance pool memory of this bank).
  r.effectCount = nX;
  r.effects = reinterpret_cast<EffectInstance*>(mem + oFx);
  float* params = reinterpret_cast<float*>(mem + oParams);
  if (nX != 0u) {
    snd_effect_desc* descs = static_cast<snd_effect_desc*>(im.alloc->Allocate(sizeof(snd_effect_desc) * nX, 16, MemTag::Misc));
    std::uint32_t* sizes = static_cast<std::uint32_t*>(im.alloc->Allocate(sizeof(std::uint32_t) * nX, 16, MemTag::Misc));
    if (descs == nullptr || sizes == nullptr) {
      if (descs != nullptr) im.alloc->Free(descs, MemTag::Misc);
      if (sizes != nullptr) im.alloc->Free(sizes, MemTag::Misc);
      im.alloc->Free(mem, MemTag::Tables);
      return false;
    }
    const bk::ByteSpan efbl = v.Chunk(bf::chunk::EFBL);
    std::uint64_t total = 0;
    for (std::uint32_t x = 0; x < nX; ++x) {
      std::uint32_t ch = im.cfg.outChannels;
      for (std::uint32_t b = 0; b < nB; ++b) {
        const std::uint32_t f = v.Column<bk::col::BUSS_effectFirst>()[b];
        const std::uint32_t c = v.Column<bk::col::BUSS_effectCount>()[b];
        if (f != bf::kNone && x >= f && x < f + c) {
          ch = r.busChannels[b];
          break;
        }
      }
      snd_effect_desc& d = descs[x];
      d = snd_effect_desc{};
      d.effect_type = v.Column<bk::col::EFFX_effectType>()[x];
      d.channels = ch;
      d.max_frames = im.cfg.blockFrames;
      const std::uint32_t pf = v.Column<bk::col::EFFX_paramFirst>()[x];
      const std::uint32_t pc = pf == bf::kNone ? 0u : v.Column<bk::col::EFFX_paramCount>()[x];
      d.param_count = pc;
      d.initial_params = pc != 0u ? v.Column<bk::col::EFPV_value>().data() + pf : nullptr;
      const std::uint64_t bb = v.Column<bk::col::EFFX_blobBytes>()[x];
      d.blob = bb != 0u && efbl.data != nullptr ? efbl.data + v.Column<bk::col::EFFX_blobOffset>()[x] : nullptr;
      d.blob_bytes = bb;
      d.sample_rate = 0;  // EffectInstanceBytes fills it from the rate argument (D-48)
      sizes[x] = dsp::EffectInstanceBytes(d, im.cfg.sampleRate, im.config.plugins.effects,
                                          im.config.plugins.effect_count);
      total += sizes[x];
    }
    if (total != 0u) {
      r.dspMemory = static_cast<std::uint8_t*>(im.alloc->Allocate(static_cast<std::size_t>(total), 64, MemTag::DspPool));
      if (r.dspMemory == nullptr) {
        im.alloc->Free(descs, MemTag::Misc);
        im.alloc->Free(sizes, MemTag::Misc);
        im.alloc->Free(mem, MemTag::Tables);
        return false;
      }
      r.dspBytes = total;
    }
    std::uint64_t at = 0;
    for (std::uint32_t x = 0; x < nX; ++x) {
      EffectInstance& e = r.effects[x];
      snd_effect_desc d = descs[x];
      d.sample_rate = im.cfg.sampleRate;
      e.type = d.effect_type;
      e.channels = d.channels;
      e.params = params + std::size_t{x} * dsp::kMaxEffectParams;
      e.ctl = reinterpret_cast<EffectParamControl*>(mem + oCtl) + std::size_t{x} * dsp::kMaxEffectParams;
      e.blockStart = reinterpret_cast<float*>(mem + oBlock) + std::size_t{x} * 2u * dsp::kMaxEffectParams;
      e.blockEnd = e.blockStart + dsp::kMaxEffectParams;
      e.paramCount = d.param_count < dsp::kMaxEffectParams ? d.param_count : dsp::kMaxEffectParams;
      for (std::uint32_t k = 0; k < e.paramCount; ++k) e.params[k] = d.initial_params[k];
      e.plugin = nullptr;
      for (std::uint32_t p = 0; p < im.config.plugins.effect_count; ++p) {
        if (im.config.plugins.effects[p].effect_type == d.effect_type) {
          e.plugin = &im.config.plugins.effects[p];
          break;
        }
      }
      if (sizes[x] == 0u) continue;  // unknown type: not processed
      e.state = r.dspMemory + at;
      at += sizes[x];
      if (e.plugin != nullptr) {
        e.plugin->init(e.plugin->user, e.state, &d, im.cfg.sampleRate);
        e.ok = 1u;
      } else {
        const dsp::EffectInfo* info = dsp::FindEffect(d.effect_type);
        if (info != nullptr && dsp::EffectInit(e.state, sizes[x], d, im.cfg.sampleRate)) {
          e.ok = 1u;
          // Missing parameters take the kind's defaults (ParamBlock covers all of them).
          float defaults[dsp::kMaxEffectParams] = {};
          dsp::EffectDefaultParams(info->kind, defaults);
          for (std::uint32_t k = e.paramCount; k < info->paramCount && k < dsp::kMaxEffectParams; ++k) {
            e.params[k] = defaults[k];
          }
          if (info->paramCount > e.paramCount) e.paramCount = info->paramCount;
        }
      }
      // D-130: controls start at the bank values, no ramp.
      for (std::uint32_t k = 0; k < e.paramCount; ++k) {
        e.ctl[k] = EffectParamControl{TimedRamp{e.params[k], e.params[k], 0u, 0u, 0u}, 0.0f, e.params[k]};
        e.blockStart[k] = e.params[k];
        e.blockEnd[k] = e.params[k];
      }
    }
    im.alloc->Free(descs, MemTag::Misc);
    im.alloc->Free(sizes, MemTag::Misc);
  }
  im.records[reg.bank.slot] = &r;
  return true;
}

void FreeBankRecord(Impl& im, std::uint32_t slot) noexcept {
  BankRecord* r = im.records[slot];
  if (r == nullptr) return;
  im.records[slot] = nullptr;
  if (r->dspMemory != nullptr) im.alloc->Free(r->dspMemory, MemTag::DspPool);
  void* mem = r->memory;
  r->~BankRecord();
  im.alloc->Free(mem, MemTag::Tables);
}

// ---------------------------------------------------------------------------
// Audio thread: registration (ApplyCommands)
// ---------------------------------------------------------------------------
namespace {

void WriteBusDefinition(Impl& im, SlotIndex slot, const BankRecord& r, std::uint32_t row) noexcept {
  schema::Tables& t = im.tables;
  const bk::BankView& v = *r.view;
  BusDef& d = im.busDefs[slot];
  d.defBank = static_cast<std::uint16_t>(r.handle.slot);
  d.defRow = row;
  d.channels = r.busChannels[row];
  d.flags = v.Column<bk::col::BUSS_flags>()[row];
  d.baseVolumeDb = v.Column<bk::col::BUSS_volumeDb>()[row];
  const std::uint32_t dn = base::rows::DenseOf(t.Bus.rows, slot);
  const std::uint32_t parent = v.Column<bk::col::BUSS_parent>()[row];
  SND_W(Bus_parentSlot)[dn] = parent < r.busCount ? r.busSlot[parent] : kNoSlot;
  const std::uint32_t sf = v.Column<bk::col::BUSS_sendFirst>()[row];
  SND_W(Bus_sendRange)[dn] = sf == bf::kNone ? RangeU32{0, 0} : RangeU32{sf, v.Column<bk::col::BUSS_sendCount>()[row]};
  const std::uint32_t ef = v.Column<bk::col::BUSS_effectFirst>()[row];
  SND_W(Bus_effectRange)[dn] =
      ef == bf::kNone ? RangeU32{0, 0} : RangeU32{ef, v.Column<bk::col::BUSS_effectCount>()[row]};
}

void WriteCategoryDefinition(Impl& im, SlotIndex slot, const BankRecord& r, std::uint32_t row) noexcept {
  schema::Tables& t = im.tables;
  const bk::BankView& v = *r.view;
  CatDef& d = im.catDefs[slot];
  d.flags = v.Column<bk::col::CATG_flags>()[row];
  d.baseVolumeDb = v.Column<bk::col::CATG_volumeDb>()[row];
  const std::uint32_t parent = v.Column<bk::col::CATG_parent>()[row];
  d.parentSlot = parent < r.catCount ? r.catSlot[parent] : im.globalCategory;
  SND_W(Category_flags)[base::rows::DenseOf(t.Category.rows, slot)] = d.flags;
}

// Finds another registered bank that has the id in its mapping (definition move).
template <class F>
bool FindOtherDefinition(Impl& im, std::uint32_t exceptBank, F&& has) noexcept {
  for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
    if (s == exceptBank) continue;
    const BankAudio& b = im.ap.banks[s];
    if ((b.state != BankAudioState::Registered && b.state != BankAudioState::Unloading) || b.rec == nullptr) continue;
    if (has(*b.rec)) return true;
  }
  return false;
}

}  // namespace

void ApplyBankRegister(Impl& im, const cmd::BankRegister& reg) noexcept {
  schema::Tables& t = im.tables;
  if (reg.bank.slot >= im.cfg.maxBanks) return;
  BankRecord* rec = im.records[reg.bank.slot];
  if (rec == nullptr || rec->handle.slot != reg.bank.slot || rec->handle.generation != reg.bank.generation) return;
  BankAudio& ba = im.ap.banks[reg.bank.slot];
  if (ba.state != BankAudioState::Free) return;  // already registered
  const bk::BankView& v = *rec->view;
  BankRecord& r = *rec;
  const std::uint16_t bankSlot = static_cast<std::uint16_t>(reg.bank.slot);

  // Buses (A1-16): one row per id.
  for (std::uint32_t b = 0; b < r.busCount; ++b) {
    const Id id = v.Column<bk::col::BUSS_id>()[b];
    std::uint32_t slot = im.busMap.Find(id);
    if (slot != IdMap::kNotFound) {
      ++im.busDefs[slot].refCount;
      r.busSlot[b] = slot;
      continue;
    }
    slot = im.busFree.Pop();
    if (slot == base::kInvalidIndex) {
      r.busSlot[b] = kNoSlot;  // over CoreLimits::maxBuses: routed to the master
      PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, id);
      continue;
    }
    const std::uint32_t d = base::rows::InsertNext(SND_ROWS(Bus), slot);
    im.busMap.Set(id, slot);
    im.busDefs[slot] = BusDef{};
    im.busDefs[slot].id = id;
    im.busDefs[slot].refCount = 1u;
    im.busDefs[slot].defBank = kNoBank;  // defined in pass 2
    SND_W(Bus_busId)[d] = id;
    SND_W(Bus_volume)[d] = TimedRamp{1.0f, 1.0f, 0, 0, 0};
    SND_W(Bus_graphOrder)[d] = 0;
    r.busSlot[b] = slot;
  }
  for (std::uint32_t b = 0; b < r.busCount; ++b) {
    const std::uint32_t slot = r.busSlot[b];
    if (slot != kNoSlot && im.busDefs[slot].defBank == kNoBank) WriteBusDefinition(im, slot, r, b);
  }
  // Categories.
  for (std::uint32_t c = 0; c < r.catCount; ++c) {
    const Id id = v.Column<bk::col::CATG_id>()[c];
    std::uint32_t slot = im.catMap.Find(id);
    if (slot != IdMap::kNotFound) {
      ++im.catDefs[slot].refCount;
      r.catSlot[c] = slot;
      continue;
    }
    slot = im.catFree.Pop();
    if (slot == base::kInvalidIndex) {
      r.catSlot[c] = im.globalCategory;
      PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, id);
      continue;
    }
    const std::uint32_t d = base::rows::InsertNext(SND_ROWS(Category), slot);
    im.catMap.Set(id, slot);
    im.catDefs[slot] = CatDef{id, 1u, kNoSlot, 0u, 0.0f};
    SND_W(Category_categoryId)[d] = id;
    SND_W(Category_request)[d] = CategoryRequest{TimedRamp{1.0f, 1.0f, 0, 0, 0}, 0u, 0u};
    SND_W(Category_flags)[d] = 0u;
    r.catSlot[c] = slot;  // defined by this bank in the pass below
  }
  for (std::uint32_t c = r.catCount; c-- > 0;) {  // parents come later: define them first
    const std::uint32_t slot = r.catSlot[c];
    if (slot < im.cfg.catCap && slot != im.globalCategory && im.catDefs[slot].refCount == 1u &&
        im.catDefs[slot].parentSlot == kNoSlot) {
      WriteCategoryDefinition(im, slot, r, c);
    }
  }
  // Snapshots.
  for (std::uint32_t s = 0; s < r.snapCount; ++s) {
    const Id id = v.Column<bk::col::SNAP_id>()[s];
    std::uint32_t slot = im.snapMap.Find(id);
    if (slot != IdMap::kNotFound) {
      ++im.snapDefs[slot].refCount;
      r.snapSlot[s] = slot;
      continue;
    }
    slot = im.snapFree.Pop();
    if (slot == base::kInvalidIndex) {
      r.snapSlot[s] = kNoSlot;
      continue;
    }
    const std::uint32_t d = base::rows::InsertNext(SND_ROWS(Snapshot), slot);
    im.snapMap.Set(id, slot);
    im.snapDefs[slot] = SnapDef{id, 1u, bankSlot, 0, s};
    SND_W(Snapshot_snapshotId)[d] = id;
    SND_W(Snapshot_request)[d] = SnapshotRequest{TimedRamp{0.0f, 0.0f, 0, 0, 0}};
    r.snapSlot[s] = slot;
  }
  // Voice limit groups.
  for (std::uint32_t g = 0; g < r.groupCount; ++g) {
    const Id id = v.Column<bk::col::VLGR_id>()[g];
    std::uint32_t slot = im.groupMap.Find(id);
    if (slot != IdMap::kNotFound) {
      ++im.groupDefs[slot].refCount;
      r.groupSlot[g] = slot;
      continue;
    }
    slot = im.groupFree.Pop();
    if (slot == base::kInvalidIndex) {
      r.groupSlot[g] = kNoSlot;
      continue;
    }
    im.groupMap.Set(id, slot);
    im.groupDefs[slot] =
        GroupDef{id, 1u, v.Column<bk::col::VLGR_maxVoices>()[g], v.Column<bk::col::VLGR_stealMode>()[g]};
    r.groupSlot[g] = slot;
  }
  // Parameter definitions.
  const std::uint32_t nP = v.Rows(bf::chunk::PARM);
  for (std::uint32_t p = 0; p < nP; ++p) {
    const Id id = v.Column<bk::col::PARM_id>()[p];
    std::uint32_t slot = im.paramDefMap.Find(id);
    if (slot != IdMap::kNotFound) {
      ++im.paramDefs[slot].refCount;
      continue;
    }
    slot = im.paramDefFree.Pop();
    if (slot == base::kInvalidIndex) continue;
    im.paramDefMap.Set(id, slot);
    ParamDef& pd = im.paramDefs[slot];
    pd.id = id;
    pd.refCount = 1u;
    pd.kind = v.Column<bk::col::PARM_kind>()[p];
    pd.scopeMask = v.Column<bk::col::PARM_scopeMask>()[p];
    pd.minValue = v.Column<bk::col::PARM_minValue>()[p];
    pd.maxValue = v.Column<bk::col::PARM_maxValue>()[p];
    pd.defaultValue = v.Column<bk::col::PARM_defaultValue>()[p];
    pd.rampRate = v.Column<bk::col::PARM_rampRate>()[p];
  }
  // Ducking rules (deduplicated by kind + source id + target id).
  for (std::uint32_t k = 0; k < r.duckCount; ++k) {
    DuckRule n{};
    n.kind = v.Column<bk::col::DUCK_kind>()[k];
    const std::uint32_t src = v.Column<bk::col::DUCK_source>()[k];
    const std::uint32_t tgt = v.Column<bk::col::DUCK_targetBus>()[k];
    if (n.kind == 0u) {
      n.sourceSlot = src < r.busCount ? r.busSlot[src] : kNoSlot;
      n.sourceId = src < r.busCount ? v.Column<bk::col::BUSS_id>()[src] : 0u;
    } else {
      n.sourceSlot = src < r.catCount ? r.catSlot[src] : kNoSlot;
      n.sourceId = src < r.catCount ? v.Column<bk::col::CATG_id>()[src] : 0u;
    }
    n.targetBus = tgt < r.busCount ? r.busSlot[tgt] : kNoSlot;
    n.targetId = tgt < r.busCount ? v.Column<bk::col::BUSS_id>()[tgt] : 0u;
    n.amountDb = v.Column<bk::col::DUCK_amountDb>()[k];
    n.attackMs = v.Column<bk::col::DUCK_attackMs>()[k];
    n.releaseMs = v.Column<bk::col::DUCK_releaseMs>()[k];
    n.thresholdDb = v.Column<bk::col::DUCK_thresholdDb>()[k];
    std::uint32_t found = kNoSlot;
    for (std::uint32_t i = 0; i < im.cfg.duckCap; ++i) {
      const DuckRule& e = im.duckRules[i];
      if (e.refCount != 0u && e.kind == n.kind && e.sourceId == n.sourceId && e.targetId == n.targetId) {
        found = i;
        break;
      }
    }
    if (found != kNoSlot) {
      ++im.duckRules[found].refCount;
      r.duckSlot[k] = found;
      continue;
    }
    const std::uint32_t slot = im.duckFree.Pop();
    if (slot == base::kInvalidIndex) {
      r.duckSlot[k] = kNoSlot;
      continue;
    }
    n.refCount = 1u;
    im.duckRules[slot] = n;
    im.dp.duckDb[slot] = 0.0f;
    r.duckSlot[k] = slot;
  }
  ba.rec = rec;
  ba.generation = reg.bank.generation;
  ba.state = BankAudioState::Registered;
  ba.order = ++im.ap.registrationOrder;
  ba.discardSent = 0;
  for (std::uint32_t e = 0; e < r.eventCount; ++e) r.events[e] = EventState{};
  RebuildBusGraph(im);
  // LOADING -> LOADED (an unload that came first keeps UNLOADING).
  std::uint32_t expected = SND_BANK_STATE_LOADING;
  im.bankStates[reg.bank.slot].state.compare_exchange_strong(expected, SND_BANK_STATE_LOADED,
                                                              std::memory_order_acq_rel);
  if (ba.pendingUnload != 0u && ba.pendingGeneration == reg.bank.generation) {
    ba.state = BankAudioState::Unloading;
    ba.unloadMode = ba.pendingMode;
  }
  ba.pendingUnload = 0;
}

void UnregisterBank(Impl& im, std::uint32_t bankSlot) noexcept {
  schema::Tables& t = im.tables;
  BankAudio& ba = im.ap.banks[bankSlot];
  BankRecord& r = *ba.rec;
  const bk::BankView& v = *r.view;
  ba.state = BankAudioState::AwaitEpoch;  // excluded from the definition search below
  ba.unregisterBlock = im.block;
  // Sources drop what they cached of this bank (PCM cache) once it is unused (stage 2).
  if (im.desc.bankUnregistered != nullptr) im.desc.bankUnregistered(im.desc.bankUser, static_cast<std::uint16_t>(bankSlot));
  // Buses.
  for (std::uint32_t b = 0; b < r.busCount; ++b) {
    const std::uint32_t slot = r.busSlot[b];
    if (slot == kNoSlot) continue;
    BusDef& d = im.busDefs[slot];
    if (--d.refCount == 0u) {
      RemoveBusRow(t, slot);
      im.busMap.Erase(d.id);
      d = BusDef{};
      im.busFree.Push(slot);
      continue;
    }
    if (d.defBank == bankSlot) {
      d.defBank = kNoBank;
      FindOtherDefinition(im, bankSlot, [&](const BankRecord& o) {
        for (std::uint32_t ob = 0; ob < o.busCount; ++ob) {
          if (o.busSlot[ob] == slot) {
            WriteBusDefinition(im, slot, o, ob);
            return true;
          }
        }
        return false;
      });
    }
  }
  // Categories.
  for (std::uint32_t c = 0; c < r.catCount; ++c) {
    const std::uint32_t slot = r.catSlot[c];
    if (slot == kNoSlot || slot == im.globalCategory) continue;
    CatDef& d = im.catDefs[slot];
    if (--d.refCount == 0u) {
      RemoveCategoryRow(t, slot);
      im.catMap.Erase(d.id);
      d = CatDef{};
      im.catFree.Push(slot);
    }
  }
  // Re-point parents of categories that lost theirs (rare: shared child, unshared parent).
  for (std::uint32_t s = 0; s < im.cfg.catCap; ++s) {
    CatDef& d = im.catDefs[s];
    if (d.refCount != 0u && d.parentSlot != kNoSlot && im.catDefs[d.parentSlot].refCount == 0u) {
      d.parentSlot = im.globalCategory;
    }
  }
  // Snapshots.
  for (std::uint32_t s = 0; s < r.snapCount; ++s) {
    const std::uint32_t slot = r.snapSlot[s];
    if (slot == kNoSlot) continue;
    SnapDef& d = im.snapDefs[slot];
    if (--d.refCount == 0u) {
      RemoveSnapshotRow(t, slot);
      im.snapMap.Erase(d.id);
      d = SnapDef{};
      im.snapFree.Push(slot);
      continue;
    }
    if (d.defBank == bankSlot) {
      FindOtherDefinition(im, bankSlot, [&](const BankRecord& o) {
        for (std::uint32_t os = 0; os < o.snapCount; ++os) {
          if (o.snapSlot[os] == slot) {
            d.defBank = static_cast<std::uint16_t>(o.handle.slot);
            d.defRow = os;
            return true;
          }
        }
        return false;
      });
    }
  }
  // Groups.
  for (std::uint32_t g = 0; g < r.groupCount; ++g) {
    const std::uint32_t slot = r.groupSlot[g];
    if (slot == kNoSlot) continue;
    GroupDef& d = im.groupDefs[slot];
    if (--d.refCount == 0u) {
      im.groupMap.Erase(d.id);
      d = GroupDef{};
      im.ev.groupLive[slot] = 0;
      im.groupFree.Push(slot);
    }
  }
  // Parameter definitions.
  const std::uint32_t nP = v.Rows(bf::chunk::PARM);
  for (std::uint32_t p = 0; p < nP; ++p) {
    const Id id = v.Column<bk::col::PARM_id>()[p];
    const std::uint32_t slot = im.paramDefMap.Find(id);
    if (slot == IdMap::kNotFound) continue;
    ParamDef& pd = im.paramDefs[slot];
    if (--pd.refCount == 0u) {
      im.paramDefMap.Erase(id);
      pd = ParamDef{};
      im.paramDefFree.Push(slot);
    }
  }
  // Ducking.
  for (std::uint32_t k = 0; k < r.duckCount; ++k) {
    const std::uint32_t slot = r.duckSlot[k];
    if (slot == kNoSlot) continue;
    if (--im.duckRules[slot].refCount == 0u) {
      im.duckRules[slot] = DuckRule{};
      im.duckFree.Push(slot);
    }
  }
  // Ducking rules whose source / target went away keep working through ids only when re-registered.
  for (std::uint32_t i = 0; i < im.cfg.duckCap; ++i) {
    DuckRule& d = im.duckRules[i];
    if (d.refCount == 0u) continue;
    if (d.targetBus != kNoSlot && im.busDefs[d.targetBus].id != d.targetId) d.targetBus = im.busMap.Find(d.targetId);
    if (d.kind == 0u && d.sourceSlot != kNoSlot && im.busDefs[d.sourceSlot].id != d.sourceId) {
      d.sourceSlot = im.busMap.Find(d.sourceId);
    }
  }
  RebuildBusGraph(im);
}

// Topological order of the bus graph (4.6): a bus comes before its parent and
// before the targets of its sends; ties by slot (deterministic). Sends that
// would close a cycle (possible only across banks) are ignored at mix time.
void RebuildBusGraph(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  const schema::TableRows& rows = t.Bus.rows;
  const std::uint32_t cap = im.cfg.busCap;
  std::uint32_t* indeg = im.frameArena.AllocateArray<std::uint32_t>(cap);
  std::uint8_t* done = im.frameArena.AllocateArray<std::uint8_t>(cap);
  if (indeg == nullptr || done == nullptr) return;
  for (std::uint32_t s = 0; s < cap; ++s) {
    indeg[s] = 0;
    done[s] = 0;
  }
  const SlotIndex* parent = SND_W(Bus_parentSlot);
  const RangeU32* sends = SND_W(Bus_sendRange);
  auto forEachTarget = [&](std::uint32_t slot, auto&& fn) {
    const std::uint32_t dn = base::rows::DenseOf(rows, slot);
    if (parent[dn] != kNoSlot && base::rows::IsLive(rows, parent[dn])) fn(parent[dn]);
    const BusDef& def = im.busDefs[slot];
    const BankRecord* br = RegisteredBank(im, def.defBank);
    if (br == nullptr) {
      // The defining bank is being unregistered right now: use the record directly.
      if (def.defBank < im.cfg.maxBanks) br = im.ap.banks[def.defBank].rec;
    }
    if (br == nullptr) return;
    const RangeU32 sr = sends[dn];
    for (std::uint32_t k = 0; k < sr.count; ++k) {
      const std::uint32_t tb = br->view->Column<bk::col::SEND_targetBus>()[sr.first + k];
      if (tb < br->busCount && br->busSlot[tb] != kNoSlot && base::rows::IsLive(rows, br->busSlot[tb])) {
        fn(br->busSlot[tb]);
      }
    }
  };
  for (std::uint32_t s = 0; s < cap; ++s) {
    if (base::rows::IsLive(rows, s)) forEachTarget(s, [&](std::uint32_t tgt) { ++indeg[tgt]; });
  }
  std::uint32_t order = 0;
  std::uint32_t* go = SND_W(Bus_graphOrder);
  for (;;) {
    std::uint32_t pick = kNoSlot;
    for (std::uint32_t s = 0; s < cap; ++s) {
      if (base::rows::IsLive(rows, s) && done[s] == 0u && indeg[s] == 0u) {
        pick = s;
        break;
      }
    }
    if (pick == kNoSlot) {  // cycle: take the lowest remaining slot
      for (std::uint32_t s = 0; s < cap; ++s) {
        if (base::rows::IsLive(rows, s) && done[s] == 0u) {
          pick = s;
          break;
        }
      }
    }
    if (pick == kNoSlot) break;
    done[pick] = 1u;
    go[base::rows::DenseOf(rows, pick)] = order++;
    forEachTarget(pick, [&](std::uint32_t tgt) {
      if (indeg[tgt] > 0u) --indeg[tgt];
    });
  }
  // Depth (for diagnostics) and the master slot.
  im.rd.masterSlot = kNoSlot;
  for (std::uint32_t s = 0; s < cap; ++s) {
    if (!base::rows::IsLive(rows, s)) continue;
    const std::uint32_t dn = base::rows::DenseOf(rows, s);
    if (parent[dn] == kNoSlot && (im.busDefs[s].flags & bf::kBusOutput) == 0u &&
        (im.rd.masterSlot == kNoSlot || (im.busDefs[s].flags & bf::kBusMaster) != 0u)) {
      if (im.rd.masterSlot == kNoSlot || (im.busDefs[im.rd.masterSlot].flags & bf::kBusMaster) == 0u) {
        im.rd.masterSlot = s;
      }
    }
  }
  im.ap.graphDirty = 1u;
}

}  // namespace snd::core
