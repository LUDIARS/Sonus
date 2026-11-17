// Header (screens, connection) and footer (last message).
#include <cstdio>

#include "snd/tool/names.hpp"
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

void DeclareHeader(tela::Document& doc, ViewContext& ctx, float width) {
  Actions* a = &ctx.actions;
  const Connection& conn = ctx.session.connection();
  const bool mixer = ctx.state.screen == Screen::Mixer;
  Button(doc, "tab.mixer", mixer ? "[Mixer]" : "Mixer", [a] { a->OnScreen(Screen::Mixer); }, 6, 6, 96, 28);
  Button(doc, "tab.profiler", mixer ? "Profiler" : "[Profiler]", [a] { a->OnScreen(Screen::Profiler); }, 108, 6, 96,
         28);
  const Project& p = ctx.session.project();
  Text(doc, "hdr.project", "project " + (p.name().empty() ? p.directory() : p.name()), 214, 8, 220, 24);
  // Connection: state light, text, connect / disconnect.
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
  Text(doc, "link.state", text, lx + 18, 8, 330, 24);
  Button(doc, "link.toggle", conn.IsOpen() ? "Disconnect" : "Connect", [a] { a->OnConnectToggle(); },
         width - 116, 6, 110, 28);
}

void DeclareFooter(tela::Document& doc, ViewContext& ctx, float width) {
  const auto& msgs = ctx.session.messages();
  const CellSync& sync = ctx.session.sync();
  char b[96];
  std::snprintf(b, sizeof b, "pending %u  no answer %u  unsent %u  refused %u", sync.Count(CellState::Pending),
                sync.NoAnswer(ctx.nowNs), sync.Count(CellState::Unsent), sync.Count(CellState::Failed));
  Text(doc, "footer.msg", msgs.empty() ? std::string() : msgs.back(), 6, 2, width - 340, 22, 0.7f);
  Text(doc, "footer.sync", b, width - 330, 2, 324, 22, 0.7f);
}

}  // namespace snd::tool::ui
