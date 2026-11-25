// Mixer model of the tool (design.md 11.1 ミキサー, 11.2 上書き層).
//
// State: per bus a fader (dB), mute, solo and the values of its effects'
// parameters; per category a fader and mute; per global parameter an optional
// override. Every edit goes through Set(), is recorded for undo / redo and
// bumps revision(). The model knows nothing about the network: Cells() turns
// the state into the values the game must hold (livelink SetValue targets),
// which CellSync sends and confirms.
//
// Rules (decided here, tested in tool/test):
//   - Fader range: kFaderOffDb (-inf, sent as dsp kMinDb = -144 dB, silence)
//     .. kFaderMaxDb (+12 dB). Values are clamped into it.
//   - Mute: the bus's BUSS.volumeDb is sent as -144 dB; the fader keeps its value.
//   - Solo: when any bus is soloed, every bus that is not soloed, not above a
//     soloed bus (its ancestors carry the soloed signal) and not below one is
//     muted ("solo-implied mute"). Sends are not followed (stage 2).
//   - Category mute: CATG.volumeDb sent as -144 dB.
//   - Effect parameters are clamped into the dsp range; integer parameters are
//     rounded; init-only parameters (dsp kInit) are not live-editable (Set
//     refuses them).
//   - Overrides(): every managed value that differs from the bank's (the
//     project's) value, plus the active parameter overrides. Revert(item) puts
//     that item back (one undo step); for a bus volume it also clears the bus's
//     mute and solo, and every solo if the bus is still solo-muted. RevertAll()
//     puts everything back as one undo step.
#ifndef SND_TOOL_MIXER_HPP
#define SND_TOOL_MIXER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/edit_history.hpp"
#include "snd/tool/project.hpp"

namespace snd::tool {

inline constexpr float kFaderOffDb = -144.0f;  // = dsp::kMinDb: gain 0
inline constexpr float kFaderMaxDb = 12.0f;

// What a managed value is in the game (livelink SetValue fields, protocol §5).
struct CellAddress {
  std::uint32_t chunk = 0;      // bank_format chunk fourcc (BUSS, EFPV, CATG)
  std::uint32_t runtimeId = 0;  // bus / category id
  std::uint32_t column = 0;     // bank_format column id
  std::uint32_t index = 0;      // EFPV: effect << 16 | parameter
  Guid guid;                    // the owner's GUID (SetValue.guidLo / guidHi)
  bool operator==(const CellAddress& o) const {
    return chunk == o.chunk && runtimeId == o.runtimeId && column == o.column && index == o.index;
  }
};

struct Cell {
  CellAddress address;
  float value = 0.0f;      // what the game should hold now
  float bankValue = 0.0f;  // what the bank holds (project)
  Target owner;            // the control that sets it (BusFader / EffectParam / CategoryFader)
};

struct ParamCell {  // OverrideParam (protocol §5)
  Id paramId = 0;
  bool enabled = false;
  float value = 0.0f;
};

struct OverrideItem {
  std::string label;     // "SFX volume", "SFX snd.fx.delay DryDb", "Param Speed"
  Target target;         // BusFader / EffectParam / CategoryFader / ParamOverrideOn
  float bankValue = 0.0f;
  float value = 0.0f;    // effective value sent to the game
  bool muted = false;    // bus / category volume forced to -inf by mute or solo
  std::string key;       // stable id ("bus.<id>.vol", "fx.<bus>.<e>.<p>", "cat.<id>.vol", "par.<id>")
};

// A live value of the mixer named by its object instead of its position (D-188): what a rebind to a
// new Project (the document's structure changed) carries over. `value` is the stored value of the
// control (dB, 0 / 1, the parameter's value).
struct LiveValue {
  Control control = Control::BusFader;  // never ParamOverride without its ParamOverrideOn
  Guid guid;                            // the bus / category / parameter
  std::string name;                     // its name (for the question when it cannot be carried)
  std::uint32_t effect = 0, param = 0;  // EffectParam
  std::string effectType;               // EffectParam: the effect's type at `effect`
  // EffectParam: the bus's effect types in order when the value was taken. Effects have no GUID, so
  // a value goes over only while the bus's chain is the same (count and order, D-188); when it is
  // empty only `effectType` at `effect` is compared.
  std::vector<std::string> effectChain;
  float value = 0.0f;
};

class MixerModel {
 public:
  struct BusStrip {
    float faderDb = 0.0f;
    bool muted = false;
    bool soloed = false;
    std::vector<std::vector<float>> params;  // [effect][parameter]
  };
  struct CategoryStrip {
    float faderDb = 0.0f;
    bool muted = false;
  };
  struct ParamState {
    bool enabled = false;
    float value = 0.0f;
  };

