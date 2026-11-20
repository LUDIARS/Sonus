// The editable project of the authoring tool (design.md 10.1, 11.1;
// spec/interface/project-format.md). Every editor of the tool (Event, mixer,
// parameters and curves, 3D, music, material tables, patches) edits one
// ProjectDocument; the Tela screens (tool/ui) and other hosts (Ergo Studio's sonus
// plugin) use the same model (Sonus::tool_core, no UI dependency).
//
// Contents: one JSON object per project file, as read (unknown members and "notes" at
// every level stay; nothing is dropped or reordered). Objects are identified by their
// GUID (lower case, no braces).
//
// Saving (Save / PendingChanges):
//   - A file whose object did not change is not written (byte for byte as on disk).
//   - A changed object is written in the fixed form of json::Write (2-space indent,
//     members in stored order, '\n' at the end). Members keep their order; a member the
//     tool adds is inserted at its place in the spec's member order (project-format.md
//     tables) after the members that come before it. Numbers that were not edited keep
//     their literal; edited numbers are written with the shortest text that reads back to
//     the same double (FormatNumber).
//   - Load -> Save -> Load gives the same objects; Load -> edit -> undo -> Save writes
//     nothing.
//   - New objects: "<folder>/<name>.json" (FileNameFor): the folder per kind (KindFolder:
//     "events", "buses", ...), '/' in the name makes sub-folders, characters that are not
//     portable in file names become '_', a name part starting with '.' gets a '_' prefix
//     (the loader skips hidden directories), a Windows reserved device name (CON, PRN, AUX, NUL,
//     COM1..9, LPT1..9, any case, the part before the first '.') gets a '_' prefix (D-176f),
//     "_2", "_3", ... on a clash with any other file (case-insensitive for ASCII letters only). Rename moves the file to the new name's path only when
//     the file is at the old name's path (RenameOptions::moveFile), else it stays. Remove
//     deletes the file on Save.
//
// References (project-format.md §1): the tool writes GUIDs. ReferencesTo lists the members
// that point at an object (by GUID or by name), BrokenReferences the ones that resolve to
// nothing. Rename rewrites references by the old name to the GUID (the build resolves
// names, not aliases) and can keep the old name in "aliases" (10.1). Remove refuses an
// object that is still referenced unless forced.
//
// Undo / redo (D-170 candidate): every edit of the document is undoable, across editors,
// in one history. An edit records the objects it touched (before / after). Calls between
// BeginGroup / EndGroup are one step (a drag); outside a group an edit with a non-zero
// merge key merges into the previous step of the same key (wheel notches); a step that
// ends where it started is dropped. The mixer's live values (MixerModel, the override
// layer of 11.2) keep their own history: they are not document edits until written back
// (WriteBackMixer in mixer_editor.hpp, one document step).
//
// Not thread-safe; the UI thread owns it.
#ifndef SND_TOOL_DOCUMENT_HPP
#define SND_TOOL_DOCUMENT_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "snd/buildtool/json.hpp"
#include "snd/id.hpp"

