// Event editor model (design.md 4.2, 11.1 Event エディタ; project-format.md "event").
//
// A typed view over one event object of a ProjectDocument. Every setter is one undoable
// document step (or part of the caller's group); a setter returns false and changes
// nothing when the edit breaks a rule:
//   - startMs, fades, retriggerMs, fadeOutMs, comboWindowMs, maxDistance, random widths,
//     weight >= 0; loopCount an integer >= 0 (0 = infinite); priority 0..255;
//     maxInstances an integer >= 0; layer min <= max; cone 0 <= inner <= outer <= 360.
//   - a clip has exactly one source (asset / event / synth); a nested event must not be
//     the event itself or contain it (cycle, 4.2 "入れ子 ... 循環はビルドで検出");
//     references must name an object of the right kind (written as GUIDs).
// Members that the event's sequence kind does not use (weight outside random kinds,
// switchValue outside switch, layer outside layerCrossfade, comboWindowMs outside
// comboSequential) are kept and reported by UnusedMembers (the screen greys them out;
// the build ignores them too).
//
// The timeline (ComputeTimeline) is what the Event screen draws: per clip its start, its
// length (the asset's WAV: frames / rate, loop region from "loop" or the WAV's smpl chunk,
// repeated loopCount times, scaled by the pitch 2^(-cents/1200); a nested event: that
// event's length; a synth or loopCount 0: infinite), fades, the event's length and the
// overlaps of clips on one track.
#ifndef SND_TOOL_EVENT_EDITOR_HPP
#define SND_TOOL_EVENT_EDITOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/document.hpp"

namespace snd::tool {

enum class SequenceKind : std::uint8_t {
  Polyphonic = 0,
  Random = 1,
  RandomNoRepeat = 2,
  Sequential = 3,
  Shuffle = 4,
  Switch = 5,
  ComboSequential = 6,
  LayerCrossfade = 7,
};
const char* SequenceName(SequenceKind k);
bool ParseSequence(const std::string& s, SequenceKind* out);

// Which members a sequence kind reads (4.2).
struct SequenceUse {
  bool weight = false;          // clip weight: random, randomNoRepeat, shuffle
  bool switchValue = false;     // switch
  bool layer = false;           // clip layer + track layerParameter: layerCrossfade
  bool comboWindowMs = false;   // comboSequential
  bool allClipsPlay = false;    // polyphonic, layerCrossfade: every clip sounds; else one per play
};
SequenceUse UseOf(SequenceKind k);

enum class ClipNumber : std::uint8_t { StartMs, Weight, VolumeDb, PitchCents, FadeInMs, FadeOutMs, LoopCount };
enum class RandomField : std::uint8_t { VolumeDb, PitchCents, Pan, StartMs, FilterHz };
enum class EventNumber : std::uint8_t { MaxInstances, RetriggerMs, FadeOutMs, ComboWindowMs, MaxDistance, Priority };
enum class EventFlag : std::uint8_t { Is3D, IgnorePause, Stinger };
enum class ActionKind : std::uint8_t { Play = 0, Stop = 1, SetParameter = 2, ApplySnapshot = 3 };
enum class VirtualReturn : std::uint8_t { Continue = 0, Restart = 1, Never = 2 };

struct SynthDesc {
  std::string kind = "oscillator";  // oscillator | noise | granular | patch | wavetable
  std::string waveform = "sine";    // sine | saw | square | triangle | pulse
  double frequencyHz = 440.0;
  double duty = 0.5;
  std::string noiseColor = "white";
  std::string asset;                // granular / wavetable source (GUID)
  std::string patch;                // kind patch (GUID)
  std::uint32_t channels = 1;
};

struct ActionDesc {
  double timeMs = 0.0;
  ActionKind action = ActionKind::Play;
  std::string target;  // GUID of the event / parameter / snapshot
  double value = 0.0;
};

struct TimelineClip {
  std::uint32_t track = 0;
  std::uint32_t clip = 0;
  std::string source;      // "asset", "event", "synth" or "" (none)
  std::string sourceGuid;  // asset / nested event
  double startMs = 0.0;
  double passMs = 0.0;     // one pass of the material (no loops), pitch applied
  double endMs = 0.0;      // start + length; == startMs + passMs * loops for finite clips
  bool infinite = false;   // loopCount 0, a looping nested event, or a synth
  bool lengthKnown = true; // false: the material could not be read (length 0)
  double fadeInMs = 0.0;
  double fadeOutMs = 0.0;
};

struct TimelineOverlap {
  std::uint32_t track = 0;
  std::uint32_t clipA = 0;
  std::uint32_t clipB = 0;
  double fromMs = 0.0;
  double toMs = 0.0;  // +inf when an infinite clip is involved
};

struct TimelineAction {
  std::uint32_t track = 0;
  std::uint32_t action = 0;
  double timeMs = 0.0;
};

struct Timeline {
  std::vector<TimelineClip> clips;
  std::vector<TimelineAction> actions;
  std::vector<TimelineOverlap> overlaps;
  double lengthMs = 0.0;   // latest end of a finite clip (and action time)
  bool infinite = false;   // some clip that can sound never ends
  bool oneClipPerPlay = false;  // sequence plays one clip (lengthMs = the longest)
};

class EventEditor {
 public:
  EventEditor(ProjectDocument& doc, std::string guid);
  bool valid() const;
  const std::string& guid() const { return guid_; }
  const json::Value* event() const;

