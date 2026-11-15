#include <cstring>

#include "fixture_writer.hpp"
#include "snd/id.hpp"

namespace fixture {

namespace c = bf::col;
namespace ch = bf::chunk;
using snd::HashId;
constexpr std::uint32_t N = bf::kNone;

static std::vector<std::uint8_t> Pod(const void* p, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  return std::vector<std::uint8_t>(b, b + n);
}

Bank RichBank() {
  Bank b;
  b.bankId = HashId("Common");
  b.flags = bf::kFileHasStrings | bf::kFileDevBuild;
  // Buses in graph order: inputs first, master last.
  b.Table(ch::BUSS, 3,
          {U32(c::BUSS_id, {HashId("SFX"), HashId("Reverb"), HashId("Master")}),
           U32(c::BUSS_parent, {2, 2, N}), U32(c::BUSS_sendFirst, {0, N, N}), U32(c::BUSS_sendCount, {1, 0, 0}),
           U32(c::BUSS_effectFirst, {0, 1, N}), U32(c::BUSS_effectCount, {1, 1, 0}),
           F32(c::BUSS_volumeDb, {-3.0f, 0.0f, 0.0f}), U32(c::BUSS_flags, {0, 0, bf::kBusMaster})});
  b.Table(ch::SEND, 1, {U32(c::SEND_targetBus, {1}), F32(c::SEND_levelDb, {-6.0f})});
  b.Table(ch::EFFX, 2,
          {U32(c::EFFX_effectType, {HashId("Lowpass"), HashId("Convolution")}), U32(c::EFFX_paramFirst, {0, 2}),
           U32(c::EFFX_paramCount, {2, 1}), U64(c::EFFX_blobOffset, {0, 0}), U64(c::EFFX_blobBytes, {0, 16}),
           U32(c::EFFX_stateBytes, {100, 200})});
  b.Table(ch::EFPV, 3, {F32(c::EFPV_value, {1000.0f, 0.7f, 0.5f})});
  b.Blob(ch::EFBL, std::vector<std::uint8_t>(16, 7));
  const bf::DspPoolNeed need{128 + 256, 2, 0};
  b.Blob(ch::DSPN, Pod(&need, sizeof(need)));
  b.Table(ch::PARM, 1, {U32(c::PARM_id, {HashId("Speed")}), U32(c::PARM_kind, {0}), F32(c::PARM_maxValue, {10.0f})});
  b.Table(ch::CURV, 2,
          {U32(c::CURV_pointFirst, {0, 2}), U32(c::CURV_pointCount, {2, 2}),
           U32(c::CURV_input, {bf::kInputDistance, HashId("Speed")}), U32(c::CURV_target, {0, 1})});
  b.Table(ch::CPNT, 4, {F32(c::CPNT_x, {0, 50, 0, 10}), F32(c::CPNT_y, {0, -40, 0, 1200})});
  b.Table(ch::CATG, 2,
          {U32(c::CATG_id, {HashId("UI"), HashId("SE")}), U32(c::CATG_flags, {bf::kCategoryIgnorePause, 0})});
  b.Table(ch::VLGR, 1, {U32(c::VLGR_id, {HashId("Footsteps")}), U32(c::VLGR_maxVoices, {8})});
  b.Table(ch::EVNT, 3,
          {U32(c::EVNT_id, {HashId("SE/Explosion"), HashId("SE/Nested"), HashId("Music/Main")}),
           U32(c::EVNT_trackFirst, {0, 2, N}), U32(c::EVNT_trackCount, {2, 1, 0}),
           U32(c::EVNT_sequenceKind, {1, 0, 0}), U32(c::EVNT_priority, {200, 128, 250}), U32(c::EVNT_bus, {0, 0, 2}),
           U32(c::EVNT_category, {1, N, N}), U32(c::EVNT_limitGroup, {0, N, N}),
           U32(c::EVNT_flags, {bf::kEvent3D, 0, bf::kEventMusic}), U32(c::EVNT_curveVolume, {0, N, N}),
           U32(c::EVNT_musicState, {N, N, 0}), U32(c::EVNT_curveFirst, {1, N, N}), U32(c::EVNT_curveCount, {1, 0, 0})});
  b.Table(ch::TRAK, 3,
          {U32(c::TRAK_event, {0, 0, 1}), U32(c::TRAK_clipFirst, {0, 2, 3}), U32(c::TRAK_clipCount, {2, 1, 1}),
           U32(c::TRAK_actionFirst, {0, N, N}), U32(c::TRAK_actionCount, {1, 0, 0})});
  b.Table(ch::CLIP, 4,
          {U32(c::CLIP_kind, {0, 0, 1, 2}), U32(c::CLIP_target, {0, 1, 1, 0}), F32(c::CLIP_randPitchCents, {50, 0, 0, 0})});
  b.Table(ch::ACTN, 1,
          {F32(c::ACTN_timeMs, {100.0f}), U32(c::ACTN_action, {2}), U32(c::ACTN_target, {HashId("Speed")}),
           F32(c::ACTN_value, {0.5f})});
  b.Table(ch::ASST, 3,
          {U32(c::ASST_id, {HashId("w/boom"), HashId("w/amb"), HashId("w/seg")}), U32(c::ASST_loadForm, {0, 3, 0}),
           U32(c::ASST_codec, {SND_CODEC_PCM_F32, SND_CODEC_PCM_S16, SND_CODEC_PCM_S16}),
           U32(c::ASST_channels, {1, 2, 2}), U32(c::ASST_sampleRate, {48000, 48000, 48000}),
           U64(c::ASST_totalFrames, {64, 48000, 16}), U64(c::ASST_dataOffset, {0, 0, 256}),
           U64(c::ASST_dataBytes, {256, 0, 64}), U64(c::ASST_preloadOffset, {0, 0, 0}),
           U32(c::ASST_preloadBytes, {0, 64, 0}), U32(c::ASST_preloadFrames, {0, 16, 0}),
           U32(c::ASST_seekFirst, {N, 0, N}), U32(c::ASST_seekCount, {0, 2, 0}), U32(c::ASST_stream, {N, 0, N})});
  std::vector<std::uint8_t> wave(320, 0);
  for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = static_cast<std::uint8_t>(i);
  b.Blob(ch::WAVE, wave);
  b.Blob(ch::PREL, std::vector<std::uint8_t>(64, 3));
  b.Table(ch::SEEK, 2, {U64(c::SEEK_frame, {0, 24000}), U64(c::SEEK_byteOffset, {0, 96000})});
  b.Table(ch::STRM, 1,
          {U32(c::STRM_pathOffset, {0}), U64(c::STRM_fileBytes, {4096}), U64(c::STRM_dataOffset, {2048})});
  b.Blob(ch::SPTH, Bytes("streams/amb.snds"));
  b.Table(ch::SYNP, 1, {U32(c::SYNP_kind, {0}), U32(c::SYNP_waveform, {2})});
  b.Table(ch::MTMP, 1, {Column{}, F32(c::MTMP_bpm, {120.0f})});
  b.Find(ch::MTMP)->columns[0] = Column{c::MTMP_tick, 8, std::vector<std::uint8_t>(8, 0), -1};
  b.Table(ch::MMRK, 1, {U32(c::MMRK_id, {HashId("Entry")}), U64(c::MMRK_frame, {0}), U32(c::MMRK_kind, {1})});
  b.Table(ch::MSEG, 1,
          {U32(c::MSEG_id, {HashId("Seg/A")}), U32(c::MSEG_asset, {2}), U32(c::MSEG_tempoFirst, {0}),
           U32(c::MSEG_tempoCount, {1}), U32(c::MSEG_bars, {4}), U32(c::MSEG_markerFirst, {0}),
           U32(c::MSEG_markerCount, {1})});
  b.Table(ch::MPLE, 1, {U32(c::MPLE_segment, {0})});
  b.Table(ch::MPLS, 1,
          {U32(c::MPLS_id, {HashId("PL/Explore")}), U32(c::MPLS_entryFirst, {0}), U32(c::MPLS_entryCount, {1})});
  b.Table(ch::MSTA, 1, {U32(c::MSTA_id, {HashId("Explore")}), U32(c::MSTA_playlist, {0}), U32(c::MSTA_musicEvent, {2})});
  b.Table(ch::MSTM, 1,
          {U32(c::MSTM_segment, {0}), U32(c::MSTM_channelFirst, {0}), U32(c::MSTM_channelCount, {2}),
           U32(c::MSTM_volumeCurve, {1})});
  b.Table(ch::MTRN, 1,
          {U32(c::MTRN_musicEvent, {2}), U32(c::MTRN_fromState, {0}), U32(c::MTRN_toState, {0}), U32(c::MTRN_timing, {2})});
  b.Table(ch::MSTG, 1, {U32(c::MSTG_id, {HashId("Sting")}), U32(c::MSTG_event, {0}), U32(c::MSTG_timing, {1})});
  b.Table(ch::SNAP, 1, {U32(c::SNAP_id, {HashId("Pause")}), U32(c::SNAP_valueFirst, {0}), U32(c::SNAP_valueCount, {2})});
  b.Table(ch::SNPV, 2,
          {U32(c::SNPV_targetKind, {0, 1}), U32(c::SNPV_targetRow, {2, 0}), U32(c::SNPV_paramIndex, {0, 1}),
           F32(c::SNPV_value, {-6.0f, 0.3f})});
  b.Table(ch::DUCK, 1,
          {U32(c::DUCK_kind, {0}), U32(c::DUCK_source, {0}), U32(c::DUCK_targetBus, {1}), F32(c::DUCK_amountDb, {-6.0f})});
  // "SE/Explosion\0Master\0SE/OldExplosion\0"
  std::vector<std::uint8_t> strs = Bytes("SE/Explosion");
  const std::uint32_t masterOff = static_cast<std::uint32_t>(strs.size());
  for (std::uint8_t x : Bytes("Master")) strs.push_back(x);
  const std::uint32_t aliasOff = static_cast<std::uint32_t>(strs.size());
  for (std::uint8_t x : Bytes("SE/OldExplosion")) strs.push_back(x);
  b.Blob(ch::STRS, strs, bf::kChunkStrippable);
  b.Table(ch::NAME, 3,
          {U32(c::NAME_id, {HashId("SE/Explosion"), HashId("Master"), HashId("SE/Explosion")}),
           U32(c::NAME_stringOffset, {0, masterOff, aliasOff}), U32(c::NAME_kind, {0, 1, 6})})
      .flags |= bf::kChunkStrippable;
  b.Table(ch::GUID, 2,
          {U32(c::GUID_id, {HashId("SE/Explosion"), HashId("Master")}), U64(c::GUID_guidLo, {0x1111, 0x3333}),
           U64(c::GUID_guidHi, {0x2222, 0x4444})})
      .flags |= bf::kChunkDevOnly;
  // PHYS: header, one table with N = 2 materials.
  {
    std::vector<std::uint8_t> p(16 + 64, 0);
    const std::uint32_t n = 2;
    bf::PhysChunkHeader ph{1, 0, 16};
    std::memcpy(p.data(), &ph, sizeof(ph));
    const std::uint64_t single = p.size();
    p.resize(p.size() + n * 16, 0);
    const std::uint64_t pair = p.size();
    p.resize(p.size() + n * n * 4, 0);
    const std::uint64_t foot = p.size();
    p.resize(p.size() + n * 4, 0);
    const std::uint64_t curves = p.size();
    p.resize(p.size() + n * 32, 0);
    bf::PhysTable t{};
    t.id = HashId("Default");
    t.materialCount = n;
    t.singleOffset = single;
    t.pairOffset = pair;
    t.footstepOffset = foot;
    t.curveOffset = curves;
    t.defaultEvent[0] = HashId("SE/Explosion");
    std::memcpy(p.data() + 16, &t, sizeof(t));
    const std::uint32_t pairEvent = HashId("SE/Nested");
    std::memcpy(p.data() + pair + (0 * n + 1) * 4, &pairEvent, 4);
    b.Blob(ch::PHYS, p, bf::kChunkApplication);
  }
  // Unknown application chunk (newer minor / game data): ignored by validation, readable.
  b.Blob(snd::Fourcc('X', 'T', 'R', 'A'), Bytes("hello"), bf::kChunkApplication);
  return b;
}

}  // namespace fixture
