// Connection screen (design.md 11.2): the connection target (typed), the matching on connect
// (protocol versions, build hashes, every bank's verdict), the question on a mismatch (send every
// difference / refuse), what a 1.0 game cannot do, the live reflection (each object's state,
// problems, what cannot go live), the hot reloads' progress, the development reserve in use with
// the structural edits (undo each) and the counters. The model is the session's LiveSync.
#include <algorithm>
#include <cstdio>

#include "editor_util.hpp"
#include "snd/tool/live_sync.hpp"
#include "snd/tool/names.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"
#include "snd/tool/ui/widgets.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kRow = 26.0f;

const char* VerdictName(BankVerdict v) {
  switch (v) {
    case BankVerdict::Same: return "same";
    case BankVerdict::Differs: return "differs";
    case BankVerdict::ToolOnly: return "only in the tool";
    case BankVerdict::GameOnly: return "only in the game";
  }
  return "?";
}
const char* ReloadPhaseText(livelink::ReloadPhase p) {
  switch (p) {
    case livelink::ReloadPhase::Unloading: return "unloading";
    case livelink::ReloadPhase::Loading: return "loading";
    case livelink::ReloadPhase::Done: return "done";
    case livelink::ReloadPhase::Failed: return "failed";
  }
  return "?";
}
const char* StructOpName(livelink::StructOp op) {
  switch (op) {
    case livelink::StructOp::AddBus: return "add bus";
    case livelink::StructOp::RemoveBus: return "remove bus";
    case livelink::StructOp::AddEffect: return "add effect";
    case livelink::StructOp::RemoveEffect: return "remove effect";
    case livelink::StructOp::AddSend: return "add send";
  }
  return "?";
}

