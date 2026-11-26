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
#include "snd/buildtool/source.hpp"
#include "snd/ext.h"
#include "snd/format/bank_format.hpp"

namespace snd::buildtool {

struct BuildOptions {
  std::string projectDir;
  std::string outDir;
  std::string cacheDir;            // differential build cache ("" = no cache)
  std::uint32_t platform = bank_format::kPlatformAny;
  bool devBuild = true;            // GUID chunk + kFileDevBuild (live link override layer)
  bool stripStrings = false;       // drop STRS / NAME (release)
  bool warningsAsErrors = false;   // any warning fails the build (nothing written)
  std::string idsHeader = "sonus_ids.h";  // relative to outDir ("" = none)
  const EncoderRegistry* encoders = nullptr;  // nullptr = PCMF + PC16 (sonus-build: CodecEncoders())
  // EFFX.stateBytes / DSPN. stateBytes == nullptr (default): the dsp module's
  // sizing (dsp::EffectInstanceBytes, as dsp::ComputeDspPoolNeed at runtime) for
  // dspSampleRate / dspMaxFrames, with pluginEffects for plugin types. Standard
  // effects are checked against dsp's table (too many parameters: error; a value
  // outside the range: warning). Unknown types: warning, stateBytes 0, not in DSPN.
  DspSizer dsp;
  std::uint32_t dspSampleRate = 48000;  // = Config.sampleRate of the runtime (default 48000)
  std::uint32_t dspMaxFrames = 256;     // = Config.blockSize (default 256)
  std::vector<snd_dsp_effect> pluginEffects;  // = Config.plugins.effects (sizing only)
  // In-memory project (source.hpp). Set: the objects and the files they name are read
  // from it, and projectDir is only used as the project's name in messages (may be "").
  const ProjectSource* source = nullptr;
  // Set: the outputs are appended here (in the order of the files written to disk,
  // banks first) and nothing is written; outDir may be "". Reading old outputs to skip
  // unchanged files does not happen (BuildResult::filesWritten counts every output).
  std::vector<OutputFile>* memoryOutput = nullptr;
  bool allowEditorOnlyPatches = false;  // preview: retain unrepresentable named XPCH graphs with a warning
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
