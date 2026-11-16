// System 3: EvaluateLogicParams (4.3). Moves ParamValue.current toward the
// target at the parameter's ramp rate (units per second, 0 = immediate) for
// the three scope tables; switches copy the value at once. A row seen for
// the first time starts at the parameter's default (PARM), so a value set for
// the first time ramps from the default. Emitter occlusion / obstruction come
// from the emitter-scope parameters "snd.occlusion" / "snd.obstruction"
// (ramped like any parameter; their effect on the sound is stage 3).
#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::EvaluateLogicParams;
constexpr Id kOcclusionParam = HashId("snd.occlusion");
constexpr Id kObstructionParam = HashId("snd.obstruction");

float DefaultOf(const Impl& im, Id id, float fallback) noexcept {
  const std::uint32_t d = im.paramDefMap.Find(id);
  return d == IdMap::kNotFound || im.paramDefs[d].kind != 0u ? fallback : im.paramDefs[d].defaultValue;
}

template <class Cols>
void Step(Impl& im, const schema::TableRows& rows, std::uint32_t* seen, const Id* ids, const ParamKind* kinds,
          const float* targets, const float* rates, float* current, float seconds) noexcept {
  (void)sizeof(Cols);
  for (std::uint32_t d = 0; d < rows.count; ++d) {
    const SlotIndex s = base::rows::SlotAt(rows, d);
    const float target = targets[d];
    if (seen[s] != rows.generation[s]) {
      seen[s] = rows.generation[s];
      current[d] = kinds[d] == ParamKind::Switch ? target : DefaultOf(im, ids[d], target);
    }
    if (kinds[d] == ParamKind::Switch || rates[d] <= 0.0f) {
      current[d] = target;
      continue;
    }
    const float step = rates[d] * seconds;
    const float c = current[d];
    current[d] = c < target ? (target - c <= step ? target : c + step) : (c - target <= step ? target : c - step);
  }
}
}  // namespace

void EvaluateLogicParams(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  const float seconds = static_cast<float>(im.cfg.blockFrames) / static_cast<float>(im.cfg.sampleRate);
  Step<int>(im, t.ParamGlobal.rows, im.lp.globalGen, SND_R(ParamGlobal_paramId), SND_R(ParamGlobal_kind),
            SND_R(ParamGlobal_target), SND_R(ParamGlobal_rampRate), SND_W(ParamGlobal_current), seconds);
  Step<int>(im, t.ParamEmitter.rows, im.lp.emitterGen, SND_R(ParamEmitter_paramId), SND_R(ParamEmitter_kind),
            SND_R(ParamEmitter_target), SND_R(ParamEmitter_rampRate), SND_W(ParamEmitter_current), seconds);
  Step<int>(im, t.ParamPlayback.rows, im.lp.playbackGen, SND_R(ParamPlayback_paramId), SND_R(ParamPlayback_kind),
            SND_R(ParamPlayback_target), SND_R(ParamPlayback_rampRate), SND_W(ParamPlayback_current), seconds);
  // Emitter occlusion (D-18: EvaluateLogicParams owns it, through the parameter ramp).
  Occlusion* occ = SND_W(Emitter_occlusion);
  for (std::uint32_t d = 0; d < t.Emitter.rows.count; ++d) occ[d] = Occlusion{0.0f, 0.0f};
  const Id* ids = SND_R(ParamEmitter_paramId);
  const SlotIndex* owner = SND_R(ParamEmitter_ownerSlot);
  const float* cur = SND_W(ParamEmitter_current);
  for (std::uint32_t d = 0; d < t.ParamEmitter.rows.count; ++d) {
    if (ids[d] != kOcclusionParam && ids[d] != kObstructionParam) continue;
    const std::uint32_t ed = base::rows::DenseOf(t.Emitter.rows, owner[d]);
    if (ed == kNoSlot) continue;
    const float v = cur[d] < 0.0f ? 0.0f : (cur[d] > 1.0f ? 1.0f : cur[d]);
    if (ids[d] == kOcclusionParam) {
      occ[ed].occlusion = v;
    } else {
      occ[ed].obstruction = v;
    }
  }
}

}  // namespace snd::core