std::string Hash(std::uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

void Target(Kit& kit, float x, float y, float w) {
  App* app = &kit.app();
  ConnectionDesc* desc = &app->connectionDesc();
  Session* s = &app->session();
  if (desc->connector != nullptr) {
    kit.Label("ln.target", "target: in-process (the preview engine)", x, y, w - 120, 22, 0.6f);
  } else {
    kit.NameField("ln.address", "Address", desc->address, [desc](const std::string& v) {
      if (v.empty()) return false;
      desc->address = v;
      return true;
    }, x, y, w * 0.5f, 60);
    NumberSpec port;
    port.value = desc->port;
    port.step = 1;
    port.minValue = 1;
    port.maxValue = 65535;
    port.integer = true;
    port.decimals = 0;
    port.document = false;
    kit.NumberField("ln.port", "Port", port, [desc](double v, std::uint32_t) {
      desc->port = static_cast<std::uint16_t>(v);
      return true;
    }, x + w * 0.5f + 4, y, w * 0.5f - 130, 36);
  }
  kit.Action("ln.connect", s->connection().IsOpen() ? "Disconnect" : "Connect", [app] { app->OnConnectToggle(); }, x + w - 116, y,
             116);
}

void Matching(Kit& kit, float w, float h) {
  App* app = &kit.app();
  Session* s = &app->session();
  LiveSync* live = s->live();
  float y = 0.0f;
  Target(kit, 0, y, w);
  y += kRow + 4.0f;
  const Connection& c = s->connection();
  char b[256];
  std::snprintf(b, sizeof b, "%s %s  live: %s", LinkStateName(c.state()), c.Target().c_str(),
                live != nullptr ? LivePhaseName(live->phase()) : "(no document)");
  kit.Label("ln.state", b, 0, y, w, 22, 0.6f);
  y += 24.0f;
  if (live == nullptr) {
    kit.Label("ln.nolive", "no live reflection: start the tool with --project (the mixer values still go live)", 0, y, w, 20, 0.5f);
    return;
  }
  // Banks whose hot reload failed (phase Failed, D-184): which and why, until a reload succeeds.
  const auto& failed = live->failedBanks();
  if (!failed.empty()) {
    std::size_t row = 0;
    for (const auto& [bank, why] : failed) {
      if (row >= 4u) break;
      kit.Label("ln.failed." + std::to_string(row++), "FAILED " + bank + ": " + why + " (detached in the game until a reload works)",
                0, y, w, 20, 0.55f);
      y += 20.0f;
    }
    if (failed.size() > 4u) {
      kit.Label("ln.failed.more", "+" + std::to_string(failed.size() - 4u) + " failed banks", 0, y, w, 18, 0.5f);
      y += 18.0f;
    }
  }
  const SyncCheck& chk = live->check();
  // D-189: without a connection there is no game to describe (no version, no 1.0 reasons).
  const bool connected = c.state() == LinkState::Online || c.state() == LinkState::Handshake;
  if (!connected) {
    kit.Label("ln.check", "not connected", 0, y, w, 20, 0.55f);
    y += 20.0f;
  } else if (chk.valid) {
    std::snprintf(b, sizeof b, "protocol %u.%u  tool build %s  game build %s%s", chk.peerMajor, chk.peerMinor,
                  Hash(chk.toolBuildHash).c_str(), Hash(chk.gameBuildHash).c_str(),
                  chk.toolBuildHash == chk.gameBuildHash ? "  (match)" : "  (mismatch)");
    kit.Label("ln.check", b, 0, y, w, 20, 0.55f);
    y += 20.0f;
    if (!chk.note.empty()) {
      kit.Label("ln.note", chk.note, 0, y, w, 20, 0.5f);
      y += 20.0f;
    }
  }
  if (live->pending()) {
    kit.Label("ln.ask", "the game's build differs from the project: send every difference, or refuse?", 0, y, w - 260, 22, 0.6f);
    kit.Action("ln.ask.send", "Send differences", [s] { s->live()->ChooseSendDiffs(s->connection()); }, w - 254, y, 130);
    kit.Action("ln.ask.refuse", "Refuse", [s] { s->live()->ChooseRefuse(s->connection()); }, w - 120, y, 120);
    y += kRow;
  }
  // Banks.
  kit.Label("ln.banks.head", "bank                verdict           tool content     game content     writable known", 0, y, w, 18,
            0.5f);
  y += 18.0f;
  for (std::size_t i = 0; i < chk.banks.size() && i < 8u; ++i) {
    const BankMatch& m = chk.banks[i];
    std::snprintf(b, sizeof b, "%-18s  %-16s  %s %s  %s  %s", m.file.empty() ? Hex32(m.bankId).c_str() : m.file.c_str(),
                  VerdictName(m.verdict), Hash(m.toolContentHash).c_str(), Hash(m.gameContentHash).c_str(),
                  m.writable ? "yes" : "no ", m.knownBase ? "yes" : "no");
    kit.Label("ln.bank." + std::to_string(i), b, 0, y, w, 18, 0.5f);
    y += 18.0f;
  }
  if (chk.banks.empty()) {
    kit.Label("ln.banks.none", chk.banksKnown ? "no banks" : "(the bank list comes on a mismatch from a 1.1 game)", 0, y, w, 18, 0.5f);
    y += 18.0f;
  }
  y += 6.0f;
  // Features of the game (1.0 games lack some); only for a connected game.
  const std::vector<LiveFeature> feats = connected ? live->features() : std::vector<LiveFeature>{};
  std::string fl = connected ? "features:" : "features: (not connected)";
  for (const LiveFeature& f : feats) fl += std::string(" ") + f.name + (f.available ? "" : " (no)");
  kit.Label("ln.features", Clip(fl, 160), 0, y, w, 18, 0.5f);
  y += 18.0f;
  for (std::size_t i = 0, row = 0; i < feats.size() && row < 3u; ++i) {
    if (feats[i].available || feats[i].reason.empty()) continue;
    kit.Label("ln.feature." + std::to_string(row++), std::string(feats[i].name) + ": " + feats[i].reason, 0, y, w, 18, 0.45f);
    y += 18.0f;
  }
  y += 6.0f;
  // Hot reloads.
  const auto& reloads = live->reloads();
  kit.Label("ln.reload.title", "hot reloads (" + std::to_string(reloads.size()) + ")", 0, y, w, 20, 0.6f);
  y += 22.0f;
  for (std::size_t r = 0; r < reloads.size() && r < 4u; ++r) {
    const ReloadProgress& p = reloads[reloads.size() - 1 - r];
    const float frac = p.finished ? 1.0f : p.phase == livelink::ReloadPhase::Loading ? 0.66f : 0.33f;
    std::snprintf(b, sizeof b, "%s  %s%s%s", p.bank.c_str(), ReloadPhaseText(p.phase), p.detached ? "  DETACHED in the game" : "",
                  p.reason.empty() ? "" : ("  " + p.reason).c_str());
    kit.Label("ln.reload." + std::to_string(r), Clip(b, 100), 0, y, w * 0.6f, 20, 0.5f);
    Canvas(kit.doc(), "ln.reload." + std::to_string(r) + ".bar", DrawUsageBar(w * 0.36f, 8, frac, kit.palette(), kit.ctx().dpi),
           w * 0.62f, y + 6, w * 0.36f, 8);
    y += 22.0f;
  }
  // Development reserve and the structural edits.
  const ReserveUsage ru = live->reserve();
  struct Bar {
    const char* name;
    double used, limit;
  };
  const Bar bars[] = {{"buses", static_cast<double>(ru.buses), static_cast<double>(ru.limits.buses)},
                      {"effects", static_cast<double>(ru.effects), static_cast<double>(ru.limits.effects)},
                      {"bytes", static_cast<double>(ru.bytes), static_cast<double>(ru.limits.bytes)}};
  kit.Label("ln.reserve.title", "development reserve in use", 0, y, w, 20, 0.6f);
  y += 22.0f;
  for (int i = 0; i < 3; ++i) {
    std::snprintf(b, sizeof b, "%s %.0f / %.0f", bars[i].name, bars[i].used, bars[i].limit);
    kit.Label("ln.reserve." + std::to_string(i), b, 0, y, w * 0.35f, 20, 0.5f);
    const float frac = bars[i].limit > 0.0 ? static_cast<float>(bars[i].used / bars[i].limit) : 0.0f;
    Canvas(kit.doc(), "ln.reserve." + std::to_string(i) + ".bar", DrawUsageBar(w * 0.6f, 8, frac, kit.palette(), kit.ctx().dpi),
           w * 0.38f, y + 6, w * 0.6f, 8);
    y += 20.0f;
  }
  const auto& se = live->structEdits();
  for (std::size_t i = 0; i < se.size() && y < h - 50.0f; ++i) {
    const StructEditInfo& e = se[i];
    std::snprintf(b, sizeof b, "%s %s target %s%s%s", e.bank.c_str(), StructOpName(e.op), Hex32(e.target).c_str(),
                  e.applied ? "" : "  (not applied)", e.undone ? "  undone" : "");
    kit.Label("ln.struct." + std::to_string(i), b, 0, y, w - 70, 20, 0.5f);
    if (e.applied && !e.undone) {
      kit.Action("ln.struct." + std::to_string(i) + ".undo", "Undo", [s, i] { s->live()->UndoStructEdit(i); }, w - 64, y, 64, 20);
    }
    y += 22.0f;
  }
  // Counters and the manual controls.
  const LiveSyncStats& st = live->stats();
  std::snprintf(b, sizeof b, "builds %llu  sent %llu  acked %llu  resent %llu  failed %llu  values %llu  chunks %llu  reloads %llu  "
                             "queued %u  in flight %u",
                static_cast<unsigned long long>(st.builds), static_cast<unsigned long long>(st.sent),
                static_cast<unsigned long long>(st.acked), static_cast<unsigned long long>(st.resent),
                static_cast<unsigned long long>(st.failed), static_cast<unsigned long long>(st.values),
                static_cast<unsigned long long>(st.chunks), static_cast<unsigned long long>(st.reloads), live->queued(),
                live->inFlight());
  kit.Label("ln.stats", b, 0, h - 46.0f, w, 20, 0.45f);
  kit.Action("ln.resync", "Resync", [s] { s->live()->Resync(); }, 0, h - 24.0f, 90);
  kit.Action("ln.flush", "Send now", [s] { s->live()->FlushNow(); }, 94, h - 24.0f, 90);
}

void Items(Kit& kit, float w, float h) {
  App* app = &kit.app();
  LiveSync* live = app->session().live();
  if (live == nullptr) return;
  const std::vector<LiveItem> items = live->items();
  LinkViewState* lv = &app->view().link;
  kit.Label("ln.items.title", std::to_string(items.size()) + " items (object, member, route, state)", 0, 0, w - 210, 20, 0.6f);
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - 30.0f) / 20.0f));
  const std::uint32_t first = kit.Pager("ln.items.pager", static_cast<std::uint32_t>(items.size()), per, &lv->page, w - 200, 0, 200);
  float y = 24.0f;
  char b[256];
  for (std::uint32_t i = first; i < items.size() && i < first + per; ++i) {
    const LiveItem& it = items[i];
    std::snprintf(b, sizeof b, "%-10s %-20s %-28s %-12s %-12s %s", it.kind.c_str(), it.name.c_str(), it.member.c_str(),
                  LiveRouteName(it.route), LiveStateName(it.state), it.reason.c_str());
    kit.Label("ln.item." + std::to_string(i - first), Clip(b, 160), 0, y, w - 50, 20, 0.5f);
    if (!it.guid.empty() && app->document() != nullptr && app->document()->Find(it.guid) != nullptr) {
      const std::string g = it.guid;
      kit.Action("ln.item." + std::to_string(i - first) + ".go", "Go", [app, g] { app->Navigate(g); }, w - 46, y, 46, 18);
    }
    y += 20.0f;
  }
}

}  // namespace

void DeclareLink(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  App* app = ctx.app;
  if (app == nullptr) return;
  LinkViewState* lv = &app->view().link;
  kit.Tabs("ln.tab", {"Matching", "Items", "Problems"}, lv->tab, [lv, app](std::uint32_t i) {
    if (i == 2u) {
      app->OnScreen(Screen::Problems);  // one list of problems (build, live, not live)
      return;
    }
    lv->tab = i;
    lv->page = 0;
  }, 0, 0, 100);
  const float top = 30.0f;
  Panel(doc, "ln.body", 0, top, width, height - top, [&] {
    if (lv->tab == 1u) Items(kit, width, height - top);
    else Matching(kit, width, height - top);
  });
}

}  // namespace snd::tool::ui
