// The candidate list overlay (kit.hpp PickField) and the problems screen (errors and warnings of
// the build's check, live problems, what cannot go live).
#include <algorithm>
#include <cstdio>

#include "snd/tool/live_sync.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"
#include "editor_util.hpp"

namespace snd::tool::ui {

float ObjectList(Kit& kit, const ObjectListSpec& spec, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument& doc = *app->document();
  std::vector<const DocObject*> all;
  for (const DocObject* o : doc.OfKind(spec.kind)) {
    if (!spec.filter || spec.filter(*o)) all.push_back(o);
  }
  if (spec.selected != nullptr) {
    bool found = false;
    for (const DocObject* o : all) found = found || o->guid == *spec.selected;
    if (!found) *spec.selected = all.empty() ? std::string() : all.front()->guid;
  }
  kit.Title(spec.id + ".title", spec.title + " (" + std::to_string(all.size()) + ")", x, y, w);
  std::string* sel = spec.selected;
  if (spec.create && spec.manage) {
    auto create = spec.create;
    ProjectDocument* pd = &doc;
    kit.Action(spec.id + ".new", "New", [create, sel, app, pd] {
      pd->BeginGroup();  // the object and its initial members: one undo step
      const std::string g = create();
      pd->EndGroup();
      if (g.empty()) {
        app->Message("refused: new object");
      } else if (sel != nullptr) {
        *sel = g;
      }
    }, x, y + 24, 60);
  }
  if (spec.manage && sel != nullptr && !sel->empty()) {
    auto remove = spec.remove;
    ProjectDocument* d = &doc;
    kit.Action(spec.id + ".remove", "Remove", [remove, sel, app, d] {
      const std::string g = *sel;
      const bool ok = remove ? remove(g) : d->Remove(g);
      if (!ok) {
        app->Message("refused: still referenced by " + std::to_string(d->ReferencesTo(g).size()) + " members");
      }
    }, x + 64, y + 24, 70);
    const DocObject* o = doc.Find(*sel);
    if (o != nullptr) {
      const std::string g = o->guid;
      kit.Action(spec.id + ".dup", "Copy", [d, g, sel, app] {
        const DocObject* src = d->Find(g);
        if (src == nullptr) return;
        const std::string n = NewName(*d, src->kind().c_str(), src->name());
        const std::string copy = d->Duplicate(g, n);
        if (copy.empty()) app->Message("refused: copy");
        else *sel = copy;
      }, x + 138, y + 24, 60);
      kit.NameField(spec.id + ".name", "Name", o->name(), [d, g](const std::string& n) { return d->Rename(g, n); }, x,
                    y + 50, w, 50);
    }
  }
  const float rowsTop = y + (spec.manage ? 100.0f : 48.0f);
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - (rowsTop - y)) / 24.0f));
  const std::uint32_t first = kit.Pager(spec.id + ".pager", static_cast<std::uint32_t>(all.size()), per, spec.page, x,
                                        spec.manage ? y + 76 : y + 24, w);
  KeySet keys;
  for (std::size_t i = 0; i < first && i < all.size(); ++i) keys.Key(all[i]->guid, i);  // the same keys on every page
  for (std::uint32_t i = 0; i < per && first + i < all.size(); ++i) {
    const DocObject* o = all[first + i];
    const std::string key = keys.Key(o->guid, first + i);
    const std::string g = o->guid;
    const std::string label = spec.label ? spec.label(*o) : o->name();
    const bool on = sel != nullptr && *sel == g;
    Button(kit.doc(), spec.id + "." + key, (on ? "> " : "  ") + Clip(label, 40),
           [sel, g] {
             if (sel != nullptr) *sel = g;
           },
           x, rowsTop + static_cast<float>(i) * 24.0f, w, 22, 0.55f);
  }
  return rowsTop + static_cast<float>(std::min<std::size_t>(per, all.size())) * 24.0f;
}

