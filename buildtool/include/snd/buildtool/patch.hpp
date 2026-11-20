// Waveform synthesis patches (design.md 5, 5.1; project-format.md "patch").
//
// A patch is a small synthesis graph built in the tool: sources (oscillator, noise),
// processors (filter, amp, mix), modulators (5.1: adsr, lfo, random, parameterCurve,
// envelopeFollower) and one output. Every node has at most one output port (its kind
// says whether it carries audio or a control signal) and named inputs. The build
// checks the graph and converts it into an array of nodes in dependency order
// (5.1 "ビルド時にノードを依存順に並べた配列へ変換"), deterministic: Kahn's order with
// ties broken by the node's position in the project file.
//
// The node table below is the single definition the build tool (checks, conversion)
// and the authoring tool's patch editor share.
//
// Bank representation (PROVISIONAL, bank-format.md §6.6): the runtime's Patch source
// and the PTCH layout belong to session D and were not defined when this was written.
// Until then the programs go into the optional chunk 'XPCH' (unknown chunks are
// ignored by the runtime, D-32); the clip's SYNP row has kind Patch and
// programOffset / programBytes 0, and XPCH links SYNP rows to programs. Everything
// about the layout is in WritePatchChunk / ReadPatchChunk (patch.cpp), to be replaced
// once D's definition arrives (requests/C-10.md).
#ifndef SND_BUILDTOOL_PATCH_HPP
#define SND_BUILDTOOL_PATCH_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/buildtool/json.hpp"
#include "snd/id.hpp"

namespace snd::buildtool::patch {

inline constexpr std::uint32_t kMaxNodes = 32;         // node limit of one patch (design.md 5)
inline constexpr std::uint32_t kMaxInputs = 4;         // inputs of one node kind
inline constexpr std::uint32_t kMaxParams = 8;         // numeric parameters of one node kind
inline constexpr std::uint32_t kMaxCurvePoints = 8;    // parameterCurve points
inline constexpr std::uint32_t kChunkFourcc = 0x48435058u;  // 'XPCH' (little-endian 'X','P','C','H')
inline constexpr std::uint32_t kChunkVersion = 1;

enum class Signal : std::uint8_t { None = 0, Audio = 1, Control = 2 };

struct ParamInfo {
  const char* name;
  float minValue;
  float maxValue;
  float defaultValue;
  const char* const* choices;  // enumerated (string values -> index), nullptr: number
};

struct InputInfo {
  const char* name;
  Signal signal;
  bool required;
};

struct NodeKindInfo {
  const char* name;
  Signal output;  // None: the output node
  std::uint32_t inputCount;
  InputInfo inputs[kMaxInputs];
  std::uint32_t paramCount;
  ParamInfo params[kMaxParams];
  bool modulator;      // 5.1 modulator (evaluated per block)
  bool usesParameter;  // parameterCurve: "parameter" (reference) and "points"
};

// Every node kind, in a fixed order (the index is the kind's number in the bank).
const NodeKindInfo* NodeKinds(std::uint32_t* count);
const NodeKindInfo* FindNodeKind(const std::string& name, std::uint32_t* index = nullptr);
const InputInfo* FindInput(const NodeKindInfo& kind, const std::string& input, std::uint32_t* index = nullptr);
const ParamInfo* FindParam(const NodeKindInfo& kind, const std::string& param, std::uint32_t* index = nullptr);

// One problem of a patch graph. `path` names the member ("nodes[2].params.cutoffHz").
struct Problem {
  bool error = true;
  std::string path;
  std::string message;
};

// A checked, ordered patch.
struct CompiledNode {
  std::uint32_t kind = 0;                 // index into NodeKinds
  std::uint32_t source = 0;               // index of the node in the project file
  std::uint32_t inputs[kMaxInputs] = {};  // program node index per input, kNoInput if none
  float params[kMaxParams] = {};          // every parameter (defaults filled in)
  Id parameter = 0;                       // parameterCurve: parameter id
  std::uint32_t pointCount = 0;
  float points[kMaxCurvePoints][2] = {};  // parameterCurve: (x, y), x ascending
};
inline constexpr std::uint32_t kNoInput = 0xFFFFFFFFu;

struct Program {
  Id id = 0;  // HashId(patch name)
  std::vector<CompiledNode> nodes;  // dependency order; the last node is the output
};

// Checks `doc` (a patch object) and converts it. `resolveParameter` turns a
// parameterCurve's "parameter" reference into an id (0 = unknown: an error is reported
// by the caller's resolver); may be null (then HashId of the text). Problems are
// appended; returns false if any is an error.
using ParameterResolver = Id (*)(void* user, const json::Value& ref);
bool Compile(const json::Value& doc, Id patchId, Program* out, std::vector<Problem>* problems,
             ParameterResolver resolveParameter = nullptr, void* user = nullptr);

// Bank chunk (provisional layout, see above). `links`: (SYNP row, program index).
struct Link {
  std::uint32_t synpRow = 0;
  std::uint32_t program = 0;
};
std::vector<std::uint8_t> WritePatchChunk(const std::vector<Program>& programs, const std::vector<Link>& links);
bool ReadPatchChunk(const std::uint8_t* data, std::uint64_t bytes, std::vector<Program>* programs,
                    std::vector<Link>* links);

}  // namespace snd::buildtool::patch

#endif  // SND_BUILDTOOL_PATCH_HPP
