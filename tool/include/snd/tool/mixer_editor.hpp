// Mixer structure model (design.md 4.6, 11.1 ミキサー; project-format.md bus, category,
// snapshot, voiceLimitGroup, ducking): buses and the graph, sends, effects, snapshots,
// ducking, categories, voice limit groups. Edits the ProjectDocument (undoable); the live
// values of a running game stay in MixerModel (mixer.hpp). Rebuild the Project view from
// the document (Project::Load(doc)) and MixerModel::Bind it after structural edits.
//
// Rules (the build's, refused before they reach the document):
//   - The bus tree has exactly one root, the master (project-format.md §4): a new bus needs
//     a parent; the master cannot get one; a parent change that makes a cycle is refused.
//   - Sends and parents together must stay acyclic (graph order: inputs first, master
//     last, BUSS.parent and SEND.targetBus point to later rows); a send to itself is refused.
//   - Effect parameters: the dsp table's range (clamped) and integer rounding, the same
//     normalisation as MixerModel::Normalize; unknown (plugin) types take any finite value.
//   - Category parents acyclic. Voice limit groups: maxVoices an integer >= 1.
//   - Ducking: a "bank" (it is only built into its bank), sidechain source = bus,
//     rule source = category, target = bus; attack / release >= 0.
//   - Removing a bus / category with children or still referenced is refused.
//
// Snapshots (4.6): values are busVolume (dB added to the bus's volume, weighted: the
// runtime adds weight x value, sys_dsp.cpp), effectParam and sendLevel (absolute targets;
// with weight w the result moves from the project value toward the target by w per
// snapshot; the runtime applies them from stage 3). ApplySnapshots computes the result
// of any set of weights. CaptureSnapshot writes the mixer's current state (MixerModel)
// into a snapshot.
#ifndef SND_TOOL_MIXER_EDITOR_HPP
#define SND_TOOL_MIXER_EDITOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/document.hpp"
#include "snd/tool/mixer.hpp"

namespace snd::tool {

enum class BusFlag : std::uint8_t { Output, LodBypassable, Meter };
enum class SnapshotKind : std::uint8_t { BusVolume = 0, EffectParam = 1, SendLevel = 2 };
enum class DuckKind : std::uint8_t { Sidechain = 0, Rule = 1 };
enum class StealMode : std::uint8_t { Oldest = 0, Quietest = 1, LowestPriority = 2, Never = 3 };
enum class DuckField : std::uint8_t { AmountDb, AttackMs, ReleaseMs, ThresholdDb };

struct SnapshotValue {
  SnapshotKind kind = SnapshotKind::BusVolume;
  std::string bus;          // GUID
  double value = 0.0;
  std::uint32_t effect = 0;  // EffectParam
  std::uint32_t param = 0;   // EffectParam
  std::uint32_t send = 0;    // SendLevel
};

// One target of the weighted result.
struct SnapshotTarget {
  SnapshotKind kind = SnapshotKind::BusVolume;
  std::string bus;
  std::uint32_t effect = 0, param = 0, send = 0;
  double projectValue = 0.0;  // the bus / effect / send value in the project
  double value = 0.0;         // after the snapshots
};

struct EffectTypeInfo {
  std::string name;  // "snd.fx.lowpass"
  std::vector<EffectParamDesc> params;
};
// The standard effect types of the dsp table, in name order.
std::vector<EffectTypeInfo> StandardEffectTypes();

class MixerEditor {
 public:
  explicit MixerEditor(ProjectDocument& doc) : doc_(doc) {}

  // --- buses -----------------------------------------------------------------------------
  std::string MasterBus() const;  // GUID ("" if none)
  // A new bus under `parentGuid` ("" only when the project has no bus yet: the master).
  std::string AddBus(const std::string& name, const std::string& parentGuid);
  bool RemoveBus(const std::string& bus);
  bool SetParent(const std::string& bus, const std::string& parentGuid);
  bool SetVolume(const std::string& bus, double db, std::uint32_t mergeKey = 0);
  bool SetChannels(const std::string& bus, std::uint32_t channels);  // 0 = parent's, 1..8
  bool SetFlag(const std::string& bus, BusFlag f, bool on);
  bool SetLodPriority(const std::string& bus, std::uint32_t priority);
  std::vector<std::string> Children(const std::string& bus) const;  // name order
  // Graph order of project-format.md §4 (inputs first, master last, ties by name). Empty
  // if the graph has a cycle.
  std::vector<std::string> GraphOrder() const;

