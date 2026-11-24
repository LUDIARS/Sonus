// Independent tests of group U (tool/test/indep_s7_ui_properties.md): an App over a document
// (the in-memory project of doc_fixture.hpp, or a generated large one), element lookup by id,
// prefix and label (the tests find elements in the declared tree, they do not rely on the
// implementation's id helpers), pointer gestures and keys.
#ifndef SND_TOOL_TEST_INDEP_S7_UI_FIXTURE_HPP
#define SND_TOOL_TEST_INDEP_S7_UI_FIXTURE_HPP

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <tela/runtime.hpp>

#include "doc_fixture.hpp"
#include "snd/tool/project.hpp"
#include "snd/tool/session.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/text_entry.hpp"
#include "snd/tool/ui/views.hpp"
#include "ui_helpers.hpp"

namespace iu {

using namespace snd::tool;
namespace ui = snd::tool::ui;
using doctest::G;

inline constexpr ui::Screen kAllScreens[ui::kScreenCount] = {
    ui::Screen::Mixer,    ui::Screen::Profiler, ui::Screen::Buses,  ui::Screen::Event,
    ui::Screen::Params,   ui::Screen::Spatial,  ui::Screen::Music,  ui::Screen::Material,
    ui::Screen::Patch,    ui::Screen::Replay,   ui::Screen::Link,   ui::Screen::Problems};

// The tool over a document. `files` empty: no document at all.
struct Rig {
  ProjectDocument doc;
  Project project;
  Session s;
  std::unique_ptr<ui::App> app;
  std::unique_ptr<uitest::Pointer> ptr;
  bool hasDoc = true;

