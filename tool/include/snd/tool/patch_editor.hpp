// Waveform synthesis patch editor model (design.md 5, 5.1, 11.1 波形生成パッチエディタ;
// project-format.md "patch").
//
// Edits a patch object: nodes (id, kind from the shared node table of
// snd/buildtool/patch.hpp, parameters, screen position in "position": [x, y] — not in
// notes), connections (from a node's output to a named input of another node) and the
// output node. Every edit is checked before it reaches the document:
//   - at most patch::kMaxNodes nodes; node ids unique and non-empty; one output node;
//   - parameters known to the kind, numbers inside the range (enumerations by name);
//   - a connection joins an output to an input of the same signal (audio / control), one
//     connection per input, no node feeding itself, no cycle.
// Check() runs the build's patch compiler (the same problems, by member path) and
// Compiled() gives the dependency-ordered node array the build writes.
#ifndef SND_TOOL_PATCH_EDITOR_HPP
#define SND_TOOL_PATCH_EDITOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/buildtool/patch.hpp"
#include "snd/tool/document.hpp"

namespace snd::tool {

struct PatchNode {
  std::string id;
  std::string kind;
  float x = 0.0f, y = 0.0f;
};

struct PatchConnection {
  std::string from;
  std::string to;
  std::string input;
};

class PatchEditor {
 public:
  PatchEditor(ProjectDocument& doc, std::string guid);
  bool valid() const;
  std::vector<PatchNode> Nodes() const;
  std::vector<PatchConnection> Connections() const;
  std::string OutputNode() const;  // id ("" if none)

  // A new node of `kind` at (x, y); its id is `id` or, when empty, "<kind><n>". Returns the id.
  std::string AddNode(const std::string& kind, float x, float y, const std::string& id = std::string());
  // Removes the node and its connections (one step).
  bool RemoveNode(const std::string& id);
  bool MoveNode(const std::string& id, float x, float y, std::uint32_t mergeKey = 0);
  bool RenameNode(const std::string& id, const std::string& newId);  // connections and output follow
  bool SetParam(const std::string& id, const std::string& param, double value, std::uint32_t mergeKey = 0);
  bool SetChoice(const std::string& id, const std::string& param, const std::string& choice);
  // parameterCurve nodes.
  bool SetCurve(const std::string& id, const std::string& parameterGuid, const std::vector<std::pair<double, double>>& points);
  double Param(const std::string& id, const std::string& param) const;  // the table default when absent

  bool Connect(const std::string& from, const std::string& to, const std::string& input = std::string());
  bool Disconnect(const std::string& to, const std::string& input);

  std::vector<buildtool::patch::Problem> Check() const;
  bool Compiled(buildtool::patch::Program* out) const;

 private:
  int NodeIndex(const std::string& id) const;
  bool WouldCycle(const std::string& from, const std::string& to) const;
  ProjectDocument& doc_;
  std::string guid_;
};

}  // namespace snd::tool

#endif  // SND_TOOL_PATCH_EDITOR_HPP
