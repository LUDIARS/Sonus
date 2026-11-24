// Header (screens, document, connection) and footer (last message, problems, value sync).
#include <algorithm>
#include <cstdio>

#include "snd/tool/live_sync.hpp"
#include "snd/tool/names.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"
#include "snd/tool/ui/widgets.hpp"

namespace snd::tool::ui {

bool IsPressed(const tela::Runtime& runtime, const std::string& id) {
  const tela::ElementState* s = runtime.state(id);
  return s != nullptr && s->pressed;
}

std::string BusId(std::uint32_t id) { return "bus." + Hex32(id); }
std::string CategoryId(std::uint32_t id) { return "cat." + Hex32(id); }
std::string ParameterId(std::uint32_t id) { return "par." + Hex32(id); }

std::string ObjectKey(const std::string& guid) {
  std::string k;
  for (char c : guid) {
    if (c != '-' && c != '{' && c != '}') k += c;
  }
  return k;
}

std::string NameKey(const std::string& name) { return Hex32(HashId(name)); }

const char* ScreenName(Screen s) {
  switch (s) {
    case Screen::Mixer: return "mixer";
    case Screen::Profiler: return "profiler";
    case Screen::Buses: return "buses";
    case Screen::Event: return "event";
    case Screen::Params: return "params";
    case Screen::Spatial: return "spatial";
    case Screen::Music: return "music";
    case Screen::Material: return "material";
    case Screen::Patch: return "patch";
    case Screen::Replay: return "replay";
    case Screen::Link: return "link";
    case Screen::Problems: return "problems";
  }
  return "mixer";
}

const char* ScreenLabel(Screen s) {
  switch (s) {
    case Screen::Mixer: return "Mixer";
    case Screen::Profiler: return "Profiler";
    case Screen::Buses: return "Buses";
    case Screen::Event: return "Event";
    case Screen::Params: return "Params";
    case Screen::Spatial: return "3D";
    case Screen::Music: return "Music";
    case Screen::Material: return "Material";
    case Screen::Patch: return "Patch";
    case Screen::Replay: return "Replay";
    case Screen::Link: return "Link";
    case Screen::Problems: return "Problems";
  }
  return "";
}

bool ParseScreen(const std::string& name, Screen* out) {
  for (Screen s : kTabOrder) {
    if (name == ScreenName(s)) {
      *out = s;
      return true;
    }
  }
  return false;
}

void DeclareHeader(tela::Document& doc, ViewContext& ctx, float width) {
  Actions* a = &ctx.actions;
  App* app = ctx.app;
  const Connection& conn = ctx.session.connection();
  // The screens (left), the connection (right). The project, its history and saving are in the footer.
  const float tabW = std::min(72.0f, std::max(40.0f, (width - 12.0f - 480.0f) / static_cast<float>(kScreenCount) - 4.0f));
  for (std::uint32_t i = 0; i < kScreenCount; ++i) {
    const Screen s = kTabOrder[i];
    const std::string label = ScreenLabel(s);
    Button(doc, std::string("tab.") + ScreenName(s), ctx.state.screen == s ? "[" + label + "]" : label,
           [a, s] { a->OnScreen(s); }, 6.0f + static_cast<float>(i) * (tabW + 4.0f), 6, tabW, 28, 0.55f);
  }
  // Connection: state light, text (or the mismatch question), connect / disconnect.
  const LinkState st = conn.state();
  tela::Color on = ctx.palette.failed;
  if (st == LinkState::Online) on = conn.mismatch() ? ctx.palette.pending : ctx.palette.solo;
  if (st == LinkState::Connecting || st == LinkState::Handshake) on = ctx.palette.pending;
  const float lx = width - 470.0f;
  Canvas(doc, "link.light", DrawLight(12, 12, st != LinkState::Offline, on, ctx.palette, ctx.dpi), lx, 14, 12, 12);
  std::string text = std::string(LinkStateName(st)) + "  " + conn.Target();
  if (st == LinkState::Online) {
    char b[96];
    std::snprintf(b, sizeof b, "  %u Hz / %u  rtt %u us%s", conn.hello().sampleRate, conn.hello().blockSize,
                  conn.roundTripUs(), conn.mismatch() ? "  build mismatch" : "");
    text += b;
  }
  if (st == LinkState::Rejected) text += conn.mismatch() ? "  build mismatch refused" : "  protocol mismatch";
  const LiveSync* live = ctx.session.live();
  if (live != nullptr && live->pending() && app != nullptr) {
    // The question of a build mismatch (11.2); its details are on the Link screen.
    Text(doc, "link.state", "build differs: send the tool's data?", lx + 18, 8, 210, 24, 0.55f);
    Session* s = &app->session();
    Button(doc, "link.ask.send", "Send diffs", [s] { s->live()->ChooseSendDiffs(s->connection()); }, lx + 230, 6, 64,
           28, 0.5f);
    Button(doc, "link.ask.refuse", "Refuse", [s] { s->live()->ChooseRefuse(s->connection()); }, lx + 298, 6, 52, 28,
           0.5f);
  } else {
    if (live != nullptr) text += std::string("  live: ") + LivePhaseName(live->phase());
    Text(doc, "link.state", text, lx + 18, 8, 330, 24, 0.55f);
  }
  Button(doc, "link.toggle", conn.IsOpen() ? "Disconnect" : "Connect", [a] { a->OnConnectToggle(); }, width - 116, 6,
         110, 28);
}

void DeclareFooter(tela::Document& doc, ViewContext& ctx, float width) {
  const auto& msgs = ctx.session.messages();
  const CellSync& sync = ctx.session.sync();
  char b[96];
  std::snprintf(b, sizeof b, "pending %u  no answer %u  unsent %u  refused %u", sync.Count(CellState::Pending),
                sync.NoAnswer(ctx.nowNs), sync.Count(CellState::Unsent), sync.Count(CellState::Failed));
  App* app = ctx.app;
  const ProjectDocument* d = app != nullptr ? app->document() : nullptr;
  const float msgW = d != nullptr ? width - 842.0f : width - 440.0f;
  Text(doc, "footer.msg", msgs.empty() ? std::string() : msgs.back(), 6, 2, std::max(40.0f, msgW), 22, 0.7f);
  // The project (unsaved / build errors), the document's history and saving, the problems.
  const Project& p = ctx.session.project();
  std::string project = p.name().empty() ? p.directory() : p.name();
  if (p.errorCount() != 0u) project += " (" + std::to_string(p.errorCount()) + " errors)";  // build errors too
  if (d != nullptr) {
    if (app->dirty()) project += " * unsaved";
    Text(doc, "hdr.project", project, width - 830, 2, 180, 22, 0.55f);
    Button(doc, "hdr.undo", "Undo " + std::to_string(d->UndoCount()), [app] { app->DocUndo(); }, width - 644, 2, 68, 22,
           0.5f);
    Button(doc, "hdr.redo", "Redo " + std::to_string(d->RedoCount()), [app] { app->DocRedo(); }, width - 572, 2, 68, 22,
           0.5f);
    Button(doc, "hdr.save", app->dirty() ? "Save *" : "Save", [app] { app->SaveDocument(); }, width - 500, 2, 62, 22,
           0.5f);
    std::uint32_t errors = 0, warnings = 0;
    for (const DocIssue& i : app->problems()) (i.error ? errors : warnings) += 1u;
    Button(doc, "footer.problems", std::to_string(errors) + " err " + std::to_string(warnings) + " warn",
           [app] { app->OnScreen(Screen::Problems); }, width - 432, 2, 96, 22, 0.5f);
  } else {
    Text(doc, "hdr.project", project, width - 432, 2, 96, 22, 0.5f);
  }
  Text(doc, "footer.sync", b, width - 330, 2, 324, 22, 0.7f);
}

}  // namespace snd::tool::ui
