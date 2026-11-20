// Material table editor model (design.md 9.1, 9.3, 11.1 マテリアル表エディタ;
// project-format.md materialTable -> PHYS).
//
// The last material cannot be removed (D-177d: the build needs one).
// Materials are numbered by their position in "materials" (the number the game passes as
// a sound material, 9.2). Adding at the end keeps every number; inserting, moving or
// removing renumbers the materials after the position: the edit returns the old -> new
// numbers (Renumbering) so the screen can warn that game code using numbers must follow.
// The table's other members name materials, so they follow a rename / removal (one undo
// step): single / footsteps / curves keys, pairs (a pair naming a removed material goes).
//
// Pairs are symmetric (9.3 "対称な N×N 表"): (a, b) and (b, a) are one entry; setting one
// replaces the other.
//
// Resolve (9.3 代替順, D-176 candidate in the notes): for a contact of materials a and b of
// kind k: the pair's event if set (the PHYS pair table has no contact kind: a pair
// overrides every kind); otherwise each material's single event of kind k, a material
// without one taking the global default of kind k; the same event from both sides sounds
// once; nothing set: no sound. Footsteps: the material's footstep event, else none.
// Fallbacks() lists every (pair, kind) that does not sound the materials' own events.
//
// Strength curves (PhysCurves, bank-format.md §6.2): impulseToVelocity = 3 ascending
// layer thresholds (weak / medium / strong) + the impulse of full velocity; massToPitchCents
// = cents at 1 kg and at 100 kg; speedToVolumeDb = dB at 0.1 m/s and at 10 m/s. The
// runtime evaluation belongs to the physics module (session A, a stub when this was
// written); EvaluateStrength is the tool's provisional reading of those numbers
// (log-interpolated mass and speed, clamped; non-finite inputs count as 0, D-180b), to be
// checked against A's implementation.
#ifndef SND_TOOL_MATERIAL_EDITOR_HPP
#define SND_TOOL_MATERIAL_EDITOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/document.hpp"

namespace snd::tool {

enum class ContactKind : std::uint8_t { Impact = 0, Slide = 1, Roll = 2, Break = 3 };
const char* ContactName(ContactKind k);

struct StrengthCurves {
  float impulseToVelocity[4] = {0.1f, 1.0f, 10.0f, 100.0f};  // the build's defaults
  float massToPitchCents[2] = {0.0f, 0.0f};
  float speedToVolumeDb[2] = {-30.0f, 0.0f};
};

struct StrengthResult {
  int layer = -1;          // -1 below the first threshold (dropped), 0 weak, 1 medium, 2 strong
  float velocity = 0.0f;   // impulse / full-velocity impulse, 0..1
  float pitchCents = 0.0f; // from the mass
  float volumeDb = 0.0f;   // from the relative speed (slide / roll)
};
StrengthResult EvaluateStrength(const StrengthCurves& c, float impulse, float massKg, float speed);

struct Renumbering {
  std::vector<int> oldToNew;  // -1: removed
};

struct MaterialResolution {
  std::uint32_t a = 0, b = 0;  // material numbers (a <= b)
  ContactKind kind = ContactKind::Impact;
  std::vector<std::string> events;  // event GUIDs that sound
  std::string source;               // "pair", "single", "single+default", "default", "none"
};

class MaterialEditor {
 public:
  MaterialEditor(ProjectDocument& doc, std::string guid);
  bool valid() const;
  std::vector<std::string> Materials() const;
  int IndexOf(const std::string& material) const;

  bool AddMaterial(const std::string& name, std::uint32_t at, Renumbering* r = nullptr);  // at <= count
  bool RemoveMaterial(const std::string& name, Renumbering* r = nullptr);
  bool MoveMaterial(std::uint32_t from, std::uint32_t to, Renumbering* r = nullptr);
  bool RenameMaterial(const std::string& from, const std::string& to);

  bool SetSingle(const std::string& material, ContactKind k, const std::string& eventGuid);  // "" clears
  bool SetPair(const std::string& a, const std::string& b, const std::string& eventGuid);      // "" clears
  bool SetFootstep(const std::string& material, const std::string& eventGuid);
  bool SetDefault(ContactKind k, const std::string& eventGuid);
  bool SetCurves(const std::string& material, const StrengthCurves& c);
  StrengthCurves Curves(const std::string& material) const;

  std::string Single(const std::string& material, ContactKind k) const;  // GUID or ""
  std::string Pair(const std::string& a, const std::string& b) const;
  std::string Footstep(const std::string& material) const;
  std::string Default(ContactKind k) const;

  MaterialResolution Resolve(std::uint32_t a, std::uint32_t b, ContactKind k) const;
  std::vector<MaterialResolution> Fallbacks() const;

 private:
  std::string EventRef(const std::string& ref) const;  // a stored reference -> GUID
  ProjectDocument& doc_;
  std::string guid_;
};

}  // namespace snd::tool

#endif  // SND_TOOL_MATERIAL_EDITOR_HPP
