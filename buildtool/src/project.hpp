// Project model (spec/interface/project-format.md): one JSON object per file,
// loaded into a flat list with GUID / name indexes and typed accessors that
// report "file:line:col: reason" diagnostics.
#ifndef SND_BUILDTOOL_SRC_PROJECT_HPP
#define SND_BUILDTOOL_SRC_PROJECT_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "snd/buildtool/diagnostics.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/buildtool/source.hpp"
#include "snd/id.hpp"

namespace snd::buildtool::detail {

enum class Kind : std::uint8_t {
  Project,
  Bank,
  Bus,
  Category,
  Parameter,
  Snapshot,
  VoiceLimitGroup,
  Ducking,
  Asset,
  Event,
  Segment,
  Playlist,
  MaterialTable,
  Patch,  // waveform synthesis patch (design.md 5.1; project-format.md "patch")
  Count,
};
const char* KindName(Kind k);  // JSON "type" value
bool ParseKind(const std::string& s, Kind* out);

struct Object {
  Kind kind = Kind::Project;
  std::string file;  // project-relative path
  std::string guidText;
  std::uint64_t guidLo = 0;
  std::uint64_t guidHi = 0;
  std::string name;
  Id id = 0;
  json::Value doc;
  std::string bank;  // explicit "bank" (name, resolved), "" = shared
};

class Project {
 public:
  // Loads every *.json under `root` (sorted), skipping `skipDirs` (absolute).
  bool Load(const std::string& root, const std::vector<std::string>& skipDirs, Diagnostics& diag);
  // Loads the objects of an in-memory project (source.hpp); `name` is root().
  bool Load(const ProjectSource& source, const std::string& name, Diagnostics& diag);
  // A project-relative file (WAV, blob): from the source, or from the directory.
  bool ReadProjectFile(const std::string& rel, std::vector<std::uint8_t>* out) const;

  const std::string& root() const { return root_; }
  const std::vector<Object>& objects() const { return objects_; }
  const Object* project() const { return project_; }
  // Objects of a kind, sorted by name.
  std::vector<const Object*> OfKind(Kind k) const;

  // Resolves a reference value (GUID or name of `kind`). Reports an error at
  // the value's position and returns nullptr if it cannot be resolved.
  const Object* Resolve(const Object& from, const json::Value& ref, Kind kind, Diagnostics& diag) const;
  const Object* FindByName(Kind kind, const std::string& name) const;

  // Typed member accessors (report type errors; absent -> default).
  double Num(const Object& o, const json::Value& parent, const char* key, double def, Diagnostics& d) const;
  std::uint64_t U64(const Object& o, const json::Value& parent, const char* key, std::uint64_t def,
                    Diagnostics& d) const;
  bool Bool(const Object& o, const json::Value& parent, const char* key, bool def, Diagnostics& d) const;
  std::string Str(const Object& o, const json::Value& parent, const char* key, const std::string& def,
                  Diagnostics& d) const;
  // Enumerated string -> index into `names` (nullptr-terminated); absent -> def.
  std::uint32_t Enum(const Object& o, const json::Value& parent, const char* key, const char* const* names,
                     std::uint32_t def, Diagnostics& d) const;
  void ErrorAt(const Object& o, const json::Value& v, const std::string& msg, Diagnostics& d) const;
  void WarnAt(const Object& o, const json::Value& v, const std::string& msg, Diagnostics& d) const;

 private:
  bool LoadFiles(const std::vector<std::string>& files, Diagnostics& diag);
  std::string root_;
  const ProjectSource* source_ = nullptr;
  std::vector<Object> objects_;
  const Object* project_ = nullptr;
  std::map<std::string, std::size_t> byGuid_;
  std::map<std::pair<int, std::string>, std::size_t> byName_;
};

bool ParseGuid(const std::string& text, std::uint64_t* lo, std::uint64_t* hi);
bool LooksLikeGuid(const std::string& text);
std::string Hex32(std::uint32_t v);
std::vector<std::string> SchemaKindList();
std::vector<std::string> SchemaMemberList(const std::string& kind);

}  // namespace snd::buildtool::detail

#endif  // SND_BUILDTOOL_SRC_PROJECT_HPP