  // Starts from the project's values; clears the history.
  void Bind(const Project& project);
  // D-188: every value that differs from the bank (faders, mute, solo, effect parameters, category
  // faders and mutes, parameter overrides), by object.
  std::vector<LiveValue> LiveValues() const;
  // The values whose object (and, for an effect parameter, the same chain of effect types on the
  // bus, the effect type at that index and the parameter) is not in `project`: what a rebind would
  // lose. A bus whose effects changed in number or order loses its effect values (D-188: asked).
  static std::vector<LiveValue> Unmatched(const Project& project, const std::vector<LiveValue>& values);
  // Bind(project), then puts `values` back where their objects are (no history; normalized as Set
  // does). Returns the number put back; the others go to `dropped` (may be null).
  std::uint32_t Rebind(const Project& project, const std::vector<LiveValue>& values,
                       std::vector<LiveValue>* dropped = nullptr);
  const Project* project() const { return project_; }

  // --- edits (undoable). false if the target does not exist, is not editable, or the
  // value does not change. `mergeKey`: see EditHistory::Push.
  bool Set(const Target& target, float value, std::uint32_t mergeKey = 0);
  float Get(const Target& target) const;
  float BankValue(const Target& target) const;  // the bank's (project's) value; the "default"
  bool IsEditable(const Target& target) const;
  // Clamps / rounds a value the way Set would store it.
  float Normalize(const Target& target, float value) const;

  void BeginGroup() { history_.BeginGroup(); }
  void EndGroup() { history_.EndGroup(); }
  bool Undo();
  bool Redo();
  const EditHistory& history() const { return history_; }

  // --- derived --------------------------------------------------------------------------
  bool AnySolo() const;
  bool SoloMuted(std::int32_t bus) const;  // muted by someone else's solo
  float EffectiveBusDb(std::int32_t bus) const;
  float EffectiveCategoryDb(std::int32_t category) const;
  const std::vector<BusStrip>& buses() const { return buses_; }
  const std::vector<CategoryStrip>& categories() const { return categories_; }
  const std::vector<ParamState>& parameters() const { return params_; }

  // Every managed SetValue cell, in a stable order (bus volumes, effect parameters,
  // category volumes). Init-only effect parameters are left out.
  std::vector<Cell> Cells() const;
  std::vector<ParamCell> ParamCells() const;  // one per global parameter

  std::vector<OverrideItem> Overrides() const;
  bool Revert(const OverrideItem& item);
  bool RevertAll();
  // 1.1 (group P): "only this item". Puts the value of `target` alone back to the bank value
  // (one undo step; mute, solo and other controls stay as they are). Fader / category fader /
  // effect parameter: the value; ParamOverride / ParamOverrideOn: the override off and the value
  // back to the default (as Revert). false if nothing changed or not editable. Undo puts the
  // value back (an ordinary edit: the game gets a SetValue).
  bool RevertOnly(const Target& target);
  // The SetValue cells an item's value is sent through (CellSync::RequestRevert after a revert):
  // a bus fader -> its BUSS.volumeDb (also the solo-muted buses' when the bus is soloed is not
  // included: those cells change as ordinary edits), an effect parameter -> its EFPV cell, a
  // category fader -> CATG.volumeDb; parameter overrides have none (OverrideParam).
  std::vector<CellAddress> CellsOf(const Target& target) const;

  std::uint64_t revision() const { return revision_; }

 private:
  bool Store(const Target& t, float value);  // no history
  void ApplyAll(const std::vector<Change>& changes);
  const Project* project_ = nullptr;
  std::vector<BusStrip> buses_;
  std::vector<CategoryStrip> categories_;
  std::vector<ParamState> params_;
  EditHistory history_;
  std::uint64_t revision_ = 0;
};

}  // namespace snd::tool

#endif  // SND_TOOL_MIXER_HPP