void DeclarePicker(tela::Document& doc, ViewContext& ctx, float width, float height) {
  App* app = ctx.app;
  const Picker* p = app != nullptr ? app->picker() : nullptr;
  if (p == nullptr) return;
  // Everything outside the list closes it (an exclusive button under the list).
  Button(doc, "picker.scrim", std::string(), [app] { app->ClosePicker(); }, 0, 0, width, height);
  const float w = std::min(440.0f, width - 20.0f);
  const float h = std::min(560.0f, height - 40.0f);
  const float x = (width - w) * 0.5f, y = (height - h) * 0.5f;
  Panel(doc, "picker", x, y, w, h, [&] {
    Kit kit(doc, ctx);
    kit.Title("picker.title", p->title, 0, 0, w - 90);
    kit.Action("picker.cancel", "Cancel", [app] { app->ClosePicker(); }, w - 84, 0, 70);
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - 70.0f) / 24.0f));
    const std::uint32_t total = static_cast<std::uint32_t>(p->items.size());
    const std::uint32_t pages = std::max(1u, (total + per - 1u) / per);
    const std::uint32_t page = std::min(p->page, pages - 1u);
    if (pages > 1u) {
      Button(doc, "picker.prev", "<", [app] { app->PickerPage(-1); }, 0, 26, 24, 20, 0.6f);
      Button(doc, "picker.next", ">", [app] { app->PickerPage(1); }, 28, 26, 24, 20, 0.6f);
      kit.Label("picker.page", std::to_string(page + 1u) + " / " + std::to_string(pages), 56, 26, 120);
    }
    if (total == 0u) kit.Label("picker.empty", "(nothing to choose)", 0, 50, w - 12);
    for (std::uint32_t i = 0; i < per && page * per + i < total; ++i) {
      const PickerItem& it = p->items[page * per + i];
      const std::string value = it.value;
      Button(doc, "picker.item." + std::to_string(i), Clip(it.label, 60), [app, value] { app->Pick(value); }, 0,
             50.0f + static_cast<float>(i) * 24.0f, w - 12, 22, 0.55f);
    }
  }, 6.0f);
}

void DeclareProblems(tela::Document& doc, ViewContext& ctx, float width, float height) {
  App* app = ctx.app;
  Kit kit(doc, ctx);
  if (app == nullptr || app->document() == nullptr) {
    kit.Label("prob.none", "no project document (start with --project)", 0, 0, width);
    return;
  }
  struct Row {
    std::string text;
    std::string guid;
  };
  std::vector<Row> rows;
  std::uint32_t errors = 0, warnings = 0;
  for (const DocIssue& i : app->problems()) {
    (i.error ? errors : warnings) += 1u;
    rows.push_back(Row{(i.error ? "error   " : "warning ") + i.text + (i.path.empty() ? "" : "  [" + i.path + "]"),
                       i.guid});
  }
  const LiveSync* live = ctx.session.live();
  if (live != nullptr) {
    for (const LiveProblem& p : live->problems()) {
      rows.push_back(Row{"live    r" + std::to_string(p.revision) + " " + p.name + " " + p.member + ": " + p.message,
                         p.guid});
    }
    for (const UnsupportedItem& u : live->unsupported()) {
      rows.push_back(Row{std::string(u.blocking ? "not live " : "no effect ") + u.bank + ": " + u.reason, u.guid});
    }
  }
  char b[128];
  std::snprintf(b, sizeof b, "%u errors, %u warnings (build check)%s", errors, warnings,
                live != nullptr ? ", live problems and items that cannot go live below" : "");
  kit.Title("prob.title", b, 0, 0, width - 200);
  kit.Action("prob.refresh", "Check now", [app] { app->RefreshProblems(); }, width - 190, 0, 90);
  if (live != nullptr) {
    Session* s = &app->session();
    kit.Action("prob.clear", "Clear live", [s] { s->live()->ClearProblems(); }, width - 94, 0, 90);
  }
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (height - 60.0f) / 22.0f));
  ViewState& vs = app->view();
  const std::uint32_t first = kit.Pager("prob.pager", static_cast<std::uint32_t>(rows.size()), per, &vs.problemsPage, 0,
                                        26, 240);
  for (std::uint32_t i = 0; i < per && first + i < rows.size(); ++i) {
    const Row& r = rows[first + i];
    const float y = 50.0f + static_cast<float>(i) * 22.0f;
    const std::string id = "prob." + std::to_string(i);
    kit.Label(id, Clip(r.text, 200), 0, y, width - 80, 22, 0.5f);
    if (!r.guid.empty() && app->document()->Find(r.guid) != nullptr) {
      const std::string g = r.guid;
      kit.Action(id + ".go", "Go", [app, g] { app->Navigate(g); }, width - 70, y, 60, 20);
    }
  }
  if (rows.empty()) kit.Label("prob.ok", "no problems", 0, 50, width);
}

}  // namespace snd::tool::ui
