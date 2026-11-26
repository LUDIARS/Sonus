// Patch graphs: node table, checks, dependency order, provisional bank chunk (patch.hpp).
#include "patch_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "snd/format/bank_format.hpp"
#include "snd/format/patch_program.hpp"

namespace snd::buildtool::patch {
namespace {

using json::Value;
static_assert(kChunkFourcc == ::snd::Fourcc('X', 'P', 'C', 'H'));

const char* const kWaveforms[] = {"sine", "saw", "square", "triangle", "pulse", nullptr};
const char* const kNoiseColors[] = {"white", "pink", "brown", nullptr};
const char* const kFilterModes[] = {"lowpass", "highpass", "bandpass", nullptr};

constexpr float kBig = 1.0e6f;

// The node table. Order = the kind's number in the bank (append only).
const NodeKindInfo kKinds[] = {
    {"oscillator", Signal::Audio, 2,
     {{"pitch", Signal::Control, false}, {"amp", Signal::Control, false}},
     3,
     {{"waveform", 0, 4, 0, kWaveforms}, {"frequencyHz", 1.0f, 20000.0f, 440.0f, nullptr},
      {"duty", 0.01f, 0.99f, 0.5f, nullptr}},
     false, false},
    {"noise", Signal::Audio, 1, {{"amp", Signal::Control, false}}, 2,
     {{"color", 0, 2, 0, kNoiseColors}, {"seed", 0.0f, 4294967040.0f, 0.0f, nullptr}}, false, false},
    {"filter", Signal::Audio, 2, {{"in", Signal::Audio, true}, {"cutoff", Signal::Control, false}}, 3,
     {{"mode", 0, 2, 0, kFilterModes}, {"cutoffHz", 10.0f, 24000.0f, 1000.0f, nullptr},
      {"q", 0.1f, 40.0f, 0.707f, nullptr}},
     false, false},
    {"amp", Signal::Audio, 2, {{"in", Signal::Audio, true}, {"gain", Signal::Control, false}}, 1,
     {{"gainDb", -144.0f, 24.0f, 0.0f, nullptr}}, false, false},
    {"mix", Signal::Audio, 2, {{"a", Signal::Audio, true}, {"b", Signal::Audio, true}}, 2,
     {{"levelADb", -144.0f, 24.0f, 0.0f, nullptr}, {"levelBDb", -144.0f, 24.0f, 0.0f, nullptr}}, false, false},
    {"adsr", Signal::Control, 0, {}, 4,
     {{"attackMs", 0.0f, 60000.0f, 5.0f, nullptr}, {"decayMs", 0.0f, 60000.0f, 100.0f, nullptr},
      {"sustain", 0.0f, 1.0f, 0.7f, nullptr}, {"releaseMs", 0.0f, 60000.0f, 200.0f, nullptr}},
     true, false},
    {"lfo", Signal::Control, 0, {}, 3,
     {{"waveform", 0, 4, 0, kWaveforms}, {"rateHz", 0.001f, 200.0f, 1.0f, nullptr},
      {"depth", 0.0f, 1.0f, 1.0f, nullptr}},
     true, false},
    {"random", Signal::Control, 0, {}, 3,
     {{"rateHz", 0.001f, 200.0f, 1.0f, nullptr}, {"min", -kBig, kBig, 0.0f, nullptr},
      {"max", -kBig, kBig, 1.0f, nullptr}},
     true, false},
    {"parameterCurve", Signal::Control, 0, {}, 0, {}, true, true},
    {"envelopeFollower", Signal::Control, 1, {{"in", Signal::Audio, true}}, 2,
     {{"attackMs", 0.0f, 10000.0f, 10.0f, nullptr}, {"releaseMs", 0.0f, 10000.0f, 100.0f, nullptr}}, true, false},
    {"output", Signal::None, 1, {{"in", Signal::Audio, true}}, 0, {}, false, false},
};
constexpr std::uint32_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);

std::vector<std::size_t> DependencyOrder(const std::vector<std::vector<std::size_t>>& preds) {
  const auto n = preds.size();
  // Kahn order (ties: file order); a cycle leaves nodes unplaced.
  std::vector<std::uint32_t> indeg(n, 0);
  std::vector<std::vector<std::size_t>> succ(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t p : preds[i]) {
      succ[p].push_back(i);
      ++indeg[i];
    }
  }
  std::vector<std::size_t> order;
  std::vector<std::size_t> ready;
  for (std::size_t i = 0; i < n; ++i) {
    if (indeg[i] == 0u) ready.push_back(i);
  }
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), std::greater<>());
    const std::size_t x = ready.back();
    ready.pop_back();
    order.push_back(x);
    for (std::size_t s : succ[x]) {
      if (--indeg[s] == 0u) ready.push_back(s);
    }
  }
  return order;
}