namespace snd::tool {

namespace json = ::snd::buildtool::json;

// --- JSON helpers -----------------------------------------------------------------------
// Structural equality (positions ignored; numbers by literal text).
bool JsonEqual(const json::Value& a, const json::Value& b);
// Like JsonEqual, but numbers compare by value ("2" = "2.0"; -0 and 0 differ).
bool JsonSameMeaning(const json::Value& a, const json::Value& b);
// Shortest decimal text that reads back as `v` (integers without a fraction). Non-finite
// values are written as 0.
std::string FormatNumber(double v);
json::Value Number(double v);
json::Value String(std::string s);
json::Value Bool(bool b);
// Path of a member: "tracks[0].clips[2].startMs", "music.transitions[1]", "" = the object.
json::Value* FindPath(json::Value& root, std::string_view path);
const json::Value* FindPath(const json::Value& root, std::string_view path);
// Numeric / string / bool member at a path (absent or another type: `def`).
double NumberAt(const json::Value& root, std::string_view path, double def);
std::string StringAt(const json::Value& root, std::string_view path, const std::string& def = std::string());
bool BoolAt(const json::Value& root, std::string_view path, bool def);
// The member order of project-format.md for the object at `path` of a `kind` object
// (nullptr-terminated; nullptr if the tool has no order for it: new members go last).
const char* const* SpecMemberOrder(std::string_view kind, std::string_view path);
// Sets object member `key` (inserted at its spec place when new).
void SetMember(json::Value& object, const std::string& key, json::Value value, const char* const* order);

// --- objects ----------------------------------------------------------------------------
// Kinds of project-format.md §2 (plus "patch"), in that order.
const std::vector<std::string>& ObjectKinds();
bool IsObjectKind(std::string_view kind);
const char* KindFolder(std::string_view kind);  // "events", ... ("" for "project")

struct DocObject {
  std::string file;  // project-relative path ('/' separators)
  json::Value doc;
  std::string guid;  // lower case, no braces
  std::string kind() const;
  std::string name() const;
};

struct DocLoadIssue {
  std::string file;
  std::string message;
};

// A member that names another object.
struct Reference {
  std::string fromGuid;
  std::string path;        // member path in the referring object
  std::string targetKind;  // kind it must name
  std::string text;        // the reference as written
  std::string targetGuid;  // resolved target ("" = broken)
};

struct FileChange {
  enum class Op : std::uint8_t { Write, Delete } op = Op::Write;
  std::string file;
  std::string text;  // Write: the new content
};

// Result of Save / SaveTo (D-176d, e).
struct SaveFailure {
  std::string path;    // project-relative file
  std::string reason;
};
struct SaveResult {
  bool ok = false;
  std::vector<SaveFailure> failures;   // writes, copies or deletions that did not happen
  std::vector<std::string> outside;    // SaveTo: referenced files outside the project (not copied)
  std::vector<std::string> missing;    // SaveTo: referenced files that could not be read (not copied)
};

struct RenameOptions {
  bool keepAlias = true;   // old name into "aliases" (10.1)
  bool moveFile = true;    // follow the name with the file (only if at the old name's path)
};

class ProjectDocument {
 public:
  using RandomFn = std::function<std::uint64_t()>;
  ProjectDocument();

  // --- loading ------------------------------------------------------------------------
  // Reads every *.json under `dir` (as the build does: recursive, name order, skipping
  // directories that start with '.'). Files that cannot be read as an object are kept out
  // (loadIssues) and are never written. false if the directory cannot be read.
  bool Load(const std::string& dir);
  // An in-memory project (file -> text). Binary files (WAV, blobs) are read from `dir`
  // ("" = none) unless given with SetBinaryFile. Save writes into `dir`.
  bool LoadFiles(const std::map<std::string, std::string>& files, const std::string& dir = std::string());
  // A binary project file held in memory (tests, generated material). Reads prefer these.
  void SetBinaryFile(const std::string& path, std::vector<std::uint8_t> bytes);
  bool ReadBinaryFile(const std::string& path, std::vector<std::uint8_t>* out) const;
  const std::string& directory() const { return dir_; }
  const std::vector<DocLoadIssue>& loadIssues() const { return loadIssues_; }
  // *.json files that could not be read as an object (left as they are, never written).
  const std::vector<std::string>& unreadFiles() const { return otherFiles_; }

  // --- reading -------------------------------------------------------------------------
  const std::vector<DocObject>& objects() const { return objects_; }
  const DocObject* Find(std::string_view guid) const;  // GUID (any case, braces allowed)
  const DocObject* FindByName(std::string_view kind, std::string_view name) const;
  // A reference as the build resolves it: a GUID of an object of `kind`, else a name of `kind`.
  const DocObject* Resolve(std::string_view kind, std::string_view ref) const;
  std::vector<const DocObject*> OfKind(std::string_view kind) const;  // name order
  const DocObject* projectObject() const;
  // The text the object would be saved as (json::Write), or the file as read if unchanged.
  std::string TextOf(const DocObject& o) const;
  bool IsUnchanged(const DocObject& o) const;  // same as on disk (or as loaded)