  explicit Rig(const std::map<std::string, std::string>& files = doctest::BaseFiles(), bool withDoc = true,
               int width = 1280, int height = 800, float dpi = 1.0f, const std::string& dir = std::string()) {
    hasDoc = withDoc;
    ui::AppDesc d;
    d.width = width;
    d.height = height;
    d.dpiScale = dpi;
    if (withDoc) {
      doctest::LoadBase(doc, files, dir);
      project.Load(doc);
      s.UseProject(project);
      s.AttachDocument(&doc);
      d.workspace.document = &doc;
    }
    app = std::make_unique<ui::App>(s, d);
    ptr = std::make_unique<uitest::Pointer>(*app);
    Frame();
  }
  void Frame() { app->Frame(tooltest::Now()); }
  void Declare() { app->Declare(tooltest::Now()); }
  void Show(ui::Screen sc) {
    app->OnScreen(sc);
    Frame();
  }
  const tela::Runtime& rt() const { return app->runtime(); }
  const tela::PlacedElement* El(const std::string& id) const { return uitest::Find(rt(), id); }
  // Elements whose id starts with `prefix` (and, if given, contains `part`).
  std::vector<const tela::PlacedElement*> Prefixed(const std::string& prefix, const std::string& part = "") const {
    std::vector<const tela::PlacedElement*> v;
    for (const tela::PlacedElement& p : rt().elements()) {
      if (p.element.id.rfind(prefix, 0) == 0 && (part.empty() || p.element.id.find(part) != std::string::npos))
        v.push_back(&p);
    }
    return v;
  }
  // First element whose label equals `label` (buttons, texts).
  const tela::PlacedElement* Labelled(const std::string& label, const std::string& idPart = "") const {
    for (const tela::PlacedElement& p : rt().elements()) {
      if (p.element.label == label && (idPart.empty() || p.element.id.find(idPart) != std::string::npos)) return &p;
    }
    return nullptr;
  }
  bool AnyLabelContains(const std::string& needle) const {
    for (const tela::PlacedElement& p : rt().elements()) {
      if (p.element.label.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  std::size_t CountLabelContains(const std::string& needle) const {
    std::size_t n = 0;
    for (const tela::PlacedElement& p : rt().elements()) {
      if (p.element.label.find(needle) != std::string::npos) ++n;
    }
    return n;
  }
  std::vector<std::string> IdList() const {
    std::vector<std::string> v;
    for (const tela::PlacedElement& p : rt().elements()) v.push_back(p.element.id);
    return v;
  }
  std::set<std::string> Ids() const { return uitest::Ids(rt()); }
  bool Click(const tela::PlacedElement* p) {
    if (p == nullptr) return false;
    const tela::Point c = uitest::Center(p);
    ptr->Click(c.x, c.y);
    Frame();
    return true;
  }
  bool ClickId(const std::string& id) { return Click(El(id)); }
  bool ClickLabel(const std::string& label, const std::string& idPart = "") { return Click(Labelled(label, idPart)); }
  bool Key(std::uint32_t key, std::uint32_t mods = 0, std::uint32_t text = 0) {
    ui::KeyEvent k;
    k.key = key;
    k.modifiers = mods;
    k.text = text;
    const bool used = app->Key(k);
    Frame();
    return used;
  }
  void Type(const std::string& str) {
    for (unsigned char c : str) {
      ui::KeyEvent k;
      k.text = c;
      app->Key(k);
    }
    Frame();
  }
  // Drags from the centre of `p` by (dx, dy) logical pixels in `steps` moves (DPI 1).
  void Drag(const tela::PlacedElement* p, float dx, float dy, int steps = 4, std::uint32_t mods = 0,
            bool release = true) {
    if (p == nullptr) return;
    const tela::Point c = uitest::Center(p);
    DragFrom(c.x, c.y, dx, dy, steps, mods, release);
  }
  void DragFrom(float x, float y, float dx, float dy, int steps = 4, std::uint32_t mods = 0, bool release = true) {
    ptr->Down(x, y, mods);
    Frame();
    for (int i = 1; i <= steps; ++i) {
      ptr->Move(x + dx * static_cast<float>(i) / static_cast<float>(steps),
                y + dy * static_cast<float>(i) / static_cast<float>(steps), mods);
      Frame();
    }
    if (release) {
      ptr->Up(x + dx, y + dy);
      ptr->Advance(1000);
      Frame();
    }
  }
  std::size_t Undos() const { return doc.UndoCount(); }
};

inline std::string LongName(const std::string& head, std::size_t n) {
  std::string s = head;
  while (s.size() < n) s += "-abcdefghij";
  s.resize(n);
  return s;
}

// GUIDs of the large project: kind base + index.
inline std::string LG(std::uint32_t base, std::uint32_t i) { return G(0x100000u * base + i); }

// A large project (indep_s7_ui_properties.md G1): `events` events (one with 40 tracks x 30 clips,
// extreme start times), `buses` buses with long names, a music event with `rules` transition rows,
// a patch at the node limit, a material table of `materials` materials, a parameter with an
// extreme range. File names stay short (the names inside are long).
inline std::map<std::string, std::string> LargeFiles(std::uint32_t events = 1000, std::uint32_t buses = 100,
                                                     std::uint32_t rules = 150, std::uint32_t materials = 64,
                                                     std::size_t nameLength = 200) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  for (std::uint32_t i = 0; i < buses; ++i) {
    f["buses/b" + std::to_string(i) + ".json"] = "{\"type\": \"bus\", \"guid\": \"" + LG(1, i) + "\", \"name\": \"" +
                                                 LongName("Bus" + std::to_string(i), nameLength) +
                                                 "\", \"parent\": \"" + G(2) + "\", \"volumeDb\": " +
                                                 std::to_string(-(static_cast<int>(i % 60))) + "}\n";
  }
  f["parameters/Huge.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(20) +
                              "\", \"name\": \"Huge\", \"min\": -1e30, \"max\": 1e30, \"default\": 0, \"rampRate\": 1e300}\n";
  // The big event: 40 tracks x 30 clips.
  {
    std::string t = "{\"type\": \"event\", \"guid\": \"" + G(21) + "\", \"name\": \"Big\", \"bank\": \"Main\", \"tracks\": [";
    for (int tr = 0; tr < 40; ++tr) {
      t += tr ? ", {\"clips\": [" : "{\"clips\": [";
      for (int c = 0; c < 30; ++c) {
        const double start = (tr == 39 && c == 29) ? 1e9 : c * 150.0 + tr;
        t += std::string(c ? ", " : "") + "{\"asset\": \"" + G(7) + "\", \"startMs\": " + FormatNumber(start) + "}";
      }
      t += "]}";
    }
    t += "]}\n";
    f["events/Big.json"] = t;
  }
  for (std::uint32_t i = 0; i < events; ++i) {
    f["events/e" + std::to_string(i) + ".json"] =
        "{\"type\": \"event\", \"guid\": \"" + LG(2, i) + "\", \"name\": \"" +
        LongName("Ev" + std::to_string(i), i % 7 == 0 ? nameLength : 12) + "\", \"bank\": \"Main\", \"is3D\": " +
        (i % 2 ? "true" : "false") + ", \"maxDistance\": " + std::to_string(1 + i % 500) +
        ", \"tracks\": [{\"clips\": [{\"asset\": \"" + G(7) + "\", \"startMs\": " + std::to_string(i % 1000) + "}]}]}\n";
  }
  // Music: a segment, a playlist, a music event with `rules` rows (many unreachable).
  f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"Seg\", \"asset\": \"" + G(7) +
                           "\", \"bars\": 1, \"markers\": [{\"name\": \"M1\", \"frame\": 100, \"kind\": \"user\"}]}\n";
  f["playlists/PL.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(23) + "\", \"name\": \"PL\", \"mode\": \"sequential\", "
                           "\"entries\": [{\"segment\": \"" + G(22) + "\"}]}\n";
  {
    std::string m = "{\"type\": \"event\", \"guid\": \"" + G(24) + "\", \"name\": \"Music\", \"bank\": \"Main\", \"music\": {"
                    "\"initialState\": \"S0\", \"states\": [";
    for (int s = 0; s < 6; ++s) m += std::string(s ? ", " : "") + "{\"name\": \"S" + std::to_string(s) + "\", \"playlist\": \"" + G(23) + "\"}";
    m += "], \"transitions\": [";
    for (std::uint32_t r = 0; r < rules; ++r) {
      const std::string from = r % 5 == 4 ? "*" : "S" + std::to_string(r % 6);
      const std::string to = "S" + std::to_string((r + 1) % 6);
      m += std::string(r ? ", " : "") + "{\"from\": \"" + from + "\", \"to\": \"" + to + "\", \"timing\": \"nextBeat\"}";
    }
    m += "]}}\n";
    f["events/Music.json"] = m;
  }
  // Material table.
  {
    std::string t = "{\"type\": \"materialTable\", \"guid\": \"" + G(25) + "\", \"name\": \"Phys\", \"bank\": \"Main\", \"materials\": [";
    for (std::uint32_t i = 0; i < materials; ++i) t += std::string(i ? ", " : "") + "\"mat" + std::to_string(i) + "\"";
    t += "], \"pairs\": [{\"a\": \"mat0\", \"b\": \"mat1\", \"event\": \"" + G(8) + "\"}], \"defaults\": {\"impact\": \"" + G(8) + "\"}}\n";
    f["materials/Phys.json"] = t;
  }
  // Patch at the node limit (32): 31 oscillators and the output.
  {
    std::string p = "{\"type\": \"patch\", \"guid\": \"" + G(26) + "\", \"name\": \"Patch\", \"nodes\": [";
    for (int i = 0; i < 31; ++i) {
      p += "{\"id\": \"osc" + std::to_string(i) + "\", \"kind\": \"oscillator\", \"position\": [" + std::to_string(i * 37 % 900) +
           ", " + std::to_string(i * 53 % 700) + "]}, ";
    }
    p += "{\"id\": \"out\", \"kind\": \"output\", \"position\": [1000, 300]}], \"connections\": [{\"from\": \"osc0\", "
         "\"to\": \"out\"}], \"output\": \"out\"}\n";
    f["patches/Patch.json"] = p;
  }
  return f;
}

// Duplicated ids in the declared tree ("" if none).
inline std::string FirstDuplicate(const tela::Runtime& rt) {
  std::set<std::string> seen;
  for (const tela::PlacedElement& p : rt.elements()) {
    if (!seen.insert(p.element.id).second) return p.element.id;
  }
  return std::string();
}

// Every canvas of the tree within Tela's drawing limits and every bound finite.
inline std::string LimitProblem(const tela::Runtime& rt) {
  if (rt.elements().size() > 4096) return "more than 4096 elements";
  for (const tela::PlacedElement& p : rt.elements()) {
    const tela::Rect& b = p.bounds;
    if (!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.width) || !std::isfinite(b.height))
      return "non-finite bounds: " + p.element.id;
    const tela::Drawing& d = p.element.drawing;
    if (d.shapes().size() > 256) return "more than 256 shapes: " + p.element.id;
    std::size_t points = 0;
    for (const auto& s : d.shapes()) points += s.points.size();
    if (points > 4096) return "more than 4096 points: " + p.element.id;
  }
  return std::string();
}

// Writes the declared tree (id, label, bounds) to stdout when SND_INDEP_DUMP is set.
inline void Dump(const tela::Runtime& rt, const char* title) {
  if (std::getenv("SND_INDEP_DUMP") == nullptr) return;
  std::printf("=== %s (%u)\n", title, static_cast<unsigned>(rt.elements().size()));
  for (const tela::PlacedElement& p : rt.elements()) {
    std::printf("%s | %s | %.1f %.1f %.1f %.1f\n", p.element.id.c_str(), p.element.label.c_str(), p.bounds.x,
                p.bounds.y, p.bounds.width, p.bounds.height);
  }
}

}  // namespace iu

#endif  // SND_TOOL_TEST_INDEP_S7_UI_FIXTURE_HPP