std::string NodePath(std::size_t i) { return "nodes[" + std::to_string(i) + "]"; }

bool Number(const Value& v, double* out) { return v.IsNumber() && v.AsDouble(out); }

void PutU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void PutF32(std::vector<std::uint8_t>& b, float f) {
  std::uint32_t v = 0;
  std::memcpy(&v, &f, 4);
  PutU32(b, v);
}
std::uint32_t GetU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
float GetF32(const std::uint8_t* p) {
  const std::uint32_t v = GetU32(p);
  float f = 0;
  std::memcpy(&f, &v, 4);
  return f;
}

// Chunk layout (provisional, bank-format.md §6.6): all little-endian.
//   header   16 B: u32 version, u32 programCount, u32 linkCount, u32 nodeBytes (= 128)
//   programs 16 B each: u32 patchId, u32 nodeFirst, u32 nodeCount, u32 reserved
//   nodes   128 B each: u32 kind, u32 parameterId, u32 pointCount, u32 reserved,
//                       u32 inputs[4], f32 params[8], f32 points[8][2]
//   links     8 B each: u32 synpRow, u32 program
constexpr std::uint32_t kNodeBytes = 128;

}  // namespace

const NodeKindInfo* NodeKinds(std::uint32_t* count) {
  if (count != nullptr) *count = kKindCount;
  return kKinds;
}

const NodeKindInfo* FindNodeKind(const std::string& name, std::uint32_t* index) {
  for (std::uint32_t i = 0; i < kKindCount; ++i) {
    if (name == kKinds[i].name) {
      if (index != nullptr) *index = i;
      return &kKinds[i];
    }
  }
  return nullptr;
}

const InputInfo* FindInput(const NodeKindInfo& kind, const std::string& input, std::uint32_t* index) {
  for (std::uint32_t i = 0; i < kind.inputCount; ++i) {
    if (input == kind.inputs[i].name) {
      if (index != nullptr) *index = i;
      return &kind.inputs[i];
    }
  }
  return nullptr;
}

const ParamInfo* FindParam(const NodeKindInfo& kind, const std::string& param, std::uint32_t* index) {
  for (std::uint32_t i = 0; i < kind.paramCount; ++i) {
    if (param == kind.params[i].name) {
      if (index != nullptr) *index = i;
      return &kind.params[i];
    }
  }
  return nullptr;
}

