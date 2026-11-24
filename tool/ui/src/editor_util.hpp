// Helpers shared by the editor screens (inside tool/ui/src only): candidate lists of document
// objects, display names of references, new names, the "no document" text.
#ifndef SND_TOOL_UI_SRC_EDITOR_UTIL_HPP
#define SND_TOOL_UI_SRC_EDITOR_UTIL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/document.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/kit.hpp"

namespace snd::tool::ui {

// Objects of `kind` as candidates (label = name, value = GUID), name order; `none` (if not
// empty) first with the value "".
inline std::vector<PickerItem> DocItems(const ProjectDocument& doc, const char* kind, const std::string& none = {}) {
  std::vector<PickerItem> v;
  if (!none.empty()) v.push_back(PickerItem{none, std::string()});
  for (const DocObject* o : doc.OfKind(kind)) v.push_back(PickerItem{o->name(), o->guid});
  return v;
}

// The name of the object a reference (GUID or name) resolves to; the text as written when it
// resolves to nothing ("?" appended), `none` when empty.
inline std::string RefName(const ProjectDocument& doc, const char* kind, const std::string& ref,
                           const std::string& none = "-") {
  if (ref.empty()) return none;
  const DocObject* o = doc.Resolve(kind, ref);
  return o != nullptr ? o->name() : ref + " ?";
}

// GUID of a reference ("" if it resolves to nothing).
inline std::string RefGuid(const ProjectDocument& doc, const char* kind, const std::string& ref) {
  const DocObject* o = ref.empty() ? nullptr : doc.Resolve(kind, ref);
  return o != nullptr ? o->guid : std::string();
}

// "<base> <n>" with the first n >= 1 that no object of `kind` uses.
inline std::string NewName(const ProjectDocument& doc, const char* kind, const std::string& base) {
  for (std::uint32_t n = 1; n < 100000u; ++n) {
    const std::string name = base + " " + std::to_string(n);
    if (doc.FindByName(kind, name) == nullptr) return name;
  }
  return base;
}

// The first bank's GUID ("" if the project has none).
inline std::string FirstBank(const ProjectDocument& doc) {
  const std::vector<const DocObject*> b = doc.OfKind("bank");
  return b.empty() ? std::string() : b.front()->guid;
}

// Keeps a selection valid: `guid` if it names an object of `kind`, else the first (or "").
inline void KeepSelection(const ProjectDocument& doc, const char* kind, std::string* guid) {
  const DocObject* o = guid->empty() ? nullptr : doc.Find(*guid);
  if (o != nullptr && o->kind() == kind) return;
  const std::vector<const DocObject*> all = doc.OfKind(kind);
  *guid = all.empty() ? std::string() : all.front()->guid;
}

// The screen's text when there is no document.
inline bool NeedDocument(Kit& kit, const char* id, float width) {
  if (kit.app().document() != nullptr) return true;
  kit.Label(id, "no project document: start the tool with --project <dir> to edit", 0, 0, width, 22, 0.65f);
  return false;
}

// A list of objects of `kind` with selection, paging and New / Remove (one column).
// Returns the y below it.
struct ObjectListSpec {
  std::string id;          // element id prefix
  const char* kind = "";
  std::string title;
  std::string* selected = nullptr;
  std::uint32_t* page = nullptr;
  std::function<std::string()> create;          // returns the new GUID ("" refused)
  std::function<bool(const std::string&)> remove;  // null: ProjectDocument::Remove
  std::function<bool(const DocObject&)> filter;    // null: every object of the kind
  std::function<std::string(const DocObject&)> label;  // null: the name
  bool manage = true;      // New / Remove / Copy / Name (false: a selection list only)
};
float ObjectList(Kit& kit, const ObjectListSpec& spec, float x, float y, float w, float h);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_SRC_EDITOR_UTIL_HPP
