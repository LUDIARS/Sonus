// Project view of the authoring tool (design.md 10.1, 11.1; spec/interface/project-format.md).
//
// Reads a project directory (one JSON object per file) and keeps what the
// mixer and the profiler need: the bus tree with each bus's volume and effect
// parameters, the categories and the parameters, each with its name, runtime
// id (snd::HashId(name)) and GUID. Effect parameters are described with the
// dsp module's table (name, unit, range, default); parameters the project
// does not write take the dsp default (as the build does).
//
// The project build hash (Hello.projectBuildHash, 11.2) is the build tool's:
// Build() runs snd::buildtool::Build into an output directory and keeps the
// hash and the bank files it wrote.
//
// Not thread-safe; the UI thread owns it.
#ifndef SND_TOOL_PROJECT_HPP
#define SND_TOOL_PROJECT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/id.hpp"

namespace snd::tool {

struct Guid {
  std::uint64_t lo = 0;  // last 16 hex digits (bank GUID chunk guidLo)
  std::uint64_t hi = 0;  // first 16 hex digits
  bool operator==(const Guid&) const = default;
};
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", braces allowed. false if malformed.
bool ParseGuid(const std::string& text, Guid* out);

struct EffectParamDesc {
  std::string name;   // dsp name, or "P<n>" for an unknown effect type
  std::string unit;   // "dB", "Hz", ...
  float minValue = 0.0f;
  float maxValue = 1.0f;
  float dspDefault = 0.0f;    // dsp table default
  float projectValue = 0.0f;  // the bank's value (project, or the dsp default when absent)
  bool integer = false;       // dsp kInt
  bool initOnly = false;      // dsp kInit: only read at instance creation (not live-editable)
  bool logScale = false;      // knob maps logarithmically (Hz with a positive minimum)
};

struct EffectDesc {
  std::string type;   // as written in the project ("snd.fx.delay")
  Id typeId = 0;      // HashId(type)
  bool known = false; // a standard dsp effect
  std::vector<EffectParamDesc> params;
};

struct BusDesc {
  // Stable unique key of the object in this project (element ids, override keys): the
  // runtime id in hex when no other object of the kind has the same id (always, in a
  // project that builds); otherwise "g" + the GUID (32 hex), or "n" + the position when
  // the GUID is shared too.
  std::string key;
  std::string name;
  Id id = 0;
  Guid guid;
  std::string file;           // project-relative path
  std::int32_t parent = -1;   // index in Project::buses(), -1 for the master
  std::uint32_t depth = 0;    // 0 for the master
  bool master = false;
  float volumeDb = 0.0f;
  std::uint32_t channels = 0; // 0 = parent's
  std::vector<EffectDesc> effects;
};

struct CategoryDesc {
  // Stable unique key of the object in this project (element ids, override keys): the
  // runtime id in hex when no other object of the kind has the same id (always, in a
  // project that builds); otherwise "g" + the GUID (32 hex), or "n" + the position when
  // the GUID is shared too.
  std::string key;
  std::string name;
  Id id = 0;
  Guid guid;
  std::int32_t parent = -1;
  std::uint32_t depth = 0;
  float volumeDb = 0.0f;
};

struct ParameterDesc {
  // Stable unique key of the object in this project (element ids, override keys): the
  // runtime id in hex when no other object of the kind has the same id (always, in a
  // project that builds); otherwise "g" + the GUID (32 hex), or "n" + the position when
  // the GUID is shared too.
  std::string key;
  std::string name;
  Id id = 0;
  Guid guid;
  bool isSwitch = false;
  bool global = true;  // "global" in its scope
  float minValue = 0.0f;
  float maxValue = 1.0f;
  float defaultValue = 0.0f;
};

struct ProjectIssue {
  bool error = true;
  std::string where;    // "file:line:col" or "" (project wide)
  std::string message;
};

class Project {
 public:
  // Reads every *.json under `dir` (recursive, name order, skipping directories that
  // start with '.' and `skipDir` if given). false if the directory cannot be read or any
  // error was found (the lists are still filled with what could be read).
  bool Load(const std::string& dir, const std::string& skipDir = std::string());
  // Runs the build tool (dev build) into `outDir`. Sets buildHash() and banks(). false
  // with issues() on a build error.
  bool Build(const std::string& outDir);

  const std::string& directory() const { return dir_; }
  const std::string& name() const { return name_; }
  // Buses in tree order: the master first, then depth first, children by name.
  const std::vector<BusDesc>& buses() const { return buses_; }
  const std::vector<CategoryDesc>& categories() const { return categories_; }  // tree order
  const std::vector<ParameterDesc>& parameters() const { return parameters_; }  // name order
  const std::vector<ProjectIssue>& issues() const { return issues_; }
  std::uint32_t errorCount() const;

  std::int32_t FindBus(Id id) const;
  std::int32_t FindCategory(Id id) const;
  std::int32_t FindParameter(Id id) const;
  // true if `bus` is `ancestor` or below it.
  bool IsBusUnder(std::int32_t bus, std::int32_t ancestor) const;

  std::uint64_t buildHash() const { return buildHash_; }
  void SetBuildHash(std::uint64_t h) { buildHash_ = h; }  // e.g. read from a build elsewhere
  struct BankFile {
    std::string name;
    std::string path;  // absolute (outDir/<file>)
    Id bankId = 0;
  };
  const std::vector<BankFile>& banks() const { return banks_; }

 private:
  std::string dir_;
  std::string name_;
  std::vector<BusDesc> buses_;
  std::vector<CategoryDesc> categories_;
  std::vector<ParameterDesc> parameters_;
  std::vector<ProjectIssue> issues_;
  std::vector<BankFile> banks_;
  std::uint64_t buildHash_ = 0;
};

}  // namespace snd::tool

#endif  // SND_TOOL_PROJECT_HPP