  // --- editing (each call is one undoable step unless grouped / merged) ------------------
  // A new object; returns its GUID ("" if the kind is unknown, the name is empty (except
  // ducking), has control characters or is taken). `init` members are copied in (spec order).
  std::string Create(std::string_view kind, const std::string& name, const json::Value* init = nullptr);
  // Copy of an object with a new GUID and name.
  std::string Duplicate(std::string_view guid, const std::string& newName);
  bool Rename(std::string_view guid, const std::string& newName, RenameOptions options = {});
  // Removes the object (its file is deleted on Save). Refused while referenced, unless `force`.
  bool Remove(std::string_view guid, bool force = false);
  // Sets the value at `path` (members are created in spec order; array elements must exist,
  // or index == size appends). A value equal to the current one: true, no step, not dirty
  // (D-177b; compared by meaning: numbers by value, so the text stays as written; in the editors'
  // setters an explicit default equals an absent member, and a move to the same place is success).
  // false only for a refused edit: no object, bad path, a non-finite number (Number()
  // of NaN / Inf, D-176c), or a rule of the object. The same holds for every editor's setters.
  bool Set(std::string_view guid, std::string_view path, json::Value value, std::uint32_t mergeKey = 0);
  bool SetNumber(std::string_view guid, std::string_view path, double v, std::uint32_t mergeKey = 0) {
    return Set(guid, path, Number(v), mergeKey);
  }
  // Set for a member with a default: when the current value (or `def` when the member is absent)
  // means the same as `value`, true and nothing is written (D-177b: an explicit default and an
  // absent member are the same value). The editors' setters go through it.
  bool SetDefaulted(std::string_view guid, std::string_view path, json::Value value, const json::Value& def,
                    std::uint32_t mergeKey = 0);
  // Removes the member / array element at `path`. false if absent.
  bool Erase(std::string_view guid, std::string_view path);
  // Array operations at `arrayPath` (the array is created when absent and index is 0).
  bool Insert(std::string_view guid, std::string_view arrayPath, std::size_t index, json::Value value);
  bool MoveElement(std::string_view guid, std::string_view arrayPath, std::size_t from, std::size_t to);
  // General edit: `fn` changes the object's JSON in place and returns false to cancel.
  // The GUID and "type" must not change (refused).
  bool Modify(std::string_view guid, const std::function<bool(json::Value&)>& fn, std::uint32_t mergeKey = 0);

  void BeginGroup();
  void EndGroup();
  bool InGroup() const { return groupDepth_ != 0u; }
  bool Undo();
  bool Redo();
  bool CanUndo() const { return !done_.empty(); }
  bool CanRedo() const { return !undone_.empty(); }
  std::size_t UndoCount() const { return done_.size(); }
  std::size_t RedoCount() const { return undone_.size(); }
  void ClearHistory();
  // Bumped by every change (edits, undo, redo, load).
  std::uint64_t revision() const { return revision_; }
  // GUIDs of the objects the last step (edit, undo or redo) changed.
  const std::vector<std::string>& lastChanged() const { return lastChanged_; }

  // --- references ------------------------------------------------------------------------
  std::vector<Reference> ReferencesOf(std::string_view guid) const;  // what the object names
  std::vector<Reference> ReferencesTo(std::string_view guid) const;  // who names it
  std::vector<Reference> BrokenReferences() const;

  // --- GUIDs -----------------------------------------------------------------------------
  // The generator new GUIDs come from (version 4 layout). Tests replace it. NewGuid gives up
  // after 16 values that are taken ("", D-177c; Create then fails and changes nothing).
  void SetRandom(RandomFn fn) { random_ = std::move(fn); }
  std::string NewGuid();

