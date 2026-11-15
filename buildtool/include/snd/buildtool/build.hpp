// Project -> bank build (design.md 10, spec/interface/project-format.md).
//
//   snd::buildtool::BuildOptions o;
//   o.projectDir = "game/audio";
//   o.outDir = "build/banks";
//   snd::buildtool::Diagnostics diag;
//   snd::buildtool::BuildResult r;
//   bool ok = snd::buildtool::Build(o, diag, &r);
//
// Output (outDir):
//   <bank>.sndb, <bank>.<language>.sndb   banks (language banks per project language)
//   streams/<asset>_<id>[.<lang>].snds     stream files (sector aligned)
//   ids.json                                ID table (name, hash, guid, kind, banks)
//   sonus_ids.h                             C++ header for game code (constants + static_asserts)
// Nothing is written when any error was reported. Files whose bytes did not
// change are not rewritten. The same inputs always give the same bytes.
#ifndef SND_BUILDTOOL_BUILD_HPP
#define SND_BUILDTOOL_BUILD_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/buildtool/diagnostics.hpp"
#include "snd/buildtool/plugins.hpp"
#include "snd/format/bank_format.hpp"

namespace snd::buildtool {

struct BuildOptions {
  std::string projectDir;
  std::string outDir;
  std::string cacheDir;            // differential build cache ("" = no cache)
  std::uint32_t platform = bank_format::kPlatformAny;
  bool devBuild = true;            // GUID chunk + kFileDevBuild (live link override layer)
  bool stripStrings = false;       // drop STRS / NAME (release)
  std::string idsHeader = "sonus_ids.h";  // relative to outDir ("" = none)
  const EncoderRegistry* encoders = nullptr;  // nullptr = built-ins only
  DspSizer dsp;                    // stateBytes == nullptr: provisional size + warning
};

struct BuiltBank {
  std::string name;       // bank name
  std::string language;   // "" for normal banks
  std::string path;       // file written (relative to outDir)
  std::uint32_t bankId = 0;
  std::uint64_t contentHash = 0;
  std::uint64_t bytes = 0;
  bool written = false;   // false: identical file already present
};

struct BuildResult {
  std::vector<BuiltBank> banks;
  std::vector<std::string> streams;  // stream files (relative to outDir)
  std::uint64_t projectBuildHash = 0;
  std::uint32_t assetsEncoded = 0;   // cache misses (encoder ran)
  std::uint32_t assetsFromCache = 0; // differential build hits
  std::uint32_t filesWritten = 0;
  std::uint32_t filesUnchanged = 0;
};

bool Build(const BuildOptions& options, Diagnostics& diag, BuildResult* result);

// "LNX_" / "WIN_" / ... / "ANY_" -> fourcc.
bool ParsePlatform(const std::string& name, std::uint32_t* fourcc);

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_BUILD_HPP
