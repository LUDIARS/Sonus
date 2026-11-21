// Mixer model (design.md 11.1, 11.2).
#include "snd/tool/mixer.hpp"

#include <algorithm>
#include <cmath>

#include "snd/format/bank_format.hpp"

namespace snd::tool {
namespace {

namespace bf = snd::bank_format;

bool Valid(std::int32_t i, std::size_t n) { return i >= 0 && static_cast<std::size_t>(i) < n; }

float ClampFader(float db) {
  if (std::isnan(db)) return kFaderOffDb;
  return std::clamp(db, kFaderOffDb, kFaderMaxDb);
}

}  // namespace

void MixerModel::Bind(const Project& project) {
  project_ = &project;
  buses_.clear();
  categories_.clear();
  params_.clear();
  for (const BusDesc& b : project.buses()) {
    BusStrip s;
    s.faderDb = ClampFader(b.volumeDb);
    for (const EffectDesc& e : b.effects) {
      std::vector<float> v;
      for (const EffectParamDesc& p : e.params) v.push_back(p.projectValue);
      s.params.push_back(std::move(v));
    }
    buses_.push_back(std::move(s));
  }
  for (const CategoryDesc& c : project.categories()) categories_.push_back(CategoryStrip{ClampFader(c.volumeDb), false});
  for (const ParameterDesc& p : project.parameters()) params_.push_back(ParamState{false, p.defaultValue});
  history_.Clear();
  ++revision_;
}

bool MixerModel::IsEditable(const Target& t) const {
  if (project_ == nullptr) return false;
  switch (t.control) {
    case Control::BusFader:
    case Control::BusMute:
    case Control::BusSolo: return Valid(t.a, buses_.size());
    case Control::EffectParam: {
      if (!Valid(t.a, buses_.size())) return false;
      const BusDesc& b = project_->buses()[static_cast<std::size_t>(t.a)];
      if (!Valid(t.b, b.effects.size())) return false;
      const EffectDesc& e = b.effects[static_cast<std::size_t>(t.b)];
      return Valid(t.c, e.params.size()) && !e.params[static_cast<std::size_t>(t.c)].initOnly;
    }
    case Control::CategoryFader:
    case Control::CategoryMute: return Valid(t.a, categories_.size());
    case Control::ParamOverride:
    case Control::ParamOverrideOn:
      return Valid(t.a, params_.size()) && project_->parameters()[static_cast<std::size_t>(t.a)].global;
  }
  return false;
}

float MixerModel::Normalize(const Target& t, float v) const {
  switch (t.control) {
    case Control::BusFader:
    case Control::CategoryFader: return ClampFader(v);
    case Control::BusMute:
    case Control::BusSolo:
    case Control::CategoryMute:
    case Control::ParamOverrideOn: return v != 0.0f && !std::isnan(v) ? 1.0f : 0.0f;
    case Control::EffectParam: {
      if (!IsEditable(t)) return v;
      const EffectParamDesc& p = project_->buses()[static_cast<std::size_t>(t.a)]
                                     .effects[static_cast<std::size_t>(t.b)]
                                     .params[static_cast<std::size_t>(t.c)];
      if (std::isnan(v)) v = p.projectValue;
      v = std::clamp(v, p.minValue, p.maxValue);
      return p.integer ? std::round(v) : v;
    }
    case Control::ParamOverride: {
      if (!IsEditable(t)) return v;
      const ParameterDesc& p = project_->parameters()[static_cast<std::size_t>(t.a)];
      if (std::isnan(v)) v = p.defaultValue;
      const float lo = std::min(p.minValue, p.maxValue), hi = std::max(p.minValue, p.maxValue);
      return std::clamp(v, lo, hi);
    }
  }
  return v;
}

float MixerModel::Get(const Target& t) const {
  if (!IsEditable(t) && t.control != Control::EffectParam) return 0.0f;
  switch (t.control) {
    case Control::BusFader: return buses_[static_cast<std::size_t>(t.a)].faderDb;
    case Control::BusMute: return buses_[static_cast<std::size_t>(t.a)].muted ? 1.0f : 0.0f;
    case Control::BusSolo: return buses_[static_cast<std::size_t>(t.a)].soloed ? 1.0f : 0.0f;
    case Control::EffectParam: {
      if (!Valid(t.a, buses_.size())) return 0.0f;
      const auto& fx = buses_[static_cast<std::size_t>(t.a)].params;
      if (!Valid(t.b, fx.size()) || !Valid(t.c, fx[static_cast<std::size_t>(t.b)].size())) return 0.0f;
      return fx[static_cast<std::size_t>(t.b)][static_cast<std::size_t>(t.c)];
    }
    case Control::CategoryFader: return categories_[static_cast<std::size_t>(t.a)].faderDb;
    case Control::CategoryMute: return categories_[static_cast<std::size_t>(t.a)].muted ? 1.0f : 0.0f;
    case Control::ParamOverride: return params_[static_cast<std::size_t>(t.a)].value;
    case Control::ParamOverrideOn: return params_[static_cast<std::size_t>(t.a)].enabled ? 1.0f : 0.0f;
  }
  return 0.0f;
}

float MixerModel::BankValue(const Target& t) const {
  if (project_ == nullptr) return 0.0f;
  switch (t.control) {
    case Control::BusFader:
      return Valid(t.a, buses_.size()) ? ClampFader(project_->buses()[static_cast<std::size_t>(t.a)].volumeDb) : 0.0f;
    case Control::EffectParam: {
      if (!Valid(t.a, buses_.size())) return 0.0f;
      const BusDesc& b = project_->buses()[static_cast<std::size_t>(t.a)];
      if (!Valid(t.b, b.effects.size())) return 0.0f;
      const EffectDesc& e = b.effects[static_cast<std::size_t>(t.b)];
      return Valid(t.c, e.params.size()) ? e.params[static_cast<std::size_t>(t.c)].projectValue : 0.0f;
    }
    case Control::CategoryFader:
      return Valid(t.a, categories_.size())
                 ? ClampFader(project_->categories()[static_cast<std::size_t>(t.a)].volumeDb)
                 : 0.0f;
    case Control::ParamOverride:
      return Valid(t.a, params_.size()) ? project_->parameters()[static_cast<std::size_t>(t.a)].defaultValue : 0.0f;
    case Control::BusMute:
    case Control::BusSolo:
    case Control::CategoryMute:
    case Control::ParamOverrideOn: return 0.0f;
  }
  return 0.0f;
}

bool MixerModel::Store(const Target& t, float v) {
  if (!IsEditable(t)) return false;
  v = Normalize(t, v);
  const std::size_t a = static_cast<std::size_t>(t.a);
  switch (t.control) {
    case Control::BusFader: buses_[a].faderDb = v; break;
    case Control::BusMute: buses_[a].muted = v != 0.0f; break;
    case Control::BusSolo: buses_[a].soloed = v != 0.0f; break;
    case Control::EffectParam:
      buses_[a].params[static_cast<std::size_t>(t.b)][static_cast<std::size_t>(t.c)] = v;
      break;
    case Control::CategoryFader: categories_[a].faderDb = v; break;
    case Control::CategoryMute: categories_[a].muted = v != 0.0f; break;
    case Control::ParamOverride: params_[a].value = v; break;
    case Control::ParamOverrideOn: params_[a].enabled = v != 0.0f; break;
  }
  ++revision_;
  return true;
}

bool MixerModel::Set(const Target& t, float value, std::uint32_t mergeKey) {
  if (!IsEditable(t)) return false;
  const float before = Get(t);
  const float after = Normalize(t, value);
  if (before == after) return false;
  Store(t, after);
  history_.Push(Change{t, before, after}, mergeKey);
  return true;
}

void MixerModel::ApplyAll(const std::vector<Change>& changes) {
  for (const Change& c : changes) Store(c.target, c.after);
}

bool MixerModel::Undo() {
  std::vector<Change> apply;
  if (!history_.Undo(&apply)) return false;
  ApplyAll(apply);
  return true;
}

bool MixerModel::Redo() {
  std::vector<Change> apply;
  if (!history_.Redo(&apply)) return false;
  ApplyAll(apply);
  return true;
}

bool MixerModel::AnySolo() const {
  for (const BusStrip& s : buses_) {
    if (s.soloed) return true;
  }
  return false;
}

bool MixerModel::SoloMuted(std::int32_t bus) const {
  if (project_ == nullptr || !Valid(bus, buses_.size()) || !AnySolo()) return false;
  for (std::size_t s = 0; s < buses_.size(); ++s) {
    if (!buses_[s].soloed) continue;
    const std::int32_t si = static_cast<std::int32_t>(s);
    if (project_->IsBusUnder(bus, si) || project_->IsBusUnder(si, bus)) return false;
  }
  return true;
}

float MixerModel::EffectiveBusDb(std::int32_t bus) const {
  if (!Valid(bus, buses_.size())) return kFaderOffDb;
  const BusStrip& s = buses_[static_cast<std::size_t>(bus)];
  return (s.muted || SoloMuted(bus)) ? kFaderOffDb : s.faderDb;
}

float MixerModel::EffectiveCategoryDb(std::int32_t c) const {
  if (!Valid(c, categories_.size())) return kFaderOffDb;
  const CategoryStrip& s = categories_[static_cast<std::size_t>(c)];
  return s.muted ? kFaderOffDb : s.faderDb;
}

std::vector<Cell> MixerModel::Cells() const {
  std::vector<Cell> out;
  if (project_ == nullptr) return out;
  for (std::size_t i = 0; i < buses_.size(); ++i) {
    const BusDesc& b = project_->buses()[i];
    Cell c;
    c.address = CellAddress{bf::chunk::BUSS, b.id, bf::col::BUSS_volumeDb, 0, b.guid};
    c.value = EffectiveBusDb(static_cast<std::int32_t>(i));
    c.bankValue = ClampFader(b.volumeDb);
    c.owner = Target{Control::BusFader, static_cast<std::int32_t>(i), 0, 0};
    out.push_back(c);
  }
  for (std::size_t i = 0; i < buses_.size(); ++i) {
    const BusDesc& b = project_->buses()[i];
    for (std::size_t e = 0; e < b.effects.size(); ++e) {
      for (std::size_t p = 0; p < b.effects[e].params.size(); ++p) {
        if (b.effects[e].params[p].initOnly) continue;
        Cell c;
        c.address = CellAddress{bf::chunk::EFPV, b.id, bf::col::EFPV_value,
                                static_cast<std::uint32_t>((e << 16) | p), b.guid};
        c.value = buses_[i].params[e][p];
        c.bankValue = b.effects[e].params[p].projectValue;
        c.owner = Target{Control::EffectParam, static_cast<std::int32_t>(i), static_cast<std::int32_t>(e),
                         static_cast<std::int32_t>(p)};
        out.push_back(c);
      }
    }
  }
  for (std::size_t i = 0; i < categories_.size(); ++i) {
    const CategoryDesc& d = project_->categories()[i];
    Cell c;
    c.address = CellAddress{bf::chunk::CATG, d.id, bf::col::CATG_volumeDb, 0, d.guid};
    c.value = EffectiveCategoryDb(static_cast<std::int32_t>(i));
    c.bankValue = ClampFader(d.volumeDb);
    c.owner = Target{Control::CategoryFader, static_cast<std::int32_t>(i), 0, 0};
    out.push_back(c);
  }
  return out;
}

std::vector<ParamCell> MixerModel::ParamCells() const {
  std::vector<ParamCell> out;
  if (project_ == nullptr) return out;
  for (std::size_t i = 0; i < params_.size(); ++i) {
    const ParameterDesc& p = project_->parameters()[i];
    if (!p.global) continue;
    out.push_back(ParamCell{p.id, params_[i].enabled, params_[i].value});
  }
  return out;
}

std::vector<OverrideItem> MixerModel::Overrides() const {
  std::vector<OverrideItem> out;
  if (project_ == nullptr) return out;
  for (const Cell& c : Cells()) {
    if (c.value == c.bankValue) continue;
    OverrideItem it;
    it.target = c.owner;
    it.bankValue = c.bankValue;
    it.value = c.value;
    const std::size_t a = static_cast<std::size_t>(c.owner.a);
    switch (c.owner.control) {
      case Control::BusFader: {
        const BusDesc& b = project_->buses()[a];
        it.label = b.name + " volume";
        it.muted = buses_[a].muted || SoloMuted(c.owner.a);
        it.key = "bus." + b.key + ".vol";
        break;
      }
      case Control::EffectParam: {
        const BusDesc& b = project_->buses()[a];
        const EffectDesc& e = b.effects[static_cast<std::size_t>(c.owner.b)];
        it.label = b.name + " " + e.type + " " + e.params[static_cast<std::size_t>(c.owner.c)].name;
        it.key = "fx." + b.key + "." + std::to_string(c.owner.b) + "." + std::to_string(c.owner.c);
        break;
      }
      case Control::CategoryFader: {
        const CategoryDesc& d = project_->categories()[a];
        it.label = d.name + " volume";
        it.muted = categories_[a].muted;
        it.key = "cat." + d.key + ".vol";
        break;
      }
      default: break;
    }
    out.push_back(std::move(it));
  }
  for (std::size_t i = 0; i < params_.size(); ++i) {
    if (!params_[i].enabled) continue;
    const ParameterDesc& p = project_->parameters()[i];
    OverrideItem it;
    it.target = Target{Control::ParamOverrideOn, static_cast<std::int32_t>(i), 0, 0};
    it.label = "parameter " + p.name;
    it.bankValue = p.defaultValue;
    it.value = params_[i].value;
    it.key = "par." + p.key;
    out.push_back(std::move(it));
  }
  return out;
}

bool MixerModel::Revert(const OverrideItem& item) {
  const Target& t = item.target;
  if (!IsEditable(t)) return false;
  BeginGroup();
  bool changed = false;
  switch (t.control) {
    case Control::BusFader: {
      changed |= Set(t, BankValue(t));
      changed |= Set(Target{Control::BusMute, t.a, 0, 0}, 0.0f);
      changed |= Set(Target{Control::BusSolo, t.a, 0, 0}, 0.0f);
      if (SoloMuted(t.a)) {
        for (std::size_t i = 0; i < buses_.size(); ++i) {
          changed |= Set(Target{Control::BusSolo, static_cast<std::int32_t>(i), 0, 0}, 0.0f);
        }
      }
      break;
    }
    case Control::CategoryFader:
      changed |= Set(t, BankValue(t));
      changed |= Set(Target{Control::CategoryMute, t.a, 0, 0}, 0.0f);
      break;
    case Control::ParamOverrideOn:
    case Control::ParamOverride:  // off, and the tool's value back to the bank default
      changed |= Set(Target{Control::ParamOverrideOn, t.a, 0, 0}, 0.0f);
      changed |= Set(Target{Control::ParamOverride, t.a, 0, 0}, BankValue(Target{Control::ParamOverride, t.a, 0, 0}));
      break;
    default: changed |= Set(t, BankValue(t)); break;
  }
  EndGroup();
  return changed;
}

bool MixerModel::RevertOnly(const Target& t) {
  if (!IsEditable(t)) return false;
  if (t.control == Control::ParamOverrideOn || t.control == Control::ParamOverride) {
    BeginGroup();
    bool changed = Set(Target{Control::ParamOverrideOn, t.a, 0, 0}, 0.0f);
    changed |= Set(Target{Control::ParamOverride, t.a, 0, 0}, BankValue(Target{Control::ParamOverride, t.a, 0, 0}));
    EndGroup();
    return changed;
  }
  return Set(t, BankValue(t));
}

std::vector<CellAddress> MixerModel::CellsOf(const Target& t) const {
  std::vector<CellAddress> out;
  if (project_ == nullptr) return out;
  const auto& buses = project_->buses();
  switch (t.control) {
    case Control::BusFader:
    case Control::BusMute:
    case Control::BusSolo:
      if (t.a >= 0 && static_cast<std::size_t>(t.a) < buses.size()) {
        const BusDesc& b = buses[static_cast<std::size_t>(t.a)];
        out.push_back(CellAddress{bf::chunk::BUSS, b.id, bf::col::BUSS_volumeDb, 0, b.guid});
      }
      break;
    case Control::EffectParam:
      if (t.a >= 0 && static_cast<std::size_t>(t.a) < buses.size()) {
        const BusDesc& b = buses[static_cast<std::size_t>(t.a)];
        out.push_back(CellAddress{bf::chunk::EFPV, b.id, bf::col::EFPV_value,
                                  static_cast<std::uint32_t>((t.b << 16) | t.c), b.guid});
      }
      break;
    case Control::CategoryFader:
    case Control::CategoryMute:
      if (t.a >= 0 && static_cast<std::size_t>(t.a) < project_->categories().size()) {
        const CategoryDesc& d = project_->categories()[static_cast<std::size_t>(t.a)];
        out.push_back(CellAddress{bf::chunk::CATG, d.id, bf::col::CATG_volumeDb, 0, d.guid});
      }
      break;
    default: break;
  }
  return out;
}

bool MixerModel::RevertAll() {
  if (project_ == nullptr) return false;
  BeginGroup();
  bool changed = false;
  for (std::size_t i = 0; i < buses_.size(); ++i) {
    const std::int32_t a = static_cast<std::int32_t>(i);
    changed |= Set(Target{Control::BusFader, a, 0, 0}, BankValue(Target{Control::BusFader, a, 0, 0}));
    changed |= Set(Target{Control::BusMute, a, 0, 0}, 0.0f);
    changed |= Set(Target{Control::BusSolo, a, 0, 0}, 0.0f);
    for (std::size_t e = 0; e < buses_[i].params.size(); ++e) {
      for (std::size_t p = 0; p < buses_[i].params[e].size(); ++p) {
        const Target t{Control::EffectParam, a, static_cast<std::int32_t>(e), static_cast<std::int32_t>(p)};
        if (IsEditable(t)) changed |= Set(t, BankValue(t));
      }
    }
  }
  for (std::size_t i = 0; i < categories_.size(); ++i) {
    const std::int32_t a = static_cast<std::int32_t>(i);
    changed |= Set(Target{Control::CategoryFader, a, 0, 0}, BankValue(Target{Control::CategoryFader, a, 0, 0}));
    changed |= Set(Target{Control::CategoryMute, a, 0, 0}, 0.0f);
  }
  for (std::size_t i = 0; i < params_.size(); ++i) {
    const Target t{Control::ParamOverrideOn, static_cast<std::int32_t>(i), 0, 0};
    const Target v{Control::ParamOverride, static_cast<std::int32_t>(i), 0, 0};
    if (IsEditable(t)) changed |= Set(t, 0.0f);
    if (IsEditable(v)) changed |= Set(v, BankValue(v));
  }
  EndGroup();
  return changed;
}

}  // namespace snd::tool