  // --- event members -------------------------------------------------------------------
  SequenceKind sequence() const;
  bool SetSequence(SequenceKind k);
  bool SetBus(const std::string& busGuid);            // "" = master (member removed)
  bool SetCategory(const std::string& categoryGuid);  // "" = none
  bool SetLimitGroup(const std::string& groupGuid);   // "" = none
  bool SetNumber(EventNumber n, double v, std::uint32_t mergeKey = 0);
  double NumberOf(EventNumber n) const;
  bool SetFlag(EventFlag f, bool on);
  bool Flag(EventFlag f) const;
  bool SetCone(double innerDeg, double outerDeg, double outerDb);
  // D-189: the cone in effect. Without a "cone" (or a member of it) the runtime's default: 0 / 0 /
  // 0 dB = no cone (core applies a cone only with outerDeg > 0). `present`: "cone" is written.
  struct ConeDesc {
    double innerDeg = 0.0, outerDeg = 0.0, outerDb = 0.0;
    bool present = false;
    bool active() const { return outerDeg > 0.0; }
  };
  ConeDesc Cone() const;
  bool SetVirtualReturn(VirtualReturn r);
  // Member paths set but not read with the current sequence kind.
  std::vector<std::string> UnusedMembers() const;

  // --- tracks --------------------------------------------------------------------------
  std::uint32_t TrackCount() const;
  bool AddTrack(std::uint32_t at);  // at <= TrackCount()
  bool RemoveTrack(std::uint32_t t);
  bool MoveTrack(std::uint32_t from, std::uint32_t to);
  bool SetTrackBus(std::uint32_t t, const std::string& busGuid);
  bool SetTrackLayerParameter(std::uint32_t t, const std::string& parameterGuid);
  bool SetTrackFlags(std::uint32_t t, std::uint32_t flags);

  // --- clips ---------------------------------------------------------------------------
  std::uint32_t ClipCount(std::uint32_t t) const;
  // New clip at the end of track t with one source (the other two empty).
  bool AddAssetClip(std::uint32_t t, const std::string& assetGuid, double startMs);
  bool AddEventClip(std::uint32_t t, const std::string& eventGuid, double startMs);
  bool AddSynthClip(std::uint32_t t, const SynthDesc& synth, double startMs);
  bool RemoveClip(std::uint32_t t, std::uint32_t c);
  bool MoveClip(std::uint32_t t, std::uint32_t c, double startMs, std::uint32_t mergeKey = 0);
  // Moves the clip to the end of track t2 at startMs (one step).
  bool MoveClipToTrack(std::uint32_t t, std::uint32_t c, std::uint32_t t2, double startMs);
  bool SetClipNumber(std::uint32_t t, std::uint32_t c, ClipNumber n, double v, std::uint32_t mergeKey = 0);
  double ClipNumberOf(std::uint32_t t, std::uint32_t c, ClipNumber n) const;
  bool SetClipRandom(std::uint32_t t, std::uint32_t c, RandomField f, double width, std::uint32_t mergeKey = 0);
  double ClipRandom(std::uint32_t t, std::uint32_t c, RandomField f) const;
  bool SetSwitchValue(std::uint32_t t, std::uint32_t c, const std::string& name);  // name -> snd_hash_id
  bool SetSwitchValue(std::uint32_t t, std::uint32_t c, std::uint32_t value);
  bool SetLayer(std::uint32_t t, std::uint32_t c, double minValue, double maxValue);
  bool SetSynth(std::uint32_t t, std::uint32_t c, const SynthDesc& synth);

  // --- actions -------------------------------------------------------------------------
  std::uint32_t ActionCount(std::uint32_t t) const;
  bool AddAction(std::uint32_t t, const ActionDesc& a);
  bool SetAction(std::uint32_t t, std::uint32_t i, const ActionDesc& a);
  bool RemoveAction(std::uint32_t t, std::uint32_t i);
  ActionDesc Action(std::uint32_t t, std::uint32_t i) const;

  // --- derived -------------------------------------------------------------------------
  Timeline ComputeTimeline() const;

 private:
  std::string ClipPath(std::uint32_t t, std::uint32_t c) const;
  bool ValidRef(const std::string& guid, const char* kind) const;
  bool WouldCycle(const std::string& nestedGuid) const;
  bool AddClip(std::uint32_t t, json::Value clip);
  ProjectDocument& doc_;
  std::string guid_;
};

// Length of one pass of an asset in ms (frames / rate; the loop region, start and end
// from "loop" or the WAV smpl chunk). false if the WAV cannot be read.
struct AssetLength {
  double frames = 0;
  double sampleRate = 0;
  double loopStart = 0;
  double loopEnd = 0;  // 0 = no loop region
  double Ms() const { return sampleRate > 0 ? frames * 1000.0 / sampleRate : 0.0; }
};
bool ReadAssetLength(const ProjectDocument& doc, const DocObject& asset, AssetLength* out);

}  // namespace snd::tool

#endif  // SND_TOOL_EVENT_EDITOR_HPP