  // --- saving ----------------------------------------------------------------------------
  std::vector<FileChange> PendingChanges() const;
  bool IsDirty() const { return !PendingChanges().empty(); }
  // Applies PendingChanges to directory() (D-176d): new and changed files are written to a
  // temporary file and renamed over; then, only if every write succeeded, files no object uses
  // any more are deleted (a moved object's old file goes after its new file is written). Any
  // failure: false, `error` / lastSave() name the paths, and what did not happen stays pending
  // (the document stays dirty; the next Save tries the failed deletions again).
  bool Save(std::string* error = nullptr);
  // "Save as" (D-176e): writes every object to `dir`, copies the files the project reads from
  // inside its directory (asset files and language files, effect blobs, and the *.json files
  // that could not be read) to the same relative paths, and from then on works in `dir`. Files
  // outside the project directory are not copied (lastSave().outside); a referenced file that
  // cannot be read is listed in lastSave().missing. A failed write or copy: false, directory()
  // unchanged. `dir` that is the project's directory spelled otherwise (std::filesystem::equivalent,
  // or the normalised path when it does not exist) is a Save. Refused (false, the reason in
  // lastSave(), nothing written, the document and directory() unchanged): a destination inside
  // the project directory or containing it, and a destination that already holds *.json files
  // this save does not write (they would mix into the project on the next Load).
  bool SaveTo(const std::string& dir, std::string* error = nullptr);
  const SaveResult& lastSave() const { return lastSave_; }
  // Project-relative files the objects name (asset "file" / "languages", effect "blob").
  std::vector<std::string> ReferencedFiles() const;
  // Marks the current state as saved without writing (an in-memory project).
  void MarkSaved();

  // File name a new object of `kind` named `name` would get (see the header comment).
  std::string FileNameFor(std::string_view kind, const std::string& name, std::string_view exceptGuid = {}) const;

 private:
  struct Snapshot {
    bool exists = false;
    DocObject object;
  };
  struct ObjChange {
    std::string guid;
    Snapshot before;
    Snapshot after;
  };
  struct Step {
    std::vector<ObjChange> changes;
    std::uint32_t mergeKey = 0;
  };
  // Transaction plumbing: Touch records the object's state before the first change of the
  // open operation; Close records the after states and files the step.
  void Open(std::uint32_t mergeKey);
  void Touch(const std::string& guid);
  void Close();
  void Cancel();
  void ApplySnapshot(const std::string& guid, const Snapshot& s);
  DocObject* FindMutable(std::string_view guid);
  bool NameTaken(std::string_view kind, const std::string& name, std::string_view exceptGuid) const;
  bool FileTaken(const std::string& file, std::string_view exceptGuid) const;
  void Reset();
  bool AddLoaded(const std::string& file, const std::string& text);

  std::string dir_;
  std::vector<DocObject> objects_;
  std::map<std::string, json::Value> saved_;     // file -> object as on disk
  std::map<std::string, std::string> savedText_;  // file -> text as on disk
  std::map<std::string, std::vector<std::uint8_t>> binary_;
  std::vector<std::string> otherFiles_;           // files on disk that are not objects (never touched)
  std::vector<DocLoadIssue> loadIssues_;
  SaveResult lastSave_;
  RandomFn random_;

  std::uint32_t groupDepth_ = 0;
  bool opOpen_ = false;
  Step open_;
  std::uint32_t openKey_ = 0;
  bool closedLast_ = true;
  std::vector<Step> done_;
  std::vector<Step> undone_;
  std::uint64_t revision_ = 0;
  std::vector<std::string> lastChanged_;
  std::uint32_t maxSteps_ = 1024;
};

// Normalised GUID text (lower case, no braces); "" if malformed.
std::string NormalizeGuid(std::string_view text);
bool LooksLikeGuid(std::string_view text);

}  // namespace snd::tool

#endif  // SND_TOOL_DOCUMENT_HPP