  // --- sends -----------------------------------------------------------------------------
  bool AddSend(const std::string& bus, const std::string& target, double levelDb, bool preFader = false);
  bool RemoveSend(const std::string& bus, std::uint32_t i);
  bool SetSendLevel(const std::string& bus, std::uint32_t i, double db, std::uint32_t mergeKey = 0);
  bool SetSendPreFader(const std::string& bus, std::uint32_t i, bool preFader);
  bool SetSendTarget(const std::string& bus, std::uint32_t i, const std::string& target);

  // --- effects ---------------------------------------------------------------------------
  // A new effect at index `at` (<= count) with every parameter at the dsp default.
  bool AddEffect(const std::string& bus, const std::string& type, std::uint32_t at);
  bool RemoveEffect(const std::string& bus, std::uint32_t e);
  bool MoveEffect(const std::string& bus, std::uint32_t from, std::uint32_t to);
  bool SetEffectParam(const std::string& bus, std::uint32_t e, std::uint32_t p, double v, std::uint32_t mergeKey = 0);
  double EffectParam(const std::string& bus, std::uint32_t e, std::uint32_t p) const;  // the dsp default when absent
  bool SetEffectFlags(const std::string& bus, std::uint32_t e, std::uint32_t flags);
  bool SetEffectBlob(const std::string& bus, std::uint32_t e, const std::string& path);  // "" removes
  // The value SetEffectParam stores (dsp range and rounding).
  static double NormalizeEffectParam(const std::string& type, std::uint32_t p, double v);

  // --- snapshots -------------------------------------------------------------------------
  std::string AddSnapshot(const std::string& name, const std::string& bankGuid);
  bool AddSnapshotValue(const std::string& snap, const SnapshotValue& v);
  bool SetSnapshotValue(const std::string& snap, std::uint32_t i, const SnapshotValue& v);
  bool RemoveSnapshotValue(const std::string& snap, std::uint32_t i);
  std::vector<SnapshotValue> SnapshotValues(const std::string& snap) const;
  // Replaces the values with the mixer's current state: per bus whose fader differs from
  // the project, busVolume = fader - project volume (dB offset); per editable effect
  // parameter that differs, effectParam = the value. Muted buses are taken as their fader.
  bool CaptureSnapshot(const std::string& snap, const MixerModel& mixer);
  // Weighted result (weights clamped to 0..1, D-85) of the given snapshots.
  std::vector<SnapshotTarget> ApplySnapshots(const std::vector<std::pair<std::string, double>>& weights) const;

  // --- ducking ---------------------------------------------------------------------------
  std::string AddDucking(const std::string& bankGuid, DuckKind kind, const std::string& source,
                         const std::string& target, double amountDb);
  bool SetDuckingKind(const std::string& duck, DuckKind kind, const std::string& source);
  bool SetDuckingTarget(const std::string& duck, const std::string& target);
  bool SetDucking(const std::string& duck, DuckField f, double v, std::uint32_t mergeKey = 0);

  // --- categories ------------------------------------------------------------------------
  std::string AddCategory(const std::string& name, const std::string& parentGuid);
  bool RemoveCategory(const std::string& cat);
  bool SetCategoryParent(const std::string& cat, const std::string& parentGuid);
  bool SetCategoryVolume(const std::string& cat, double db, std::uint32_t mergeKey = 0);
  bool SetCategoryFlag(const std::string& cat, bool ignorePause, bool muteOnFocusLoss);

  // --- voice limit groups ----------------------------------------------------------------
  std::string AddLimitGroup(const std::string& name, std::uint32_t maxVoices, StealMode steal);
  bool SetMaxVoices(const std::string& group, std::uint32_t maxVoices);
  bool SetSteal(const std::string& group, StealMode steal);

 private:
  const DocObject* Bus(const std::string& g) const;
  std::string ParentOf(const DocObject& bus) const;  // GUID
  bool BusGraphAcyclic(const std::string& bus, const std::string& newParent, const std::string& extraSendTarget) const;
  ProjectDocument& doc_;
};

// Writes the mixer's live values (MixerModel, 11.2 上書き層) back into the document as one
// undo step: bus faders -> bus "volumeDb", effect parameters -> "effects[e].params[p]",
// category faders -> category "volumeDb" (11.2 "正しいデータは常にツール側にある"). Mute,
// solo and parameter overrides are monitoring states and are not written. Returns the
// number of members written. The mixer's own history is unchanged; bind it to the new
// Project (Project::Load(doc)) afterwards so that the written values become its bank values.
std::uint32_t WriteBackMixer(ProjectDocument& doc, const MixerModel& mixer);

}  // namespace snd::tool

#endif  // SND_TOOL_MIXER_EDITOR_HPP