bool Compile(const json::Value& doc, Id patchId, Program* out, std::vector<Problem>* problems,
             ParameterResolver resolveParameter, void* user) {
  std::vector<Problem> local;
  std::vector<Problem>& pr = problems != nullptr ? *problems : local;
  const std::size_t firstProblem = pr.size();
  auto error = [&](std::string path, std::string msg) { pr.push_back(Problem{true, std::move(path), std::move(msg)}); };
  auto warn = [&](std::string path, std::string msg) { pr.push_back(Problem{false, std::move(path), std::move(msg)}); };
  *out = Program{};
  out->id = patchId;

  const Value* nodes = doc.Find("nodes");
  if (nodes == nullptr || !nodes->IsArray() || nodes->items.empty()) {
    error("nodes", "a patch needs \"nodes\" (an array with an output node)");
    return false;
  }
  if (nodes->items.size() > kMaxNodes) {
    error("nodes", "a patch has at most " + std::to_string(kMaxNodes) + " nodes (" +
                       std::to_string(nodes->items.size()) + " given)");
    return false;
  }
  const std::size_t n = nodes->items.size();
  std::vector<std::uint32_t> kindOf(n, kKindCount);
  std::vector<CompiledNode> compiled(n);
  std::map<std::string, std::size_t> byId;
  std::size_t outputNode = n;
  for (std::size_t i = 0; i < n; ++i) {
    const Value& nd = nodes->items[i];
    const std::string at = NodePath(i);
    if (!nd.IsObject()) {
      error(at, "a node must be an object");
      continue;
    }
    const Value* id = nd.Find("id");
    if (id == nullptr || !id->IsString() || id->text.empty()) {
      error(at + ".id", "a node needs a non-empty string \"id\"");
    } else if (!byId.emplace(id->text, i).second) {
      error(at + ".id", "duplicate node id '" + id->text + "'");
    }
    const Value* kv = nd.Find("kind");
    std::uint32_t ki = kKindCount;
    const NodeKindInfo* kind = (kv != nullptr && kv->IsString()) ? FindNodeKind(kv->text, &ki) : nullptr;
    if (kind == nullptr) {
      std::string list;
      for (std::uint32_t k = 0; k < kKindCount; ++k) list += (k ? ", " : "") + std::string(kKinds[k].name);
      error(at + ".kind", "\"kind\" must be one of: " + list);
      continue;
    }
    kindOf[i] = ki;
    CompiledNode& cn = compiled[i];
    cn.kind = ki;
    cn.source = static_cast<std::uint32_t>(i);
    for (std::uint32_t k = 0; k < kMaxInputs; ++k) cn.inputs[k] = kNoInput;
    for (std::uint32_t k = 0; k < kind->paramCount; ++k) cn.params[k] = kind->params[k].defaultValue;
    if (kind->output == Signal::None) {
      if (outputNode != n) {
        error(at, "a patch has exactly one output node (also " + NodePath(outputNode) + ")");
      } else {
        outputNode = i;
      }
    }
    if (const Value* pos = nd.Find("position"); pos != nullptr) {
      double x = 0, y = 0;
      if (!pos->IsArray() || pos->items.size() != 2 || !Number(pos->items[0], &x) || !Number(pos->items[1], &y)) {
        error(at + ".position", "\"position\" must be [x, y]");
      }
    }
    const Value* params = nd.Find("params");
    if (params != nullptr && !params->IsObject()) {
      error(at + ".params", "\"params\" must be an object");
      params = nullptr;
    }
    if (params == nullptr) {
      if (kind->usesParameter) error(at + ".params", "a parameterCurve needs \"parameter\" and \"points\"");
      continue;
    }
    for (const json::Member& m : params->members) {
      const std::string pp = at + ".params." + m.first;
      if (m.first == "notes") continue;
      if (kind->usesParameter && (m.first == "parameter" || m.first == "points")) continue;
      std::uint32_t pi = 0;
      const ParamInfo* info = FindParam(*kind, m.first, &pi);
      if (info == nullptr) {
        warn(pp, "unknown parameter \"" + m.first + "\" of a " + kind->name + " node ignored");
        continue;
      }
      if (info->choices != nullptr) {
        bool found = false;
        if (m.second.IsString()) {
          for (std::uint32_t c = 0; info->choices[c] != nullptr; ++c) {
            if (m.second.text == info->choices[c]) {
              cn.params[pi] = static_cast<float>(c);
              found = true;
            }
          }
        }
        if (!found) {
          std::string list;
          for (std::uint32_t c = 0; info->choices[c] != nullptr; ++c) list += (c ? ", " : "") + std::string(info->choices[c]);
          error(pp, "\"" + m.first + "\" must be one of: " + list);
        }
        continue;
      }
      double v = 0;
      if (!Number(m.second, &v) || !std::isfinite(v)) {
        error(pp, "\"" + m.first + "\" must be a number");
        continue;
      }
      if (v < info->minValue || v > info->maxValue) {
        warn(pp, "\"" + m.first + "\" " + std::to_string(v) + " is outside " + std::to_string(info->minValue) +
                     " .. " + std::to_string(info->maxValue) + " (clamped)");
        v = std::clamp(v, static_cast<double>(info->minValue), static_cast<double>(info->maxValue));
      }
      cn.params[pi] = static_cast<float>(v);
    }
    if (kind->usesParameter) {
      const Value* ref = params->Find("parameter");
      if (ref == nullptr || !ref->IsString() || ref->text.empty()) {
        error(at + ".params.parameter", "a parameterCurve needs \"parameter\" (a parameter reference)");
      } else {
        cn.parameter = resolveParameter != nullptr ? resolveParameter(user, *ref) : HashId(ref->text);
      }
      const Value* pts = params->Find("points");
      if (pts == nullptr || !pts->IsArray() || pts->items.size() < 2 || pts->items.size() > kMaxCurvePoints) {
        error(at + ".params.points", "\"points\" must hold 2 .. " + std::to_string(kMaxCurvePoints) + " [x, y] points");
      } else {
        double prevX = -INFINITY;
        for (std::size_t p = 0; p < pts->items.size(); ++p) {
          const Value& pt = pts->items[p];
          double x = 0, y = 0;
          if (!pt.IsArray() || pt.items.size() != 2 || !Number(pt.items[0], &x) || !Number(pt.items[1], &y)) {
            error(at + ".params.points[" + std::to_string(p) + "]", "a point must be [x, y]");
            continue;
          }
          if (x <= prevX) error(at + ".params.points[" + std::to_string(p) + "]", "curve points must have ascending x");
          prevX = x;
          cn.points[cn.pointCount][0] = static_cast<float>(x);
          cn.points[cn.pointCount][1] = static_cast<float>(y);
          ++cn.pointCount;
        }
      }
    }
  }
  if (outputNode == n) error("nodes", "a patch needs an output node");
  if (const Value* o = doc.Find("output"); o != nullptr) {
    if (!o->IsString() || byId.count(o->text) == 0u) {
      error("output", "\"output\" must name a node of the patch");
    } else if (outputNode != n && byId[o->text] != outputNode) {
      error("output", "\"output\" must name the output node");
    }
  }

  // Connections: from (node id) -> to (node id).input.
  std::vector<std::vector<std::size_t>> preds(n);
  std::vector<std::vector<std::int64_t>> inputFrom(n, std::vector<std::int64_t>(kMaxInputs, -1));
  if (const Value* conns = doc.Find("connections"); conns != nullptr) {
    if (!conns->IsArray()) {
      error("connections", "\"connections\" must be an array");
    } else {
      for (std::size_t c = 0; c < conns->items.size(); ++c) {
        const Value& cv = conns->items[c];
        const std::string at = "connections[" + std::to_string(c) + "]";
        const Value* f = cv.Find("from");
        const Value* t = cv.Find("to");
        const Value* in = cv.Find("input");
        if (f == nullptr || t == nullptr || !f->IsString() || !t->IsString()) {
          error(at, "a connection needs \"from\" and \"to\" (node ids)");
          continue;
        }
        auto fi = byId.find(f->text);
        auto ti = byId.find(t->text);
        if (fi == byId.end()) {
          error(at + ".from", "unknown node '" + f->text + "'");
          continue;
        }
        if (ti == byId.end()) {
          error(at + ".to", "unknown node '" + t->text + "'");
          continue;
        }
        if (kindOf[fi->second] >= kKindCount || kindOf[ti->second] >= kKindCount) continue;
        const NodeKindInfo& fk = kKinds[kindOf[fi->second]];
        const NodeKindInfo& tk = kKinds[kindOf[ti->second]];
        if (fk.output == Signal::None) {
          error(at + ".from", "the output node has no output to connect");
          continue;
        }
        std::string inName;
        if (in == nullptr) {
          if (tk.inputCount == 0) {
            error(at + ".to", std::string("a ") + tk.name + " node has no inputs");
            continue;
          }
          inName = tk.inputs[0].name;
        } else if (!in->IsString()) {
          error(at + ".input", "\"input\" must be a string");
          continue;
        } else {
          inName = in->text;
        }
        std::uint32_t ii = 0;
        const InputInfo* inInfo = FindInput(tk, inName, &ii);
        if (inInfo == nullptr) {
          error(at + ".input", std::string("a ") + tk.name + " node has no input '" + inName + "'");
          continue;
        }
        if (inInfo->signal != fk.output) {
          error(at, std::string("cannot connect a ") + (fk.output == Signal::Audio ? "audio" : "control") +
                        " output to the " + (inInfo->signal == Signal::Audio ? "audio" : "control") + " input '" +
                        inName + "' of '" + t->text + "'");
          continue;
        }
        if (inputFrom[ti->second][ii] >= 0) {
          error(at, "input '" + inName + "' of '" + t->text + "' is already connected (one connection per input)");
          continue;
        }
        if (fi->second == ti->second) {
          error(at, "a node cannot feed itself (cycle)");
          continue;
        }
        inputFrom[ti->second][ii] = static_cast<std::int64_t>(fi->second);
        preds[ti->second].push_back(fi->second);
      }
    }
  }
  // Required inputs.
  for (std::size_t i = 0; i < n; ++i) {
    if (kindOf[i] >= kKindCount) continue;
    const NodeKindInfo& k = kKinds[kindOf[i]];
    for (std::uint32_t ii = 0; ii < k.inputCount; ++ii) {
      if (k.inputs[ii].required && inputFrom[i][ii] < 0) {
        error(NodePath(i), std::string("input '") + k.inputs[ii].name + "' of a " + k.name + " node is not connected");
      }
    }
  }
  const auto order = DependencyOrder(preds);
  if (order.size() != n) {
    error("connections", "the connections form a cycle");
  }
  // Nodes that do not reach the output are left out (warning).
  std::vector<std::uint8_t> live(n, 0);
  if (outputNode != n) {
    std::vector<std::size_t> work{outputNode};
    live[outputNode] = 1;
    while (!work.empty()) {
      const std::size_t x = work.back();
      work.pop_back();
      for (std::size_t p : preds[x]) {
        if (live[p] == 0u) {
          live[p] = 1;
          work.push_back(p);
        }
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (live[i] == 0u && kindOf[i] < kKindCount) warn(NodePath(i), "node does not reach the output (left out)");
    }
  }
  bool failed = false;
  for (std::size_t i = firstProblem; i < pr.size(); ++i) failed = failed || pr[i].error;
  if (failed) return false;
  std::vector<std::uint32_t> programIndex(n, kNoInput);
  for (std::size_t x : order) {
    if (live[x] == 0u) continue;
    programIndex[x] = static_cast<std::uint32_t>(out->nodes.size());
    CompiledNode cn = compiled[x];
    for (std::uint32_t ii = 0; ii < kMaxInputs; ++ii) {
      const std::int64_t f = inputFrom[x][ii];
      cn.inputs[ii] = f >= 0 ? programIndex[static_cast<std::size_t>(f)] : kNoInput;
    }
    out->nodes.push_back(cn);
  }
  return true;
}

namespace pt = bank_format::patch;

static bool EncodeNodes(const std::vector<pt::PatchNode>& nodes, std::vector<std::uint8_t>& bytes) {
  const auto count = static_cast<unsigned>(nodes.size());
  if (count > pt::kPatchMaxNodes) return false;
  bytes.resize(pt::PatchProgramBytes(count));
  return pt::Encode(nodes.data(), count, bytes.data(), bytes.size()) != 0;
}

bool CompileInline(const Value& synth, std::vector<std::uint8_t>& bytes,
                   const Value*& at, std::string& message) {
  auto fail = [&](const Value& where, const char* why) {
    at = &where;
    message = why;
    return false;
  };
  const auto* list = synth.Find("nodes");
  const auto* output = synth.Find("output");
  if (!list || !list->IsArray() || list->items.size() < 2 || list->items.size() > pt::kPatchMaxNodes)
    return fail(list ? *list : synth, "PTCH needs 2..16 nodes");
  if (!output || !output->IsString()) return fail(synth, "expected an output node id");
  const auto n = list->items.size();
  std::map<std::string, std::size_t> ids;
  for (std::size_t i = 0; i < n; ++i) {
    const auto* id = list->items[i].Find("id");
    if (!id || !id->IsString() || id->text.empty() || !ids.emplace(id->text, i).second)
      return fail(list->items[i], "expected a unique nonempty node id");
  }
  const auto& ops = pt::kOps;
  std::vector<pt::PatchNode> nodes(n);
  std::vector<std::vector<std::size_t>> preds(n);
  for (std::size_t i = 0; i < n; ++i) {
    const auto& node = list->items[i];
    const auto* op = node.Find("op");
    auto entry = std::find_if(std::begin(ops), std::end(ops), [&](const pt::OpInfo& x) {
      return op && op->IsString() && op->text == x.name;
    });
    if (entry == std::end(ops)) return fail(node, "unknown patch op");
    auto& decoded = nodes[i];
    decoded.op = static_cast<pt::PatchOp>(entry - ops);
    if ((decoded.op == pt::PatchOp::Output) != (node.Find("id")->text == output->text))
      return fail(node, "output must name the only output node");
    const auto* inputs = node.Find("inputs");
    const auto* args = node.Find("args");
    auto matches = [](const Value* array, unsigned count) {
      return array ? array->IsArray() && array->items.size() == count : !count;
    };
    if (!matches(inputs, entry->inputs) || !matches(args, entry->args))
      return fail(node, "wrong number or type of patch inputs or args");
    for (unsigned j = 0; j < entry->inputs; ++j) {
      const auto& ref = inputs->items[j];
      auto pred = ids.find(ref.text);
      if (!ref.IsString() || pred == ids.end()) return fail(ref, "unknown patch input id");
      preds[i].push_back(pred->second);
    }
    for (unsigned j = 0; j < entry->args; ++j) {
      double x;
      if (!args->items[j].AsDouble(&x) || !(x >= entry->bounds[j].lo && x <= entry->bounds[j].hi))
        return fail(args->items[j], "patch argument is outside its numeric range");
      decoded.arg[j] = static_cast<float>(x);
    }
    if (const auto* wave = node.Find("waveform")) {
      const auto last = std::end(kWaveforms) - 1;
      const auto w = std::find(std::begin(kWaveforms), last, wave->text);
      if (decoded.op != pt::PatchOp::Oscillator || !wave->IsString() || w == last)
        return fail(*wave, "invalid patch waveform");
      decoded.waveform = static_cast<std::uint8_t>(w - kWaveforms);
    }
  }
  const auto order = DependencyOrder(preds);
  if (order.size() != n) return fail(*list, "cycle in patch graph");
  std::vector<pt::PatchNode> sorted;
  std::vector<std::uint8_t> indices(n);
  for (auto i : order) {
    auto node = nodes[i];
    if (!preds[i].empty()) node.a = indices[preds[i][0]];
    if (preds[i].size() == 2) node.b = indices[preds[i][1]];
    indices[i] = static_cast<std::uint8_t>(sorted.size());
    sorted.push_back(node);
  }
  if (!EncodeNodes(sorted, bytes)) return fail(*list, "disconnected patch graph or argument outside PTCH range");
  return true;
}

// Lower the existing editor IR only where PTCH v1 expresses the same signal.
// An unrepresentable graph must not turn into a successful but silent game asset.
bool EncodeRuntime(const Program& program, std::vector<std::uint8_t>& bytes) {
  using Op = pt::PatchOp;
  std::vector<pt::PatchNode> nodes;
  std::vector<std::uint8_t> index;
  auto append = [&](pt::PatchNode node) {
    nodes.push_back(node);
    return static_cast<std::uint8_t>(nodes.size() - 1);
  };
  auto gain = [&](std::uint8_t input, float db) {
    if (db == 0) return input;
    pt::PatchNode constant{Op::Constant};
    constant.arg[0] = std::pow(10.f, db / 20.f);
    const auto scale = append(constant);
    return append({Op::Multiply, input, scale});
  };
  for (const auto& ir : program.nodes) {
    const auto& info = kKinds[ir.kind];
    auto input = [&](unsigned pin) { return ir.inputs[pin] == kNoInput ? pt::kPatchNoInput : index[ir.inputs[pin]]; };
    const std::string_view kind(info.name);
    const auto* p = ir.params;
    pt::PatchNode node;
    if (kind == "oscillator" || kind == "noise") {
      if (kind == "oscillator") {
        if (input(0) != pt::kPatchNoInput) return false;
        node = {Op::Oscillator};
        node.waveform = static_cast<std::uint8_t>(p[0]);
        node.arg[0] = p[1];
        node.arg[1] = p[2];
      } else {
        if (p[0] != 0 || p[1] != 0) return false;
        node = {Op::WhiteNoise};
      }
      auto signal = append(node);
      const auto amp = input(kind == "oscillator" ? 1 : 0);
      if (amp != pt::kPatchNoInput) signal = append({Op::Multiply, signal, amp});
      index.push_back(signal);
    } else if (kind == "amp") {
      auto signal = gain(input(0), p[0]);
      if (input(1) != pt::kPatchNoInput) signal = append({Op::Multiply, signal, input(1)});
      index.push_back(signal);
    } else if (kind == "mix") {
      const auto a = gain(input(0), p[0]);
      const auto b = gain(input(1), p[1]);
      index.push_back(append({Op::Add, a, b}));
    } else if (kind == "adsr") {
      node = {Op::Adsr};
      node.arg[0] = p[0] / 1000;
      node.arg[1] = p[1] / 1000;
      node.arg[2] = p[2];
      node.arg[3] = p[3] / 1000;
      index.push_back(append(node));
    } else if (kind == "output") {
      index.push_back(append({Op::Output, input(0)}));
    } else {
      return false;
    }
  }
  return EncodeNodes(nodes, bytes);
}

std::vector<std::uint8_t> WritePatchChunk(const std::vector<Program>& programs, const std::vector<Link>& links) {
  std::vector<std::uint8_t> b;
  PutU32(b, kChunkVersion);
  PutU32(b, static_cast<std::uint32_t>(programs.size()));
  PutU32(b, static_cast<std::uint32_t>(links.size()));
  PutU32(b, kNodeBytes);
  std::uint32_t first = 0;
  for (const Program& p : programs) {
    PutU32(b, p.id);
    PutU32(b, first);
    PutU32(b, static_cast<std::uint32_t>(p.nodes.size()));
    PutU32(b, 0);
    first += static_cast<std::uint32_t>(p.nodes.size());
  }
  for (const Program& p : programs) {
    for (const CompiledNode& n : p.nodes) {
      PutU32(b, n.kind);
      PutU32(b, n.parameter);
      PutU32(b, n.pointCount);
      PutU32(b, 0);
      for (std::uint32_t i = 0; i < kMaxInputs; ++i) PutU32(b, n.inputs[i]);
      for (std::uint32_t i = 0; i < kMaxParams; ++i) PutF32(b, n.params[i]);
      for (std::uint32_t i = 0; i < kMaxCurvePoints; ++i) {
        PutF32(b, n.points[i][0]);
        PutF32(b, n.points[i][1]);
      }
    }
  }
  for (const Link& l : links) {
    PutU32(b, l.synpRow);
    PutU32(b, l.program);
  }
  return b;
}

bool ReadPatchChunk(const std::uint8_t* data, std::uint64_t bytes, std::vector<Program>* programs,
                    std::vector<Link>* links) {
  programs->clear();
  links->clear();
  if (data == nullptr || bytes < 16) return false;
  if (GetU32(data) != kChunkVersion || GetU32(data + 12) != kNodeBytes) return false;
  const std::uint64_t pc = GetU32(data + 4), lc = GetU32(data + 8);
  std::uint64_t off = 16;
  if (off + pc * 16 > bytes) return false;
  std::uint64_t totalNodes = 0;
  struct Head {
    std::uint32_t id, first, count;
  };
  std::vector<Head> heads;
  for (std::uint64_t i = 0; i < pc; ++i, off += 16) {
    heads.push_back(Head{GetU32(data + off), GetU32(data + off + 4), GetU32(data + off + 8)});
    totalNodes += GetU32(data + off + 8);
  }
  const std::uint64_t nodeOff = off;
  if (nodeOff + totalNodes * kNodeBytes + lc * 8 > bytes) return false;
  std::uint64_t expectFirst = 0;
  for (const Head& h : heads) {
    // Programs follow each other in the node array (WritePatchChunk), none empty.
    if (h.first != expectFirst || h.count == 0u || std::uint64_t{h.first} + h.count > totalNodes) return false;
    expectFirst += h.count;
    Program p;
    p.id = h.id;
    for (std::uint32_t k = 0; k < h.count; ++k) {
      const std::uint8_t* q = data + nodeOff + (std::uint64_t{h.first} + k) * kNodeBytes;
      CompiledNode n;
      n.kind = GetU32(q);
      n.parameter = GetU32(q + 4);
      n.pointCount = GetU32(q + 8);
      if (n.pointCount > kMaxCurvePoints) return false;
      if (n.kind >= kKindCount) return false;
      for (std::uint32_t i = 0; i < kMaxInputs; ++i) {
        n.inputs[i] = GetU32(q + 16 + 4 * i);
        // Dependency order: an input is an earlier node of the same program, or none.
        if (n.inputs[i] != kNoInput && n.inputs[i] >= k) return false;
      }
      for (std::uint32_t i = 0; i < kMaxParams; ++i) n.params[i] = GetF32(q + 32 + 4 * i);
      for (std::uint32_t i = 0; i < kMaxCurvePoints; ++i) {
        n.points[i][0] = GetF32(q + 64 + 8 * i);
        n.points[i][1] = GetF32(q + 68 + 8 * i);
      }
      p.nodes.push_back(n);
    }
    programs->push_back(std::move(p));
  }
  const std::uint64_t linkOff = nodeOff + totalNodes * kNodeBytes;
  if (expectFirst != totalNodes) return false;
  for (std::uint64_t i = 0; i < lc; ++i) {
    const Link l{GetU32(data + linkOff + 8 * i), GetU32(data + linkOff + 8 * i + 4)};
    if (l.program >= pc) return false;
    links->push_back(l);
  }
  return true;
}

}  // namespace snd::buildtool::patch
