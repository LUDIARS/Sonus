// Project -> banks (design.md 10). See build.hpp and spec/interface/project-format.md.
#include "snd/buildtool/build.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>

#include "convert.hpp"
#include "detail/file_io.hpp"
#include "snd/base/atomic.hpp"
#include "snd/base/hash.hpp"
#include "project.hpp"
#include "self_check.hpp"
#include "snd/buildtool/bank_writer.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/ext.h"

namespace snd::buildtool {
namespace {

namespace fs = std::filesystem;
namespace bf = bank_format;
namespace col = bf::col;
namespace ch = bf::chunk;
using detail::Converted;
using detail::Kind;
using detail::Object;
using json::Value;
constexpr std::uint32_t kNone = bf::kNone;

// --- enumerations (JSON names -> column values) --------------------------------------------
const char* const kSequence[] = {"polyphonic", "random", "randomNoRepeat", "sequential", "shuffle",
                                 "switch", "comboSequential", "layerCrossfade", nullptr};
const char* const kVirtualReturn[] = {"continue", "restart", "never", nullptr};
const char* const kLoadForm[] = {"pcm", "compressed", "decompressCache", "stream", nullptr};
const char* const kStall[] = {"time", "sound", nullptr};
const char* const kIoPriority[] = {"normal", "music", "voice", nullptr};
const char* const kAction[] = {"play", "stop", "setParameter", "applySnapshot", nullptr};
const char* const kCurveTarget[] = {"volume", "pitch", "lowpass", "highpass", "pan", "send", "effectParam",
                                    "spread", "reverbSend", "blend3D", nullptr};
const char* const kCompose[] = {"addDb", "addCents", "min", "max", "multiply", nullptr};
const char* const kParamKind[] = {"continuous", "switch", nullptr};
const char* const kSteal[] = {"oldest", "quietest", "lowestPriority", "never", nullptr};
const char* const kDuckKind[] = {"sidechain", "rule", nullptr};
const char* const kSynthKind[] = {"oscillator", "noise", "granular", "patch", "wavetable", nullptr};
const char* const kWaveform[] = {"sine", "saw", "square", "triangle", "pulse", nullptr};
const char* const kNoise[] = {"white", "pink", "brown", nullptr};
const char* const kPlaylistMode[] = {"sequential", "random", "shuffle", nullptr};
const char* const kTiming[] = {"immediate", "nextBeat", "nextBar", "barsAfter", "grid", "nextMarker",
                               "segmentEnd", nullptr};
const char* const kStartPos[] = {"head", "sameBar", "marker", "lastPosition", nullptr};
const char* const kInterrupt[] = {"allowed", "afterBridge", nullptr};
const char* const kLate[] = {"nextGrid", "startLate", "cancel", nullptr};
const char* const kOverlap[] = {"replace", "stack", nullptr};
const char* const kMarkerKind[] = {"user", "entry", "exit", "pickup", nullptr};
const char* const kSnapKind[] = {"busVolume", "effectParam", "sendLevel", nullptr};
const char* const kContact[] = {"impact", "slide", "roll", "break", nullptr};

// Compose rule per curve target (4.3).
std::uint32_t DefaultCompose(std::uint32_t target) {
  switch (target) {
    case 0: return 0;  // volume: dB add
    case 1: return 1;  // pitch: cents add
    case 2: return 2;  // lowpass: min
    case 3: return 3;  // highpass: max
    default: return 4; // multiply
  }
}

std::uint64_t Align64(std::uint64_t v) { return base::AlignUp64(v, 64); }  // base (A1-12)

bool ReadBinary(const fs::path& p, std::vector<std::uint8_t>* out) { return detail::ReadFileBytes(p, out); }

// Name -> file-name component ([A-Za-z0-9_], other characters '_', no "__").
std::string FileSafe(const std::string& name) {
  std::string s;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    const char out = ok ? c : '_';
    if (out == '_' && !s.empty() && s.back() == '_') continue;
    s.push_back(out);
  }
  return s.empty() ? std::string("_") : s;
}

// Name -> identifier of sonus_ids.h: the fixed prefix "id_" followed by the
// name with every character outside [A-Za-z0-9_] replaced by '_', runs of '_'
// collapsed and leading '_' dropped. The prefix keeps every identifier clear of
// keywords, of names starting with a digit or '_' (reserved), and of macros
// (standard and Sonus macros are upper case, start with SND_ or '_', or are a
// few lower-case names such as assert / errno, none starting with "id_").
// Names that end up equal get "_2", "_3", ... in id order (WriteIdOutputs).
std::string HeaderIdent(const std::string& name) {
  std::string s = "id_";
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    const char out = ok ? c : '_';
    if (out == '_' && s.back() == '_') continue;  // no "__", no "id__"
    s.push_back(out);
  }
  return s;
}

const Value& Empty() {
  static const Value v = json::MakeObject();
  return v;
}
const Value& EmptyArray() {
  static const Value v = json::MakeArray();
  return v;
}

// ---------------------------------------------------------------------------
// Per-bank construction state
// ---------------------------------------------------------------------------
struct BankTables {
  TableBuilder evnt{ch::EVNT}, trak{ch::TRAK}, clip{ch::CLIP}, actn{ch::ACTN}, buss{ch::BUSS}, send{ch::SEND},
      effx{ch::EFFX}, efpv{ch::EFPV}, parm{ch::PARM}, curv{ch::CURV}, cpnt{ch::CPNT}, catg{ch::CATG},
      snap{ch::SNAP}, snpv{ch::SNPV}, vlgr{ch::VLGR}, duck{ch::DUCK}, asst{ch::ASST}, seek{ch::SEEK},
      strm{ch::STRM}, synp{ch::SYNP}, mseg{ch::MSEG}, mpls{ch::MPLS}, mple{ch::MPLE}, mstm{ch::MSTM},
      msta{ch::MSTA}, mtrn{ch::MTRN}, mstg{ch::MSTG}, mmrk{ch::MMRK}, mtmp{ch::MTMP}, name{ch::NAME},
      guid{ch::GUID}, alis{ch::ALIS};
  std::vector<std::uint8_t> wave, prel, spth, efbl, strs, phys;
  std::map<std::string, std::uint32_t> strOffsets;
  std::uint64_t dspBytes = 0;
  std::uint32_t dspCount = 0;
};

struct PendingFile {
  std::string rel;
  std::vector<std::uint8_t> bytes;
};

struct BankBuild {
  const Object* bank = nullptr;
  std::string language;
  std::string file;
  std::uint32_t bankId = 0;
  BankWriter writer;
  BankHeaderInfo header;
  std::uint64_t contentHash = 0;
};

struct IdEntry {
  std::string kind;
  std::string name;
  Id id = 0;
  std::string guid;
  std::string aliasOf;  // for aliases: the current name
  std::set<std::string> banks;
};

class Builder {
 public:
  Builder(const BuildOptions& o, Diagnostics& d, BuildResult* r) : opt_(o), d_(d), res_(r) {
    enc_ = o.encoders != nullptr ? o.encoders : &builtin_;
  }
  bool Run();

 private:
  // --- setup ----------------------------------------------------------------------------
  void ReadProjectSettings();
  void CheckIds();
  void CheckNestedCycles();
  bool OrderBanks(std::vector<const Object*>* order);
  const Object* MasterBus(const Object& from, const Value& at);
  void CheckBusRoots();
  // Kahn order: `before` must precede everything it points to; ties by name.
  std::vector<const Object*> TopoOrder(const std::vector<const Object*>& nodes,
                                       const std::map<const Object*, std::vector<const Object*>>& edges,
                                       const char* what);
  bool CanReach(const std::string& fromBank, const std::string& toBank) const;
  void CheckAccess(const Object& from, const Value& at, const Object& target, const std::string& bank);

  // --- per bank -------------------------------------------------------------------------
  void BuildBank(const Object& bank, const std::string& language);
  std::uint32_t Str(BankTables& t, const std::string& s);
  void AddName(BankTables& t, Id id, const std::string& s, bf::NameKind kind);
  void AddGuid(BankTables& t, const Object& o, bf::NameKind kind);
  std::uint32_t EmitCurve(BankTables& t, const Object& o, const Value& curve, bool distance, std::uint32_t target,
                          std::set<const Object*>* params);
  bool ConvertAsset(const Object& asset, const std::string& language, Converted* out);
  // Channels an effect of bus `b` runs at: the bus's, else the nearest parent's, else 2 (Config.outputChannels).
  std::uint32_t BusChannels(const Object& b);
  // dsp sizing + validation of one bus effect (diagnostics only when `report`).
  std::uint32_t DefaultEffectStateBytes(const Object& bus, const Value& at, const std::string& type, std::uint32_t typeId,
                                        const std::vector<float>& params, std::uint32_t channels, bool report,
                                        std::uint64_t blobBytes, const std::uint8_t* blob);
  void EmitAsset(BankTables& t, const Object& asset, const std::string& language, const std::string& bankName);

  // --- outputs ----------------------------------------------------------------------------
  void AddIdEntry(const std::string& kind, const std::string& name, const std::string& guid,
                  const std::string& bank, const std::string& aliasOf = "");
  void WriteIdOutputs();
  void Stage(const std::string& rel, std::vector<std::uint8_t> bytes);
  bool Commit();

  const BuildOptions& opt_;
  Diagnostics& d_;
  BuildResult* res_;
  detail::Project p_;
  EncoderRegistry builtin_;
  const EncoderRegistry* enc_ = nullptr;
  std::vector<std::string> languages_;
  const Value* platformDefaults_ = nullptr;  // project.platforms["default"]
  const Value* platformSettings_ = nullptr;  // project.platforms[<platform>]
  std::set<std::pair<const Object*, std::size_t>> effectChecked_;  // (bus, effect index) diagnosed
  Diagnostics quiet_;  // lookups whose errors are reported elsewhere
  std::map<std::string, std::vector<std::string>> bankDeps_;  // bank -> direct deps
  std::map<std::string, std::uint64_t> bankHash_;            // bank name -> contentHash
  std::map<std::string, bool> bankIsLanguage_;
  std::map<std::uint64_t, Converted> memCache_;
  std::vector<BankBuild> banks_;
  std::vector<PendingFile> pending_;
  std::map<std::pair<std::string, std::string>, IdEntry> ids_;  // (kind, name)
};

// ---------------------------------------------------------------------------
bool Builder::Run() {
  std::vector<std::string> skip;
  if (!opt_.outDir.empty()) skip.push_back(opt_.outDir);
  if (!opt_.cacheDir.empty()) skip.push_back(opt_.cacheDir);
  if (!p_.Load(opt_.projectDir, skip, d_)) return false;
  ReadProjectSettings();
  CheckIds();
  CheckNestedCycles();
  CheckBusRoots();
  for (const Object* dk : p_.OfKind(Kind::Ducking)) {
    if (dk->bank.empty()) p_.WarnAt(*dk, dk->doc, "ducking without \"bank\" is not built into any bank", d_);
  }
  std::vector<const Object*> order;
  if (!OrderBanks(&order) || d_.errors() > 0) return false;
  for (const Object* b : order) {
    if (bankIsLanguage_[b->name]) {
      if (languages_.empty()) {
        p_.ErrorAt(*b, *b->doc.Find("language"), "language bank but the project declares no languages", d_);
      }
      for (const std::string& lang : languages_) BuildBank(*b, lang);
    } else {
      BuildBank(*b, "");
    }
  }
  if (d_.errors() > 0) return false;
  // Project build hash over every bank's content (live link handshake, 11.2).
  base::Fnv1a64 h;
  for (const BankBuild& b : banks_) h.Str(b.file).U64(b.contentHash);
  res_->projectBuildHash = h.value();
  for (BankBuild& b : banks_) {
    b.header.projectBuildHash = res_->projectBuildHash;
    std::vector<std::uint8_t> bytes = b.writer.Finish(b.header);
    BuiltBank bb;
    bb.name = b.bank->name;
    bb.language = b.language;
    bb.path = b.file;
    bb.bankId = b.bankId;
    bb.contentHash = b.contentHash;
    bb.bytes = bytes.size();
    res_->banks.push_back(bb);
    // Self-check: every bank the build writes must pass the runtime's validation.
    std::string why;
    if (!detail::SelfCheckBank(bytes, &why)) {
      d_.Error(b.file, 0, 0, "internal error: the runtime would reject this bank (" + why + ")");
      continue;
    }
    Stage(b.file, std::move(bytes));
  }
  if (d_.errors() > 0) return false;
  WriteIdOutputs();
  if (opt_.warningsAsErrors && d_.warnings() > 0) {
    d_.Error("", 0, 0, std::to_string(d_.warnings()) + " warning(s) treated as errors (--werror)");
    return false;
  }
  return Commit();
}

void Builder::ReadProjectSettings() {
  const Object* proj = p_.project();
  if (proj == nullptr) return;
  if (const Value* v = proj->doc.Find("formatVersion"); v != nullptr) {
    std::uint64_t ver = 0;
    if (!v->AsU64(&ver) || ver != 1) p_.ErrorAt(*proj, *v, "unsupported project formatVersion (this tool reads 1)", d_);
  }
  if (const Value* langs = proj->doc.Find("languages"); langs != nullptr) {
    if (!langs->IsArray()) {
      p_.ErrorAt(*proj, *langs, "\"languages\" must be an array of strings", d_);
    } else {
      for (const Value& l : langs->items) {
        if (!l.IsString() || l.text.empty() || l.text.find_first_of("./\\") != std::string::npos) {
          p_.ErrorAt(*proj, l, "a language is a non-empty string without '.', '/' or '\\'", d_);
          continue;
        }
        languages_.push_back(l.text);
      }
    }
  }
  if (const Value* plats = proj->doc.Find("platforms"); plats != nullptr && plats->IsObject()) {
    platformDefaults_ = plats->Find("default");
    platformSettings_ = plats->Find(CodecName(opt_.platform));
  }
}

// Project-wide id collisions (10.1): different strings with the same hash.
void Builder::CheckIds() {
  struct Seen {
    std::string text;
    std::string where;
  };
  std::map<Id, Seen> seen;
  auto check = [&](const std::string& text, const Object& o, const Value& at) {
    if (text.empty()) return;
    const Id id = HashId(text);
    const std::string where = o.file + ":" + std::to_string(at.line) + ":" + std::to_string(at.column);
    auto it = seen.find(id);
    if (it == seen.end()) {
      seen.emplace(id, Seen{text, where});
    } else if (it->second.text != text) {
      p_.ErrorAt(o, at,
                 "id collision: '" + text + "' and '" + it->second.text + "' (" + it->second.where + ") both hash to " +
                     detail::Hex32(id) + "; rename one of them",
                 d_);
    }
  };
  for (const Object& o : p_.objects()) {
    const Value* nameV = o.doc.Find("name");
    if (nameV != nullptr) check(o.name, o, *nameV);
    if (const Value* al = o.doc.Find("aliases"); al != nullptr) {
      if (!al->IsArray()) {
        p_.ErrorAt(o, *al, "\"aliases\" must be an array of strings", d_);
      } else {
        for (const Value& a : al->items) {
          if (!a.IsString() || a.text.empty()) {
            p_.ErrorAt(o, a, "an alias must be a non-empty string", d_);
            continue;
          }
          // An old name must not be a current name of any kind: its id would then
          // be both live and an alias (ALIS oldId -> id must be a rename).
          for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
            const Object* other = p_.FindByName(static_cast<Kind>(k), a.text);
            if (other != nullptr) {
              p_.ErrorAt(o, a, "alias '" + a.text + "' is the name of " + other->file, d_);
              break;
            }
          }
          check(a.text, o, a);
        }
      }
    }
    if (o.kind == Kind::Event) {
      if (const Value* m = o.doc.Find("music"); m != nullptr && m->IsObject()) {
        for (const char* list : {"states", "stingers"}) {
          if (const Value* arr = m->Find(list); arr != nullptr && arr->IsArray()) {
            for (const Value& s : arr->items) {
              if (const Value* n = s.Find("name"); n != nullptr && n->IsString()) check(n->text, o, *n);
            }
          }
        }
      }
      if (const Value* tracks = o.doc.Find("tracks"); tracks != nullptr && tracks->IsArray()) {
        for (const Value& tr : tracks->items) {
          if (const Value* clips = tr.Find("clips"); clips != nullptr && clips->IsArray()) {
            for (const Value& c : clips->items) {
              if (const Value* sv = c.Find("switchValue"); sv != nullptr && sv->IsString()) check(sv->text, o, *sv);
            }
          }
        }
      }
    }
    if (o.kind == Kind::Segment) {
      if (const Value* mk = o.doc.Find("markers"); mk != nullptr && mk->IsArray()) {
        for (const Value& m : mk->items) {
          if (const Value* n = m.Find("name"); n != nullptr && n->IsString()) check(n->text, o, *n);
        }
      }
    }
    if (o.kind == Kind::Bus) {
      if (const Value* fx = o.doc.Find("effects"); fx != nullptr && fx->IsArray()) {
        for (const Value& e : fx->items) {
          if (const Value* t = e.Find("type"); t != nullptr && t->IsString()) check(t->text, o, *t);
        }
      }
    }
  }
  if (const Object* proj = p_.project(); proj != nullptr) {
    if (const Value* langs = proj->doc.Find("languages"); langs != nullptr && langs->IsArray()) {
      for (const Value& l : langs->items) {
        if (l.IsString()) check(l.text, *proj, l);
      }
    }
  }
}

// Nested events (CLIP "event") must not form a cycle (4.2).
void Builder::CheckNestedCycles() {
  std::map<const Object*, std::vector<std::pair<const Object*, const Value*>>> edges;
  Diagnostics quiet;  // reference errors are reported when the bank is built
  for (const Object* e : p_.OfKind(Kind::Event)) {
    const Value* tracks = e->doc.Find("tracks");
    if (tracks == nullptr || !tracks->IsArray()) continue;
    for (const Value& tr : tracks->items) {
      const Value* clips = tr.Find("clips");
      if (clips == nullptr || !clips->IsArray()) continue;
      for (const Value& c : clips->items) {
        const Value* ev = c.Find("event");
        if (ev == nullptr) continue;
        const Object* t = p_.Resolve(*e, *ev, Kind::Event, quiet);
        if (t != nullptr) edges[e].emplace_back(t, ev);
      }
    }
  }
  std::map<const Object*, int> state;  // 0 new, 1 on stack, 2 done
  std::vector<const Object*> stack;
  std::function<void(const Object*)> visit = [&](const Object* n) {
    state[n] = 1;
    stack.push_back(n);
    for (const auto& [t, at] : edges[n]) {
      if (state[t] == 1) {
        std::string path;
        bool on = false;
        for (const Object* s : stack) {
          on = on || s == t;
          if (on) path += "'" + s->name + "' -> ";
        }
        path += "'" + t->name + "'";
        p_.ErrorAt(*n, *at, "nested event cycle: " + path, d_);
      } else if (state[t] == 0) {
        visit(t);
      }
    }
    stack.pop_back();
    state[n] = 2;
  };
  for (const Object* e : p_.OfKind(Kind::Event)) {
    if (state[e] == 0) visit(e);
  }
}

bool Builder::OrderBanks(std::vector<const Object*>* order) {
  const std::vector<const Object*> banks = p_.OfKind(Kind::Bank);
  for (const Object* b : banks) {
    bankIsLanguage_[b->name] = p_.Bool(*b, b->doc, "language", false, d_);
    std::vector<std::string>& deps = bankDeps_[b->name];
    if (const Value* dv = b->doc.Find("dependsOn"); dv != nullptr) {
      if (!dv->IsArray()) {
        p_.ErrorAt(*b, *dv, "\"dependsOn\" must be an array of bank references", d_);
        continue;
      }
      for (const Value& r : dv->items) {
        const Object* t = p_.Resolve(*b, r, Kind::Bank, d_);
        if (t == nullptr) continue;
        if (t == b) {
          p_.ErrorAt(*b, r, "a bank cannot depend on itself", d_);
          continue;
        }
        deps.push_back(t->name);
      }
    }
  }
  for (const Object* b : banks) {
    for (const std::string& dep : bankDeps_[b->name]) {
      if (bankIsLanguage_[dep]) p_.ErrorAt(*b, b->doc, "depends on language bank '" + dep + "' (not supported)", d_);
    }
  }
  // Topological order, alphabetical among ready banks.
  std::map<std::string, int> state;
  bool ok = true;
  std::function<void(const Object*)> visit = [&](const Object* b) {
    state[b->name] = 1;
    std::vector<std::string> deps = bankDeps_[b->name];
    std::sort(deps.begin(), deps.end());
    for (const std::string& dep : deps) {
      if (state[dep] == 1) {
        p_.ErrorAt(*b, *b->doc.Find("dependsOn"), "bank dependency cycle through '" + dep + "'", d_);
        ok = false;
      } else if (state[dep] == 0) {
        visit(p_.FindByName(Kind::Bank, dep));
      }
    }
    state[b->name] = 2;
    order->push_back(b);
  };
  for (const Object* b : banks) {
    if (state[b->name] == 0) visit(b);
  }
  return ok;
}

std::vector<const Object*> Builder::TopoOrder(const std::vector<const Object*>& nodes,
                                              const std::map<const Object*, std::vector<const Object*>>& edges,
                                              const char* what) {
  std::map<const Object*, int> indeg;
  for (const Object* n : nodes) indeg[n] += 0;
  for (const auto& [from, tos] : edges) {
    for (const Object* t : tos) ++indeg[t];
  }
  std::vector<const Object*> ready;
  for (const Object* n : nodes) {
    if (indeg[n] == 0) ready.push_back(n);
  }
  std::vector<const Object*> out;
  auto byName = [](const Object* a, const Object* b) {
    return a->name != b->name ? a->name < b->name : a->guidText < b->guidText;
  };
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), byName);
    const Object* n = ready.front();
    ready.erase(ready.begin());
    out.push_back(n);
    auto it = edges.find(n);
    if (it == edges.end()) continue;
    for (const Object* t : it->second) {
      if (--indeg[t] == 0) ready.push_back(t);
    }
  }
  if (out.size() != nodes.size()) {
    for (const Object* n : nodes) {
      if (indeg[n] > 0) {
        p_.ErrorAt(*n, n->doc, std::string(what) + " cycle through '" + n->name + "'", d_);
        break;
      }
    }
    return nodes;
  }
  return out;
}

bool Builder::CanReach(const std::string& from, const std::string& to) const {
  if (from == to) return true;
  std::set<std::string> seen;
  std::vector<std::string> work{from};
  while (!work.empty()) {
    const std::string b = work.back();
    work.pop_back();
    auto it = bankDeps_.find(b);
    if (it == bankDeps_.end()) continue;
    for (const std::string& d : it->second) {
      if (d == to) return true;
      if (seen.insert(d).second) work.push_back(d);
    }
  }
  return false;
}

// An object assigned to bank A may be used from bank B only if B depends on A.
void Builder::CheckAccess(const Object& from, const Value& at, const Object& target, const std::string& bank) {
  if (target.bank.empty() || CanReach(bank, target.bank)) return;
  p_.ErrorAt(from, at,
             std::string(detail::KindName(target.kind)) + " '" + target.name + "' belongs to bank '" + target.bank +
                 "' but bank '" + bank + "' does not depend on it",
             d_);
}

// The bus tree has exactly one root (a bus without "parent"); it is the master.
const Object* Builder::MasterBus(const Object& from, const Value& at) {
  const Object* root = nullptr;
  for (const Object* b : p_.OfKind(Kind::Bus)) {
    const Value* pv = b->doc.Find("parent");
    if (pv == nullptr || pv->IsNull()) {
      root = b;
      break;
    }
  }
  if (root == nullptr) p_.ErrorAt(from, at, "no \"bus\" given and the project has no root (master) bus", d_);
  return root;
}

void Builder::CheckBusRoots() {
  const Object* first = nullptr;
  for (const Object* b : p_.OfKind(Kind::Bus)) {
    const Value* pv = b->doc.Find("parent");
    if (pv != nullptr && !pv->IsNull()) continue;
    if (first == nullptr) {
      first = b;
      continue;
    }
    p_.ErrorAt(*b, b->doc,
               "bus '" + b->name + "' has no parent but '" + first->name +
                   "' is already the root: the bus tree has exactly one root, the master bus (give it a \"parent\")",
               d_);
  }
}

std::uint32_t Builder::Str(BankTables& t, const std::string& s) {
  auto it = t.strOffsets.find(s);
  if (it != t.strOffsets.end()) return it->second;
  const auto off = static_cast<std::uint32_t>(t.strs.size());
  t.strs.insert(t.strs.end(), s.begin(), s.end());
  t.strs.push_back(0);
  t.strOffsets.emplace(s, off);
  return off;
}

void Builder::AddName(BankTables& t, Id id, const std::string& s, bf::NameKind kind) {
  if (opt_.stripStrings) return;
  const std::uint32_t r = t.name.AddRow();
  t.name.U32(col::NAME_id, r, id);
  t.name.U32(col::NAME_stringOffset, r, Str(t, s));
  t.name.U32(col::NAME_kind, r, static_cast<std::uint32_t>(kind));
}

void Builder::AddGuid(BankTables& t, const Object& o, bf::NameKind kind) {
  if (!opt_.devBuild) return;
  const std::uint32_t r = t.guid.AddRow();
  t.guid.U32(col::GUID_id, r, o.id);
  t.guid.U64(col::GUID_guidLo, r, o.guidLo);
  t.guid.U64(col::GUID_guidHi, r, o.guidHi);
  t.guid.U32(col::GUID_kind, r, static_cast<std::uint32_t>(kind));
}

std::uint32_t Builder::EmitCurve(BankTables& t, const Object& o, const Value& curve, bool distance,
                                 std::uint32_t target, std::set<const Object*>* params) {
  if (!curve.IsObject()) {
    p_.ErrorAt(o, curve, "a curve is an object with \"points\"", d_);
    return kNone;
  }
  std::uint32_t input = bf::kInputDistance;
  if (!distance) {
    const Value* pv = curve.Find("parameter");
    if (pv == nullptr) {
      p_.ErrorAt(o, curve, "a curve needs \"parameter\" (a parameter reference or \"distance\")", d_);
      return kNone;
    }
    if (pv->IsString() && pv->text == "distance") {
      input = bf::kInputDistance;
    } else {
      const Object* param = p_.Resolve(o, *pv, Kind::Parameter, d_);
      if (param == nullptr) return kNone;
      input = param->id;
      params->insert(param);
    }
    target = p_.Enum(o, curve, "target", kCurveTarget, 0, d_);
  }
  const Value* pts = curve.Find("points");
  if (pts == nullptr || !pts->IsArray() || pts->items.size() < 2) {
    p_.ErrorAt(o, pts != nullptr ? *pts : curve, "a curve needs \"points\": at least two [x, y] or [x, y, shape]", d_);
    return kNone;
  }
  const std::uint32_t first = t.cpnt.rows();
  double lastX = -1e300;
  for (const Value& pt : pts->items) {
    double v[3] = {0, 0, 0};
    const bool shapeOk = pt.IsArray() && (pt.items.size() == 2 || pt.items.size() == 3);
    bool ok = shapeOk;
    for (std::size_t i = 0; ok && i < pt.items.size(); ++i) ok = pt.items[i].AsDouble(&v[i]);
    if (!ok) {
      p_.ErrorAt(o, pt, "a curve point is [x, y] or [x, y, shape] (numbers)", d_);
      continue;
    }
    if (v[0] < lastX) p_.ErrorAt(o, pt, "curve points must be sorted by x", d_);
    if (v[2] < 0) p_.ErrorAt(o, pt, "curve shape must be >= 0 (0 linear, >0 exponent)", d_);
    lastX = v[0];
    const std::uint32_t r = t.cpnt.AddRow();
    t.cpnt.F32(col::CPNT_x, r, static_cast<float>(v[0]));
    t.cpnt.F32(col::CPNT_y, r, static_cast<float>(v[1]));
    t.cpnt.F32(col::CPNT_shape, r, static_cast<float>(v[2]));
  }
  const std::uint32_t r = t.curv.AddRow();
  t.curv.U32(col::CURV_pointFirst, r, first);
  t.curv.U32(col::CURV_pointCount, r, t.cpnt.rows() - first);
  t.curv.U32(col::CURV_input, r, input);
  t.curv.U32(col::CURV_target, r, target);
  t.curv.U32(col::CURV_compose, r, p_.Enum(o, curve, "compose", kCompose, DefaultCompose(target), d_));
  return r;
}

// Conversion settings: project platforms.default < platforms[<platform>] <
// asset.convert.default < asset.convert[<platform>].
std::uint32_t Builder::BusChannels(const Object& b) {
  const Object* cur = &b;
  for (int depth = 0; cur != nullptr && depth < 256; ++depth) {  // bus cycles are reported elsewhere
    const std::uint64_t ch = p_.U64(*cur, cur->doc, "channels", 0, quiet_);
    if (ch != 0 && ch <= SND_MAX_CHANNELS) return static_cast<std::uint32_t>(ch);
    const Value* pv = cur->doc.Find("parent");
    cur = (pv != nullptr && !pv->IsNull()) ? p_.Resolve(*cur, *pv, Kind::Bus, quiet_) : nullptr;
  }
  return 2u;
}

std::uint32_t Builder::DefaultEffectStateBytes(const Object& bus, const Value& at, const std::string& type,
                                               std::uint32_t typeId, const std::vector<float>& params,
                                               std::uint32_t channels, bool report, std::uint64_t blobBytes,
                                               const std::uint8_t* blob) {
  const dsp::EffectInfo* info = dsp::FindEffect(typeId);
  const snd_dsp_effect* plugin = nullptr;
  for (const snd_dsp_effect& pe : opt_.pluginEffects) {
    if (pe.effect_type == typeId) plugin = &pe;  // registration overrides a standard id (D-31)
  }
  if (info == nullptr && plugin == nullptr) {
    if (report) {
      p_.WarnAt(bus, at,
                "unknown effect type '" + type +
                    "': not a standard effect (bank-format.md 6.4) and no plugin given to the build; "
                    "EFFX.stateBytes = 0 and the effect is not in DSPN",
                d_);
    }
    return 0;
  }
  if (info != nullptr && plugin == nullptr && report) {
    if (params.size() > info->paramCount) {
      p_.ErrorAt(bus, at,
                 "effect '" + type + "' takes " + std::to_string(info->paramCount) + " parameters, " +
                     std::to_string(params.size()) + " given",
                 d_);
    }
    for (std::size_t i = 0; i < params.size() && i < info->paramCount; ++i) {
      const dsp::EffectParamInfo& pi = info->params[i];
      if (!(params[i] >= pi.minValue && params[i] <= pi.maxValue)) {
        char range[96];
        std::snprintf(range, sizeof(range), "%g is outside %g..%g", static_cast<double>(params[i]),
                      static_cast<double>(pi.minValue), static_cast<double>(pi.maxValue));
        p_.WarnAt(bus, at,
                  "effect '" + type + "' parameter " + std::to_string(i) + " (" + pi.name + "): " + range +
                      " (the runtime clamps it)",
                  d_);
      }
    }
  }
  snd::EffectDesc desc{};
  desc.effect_type = typeId;
  desc.channels = channels;
  desc.max_frames = opt_.dspMaxFrames;
  desc.param_count = static_cast<std::uint32_t>(params.size());
  desc.initial_params = params.empty() ? nullptr : params.data();
  desc.blob = blob;
  desc.blob_bytes = blobBytes;
  desc.sample_rate = opt_.dspSampleRate;
  const std::uint32_t bytes = dsp::EffectInstanceBytes(desc, opt_.dspSampleRate, opt_.pluginEffects.data(),
                                                       static_cast<std::uint32_t>(opt_.pluginEffects.size()));
  if (bytes == 0 && report) {
    p_.ErrorAt(bus, at, "effect '" + type + "' cannot be sized (channels " + std::to_string(channels) + ")", d_);
  }
  return bytes;
}

bool Builder::ConvertAsset(const Object& a, const std::string& language, Converted* out) {
  detail::ConvertSettings s;
  std::string codec = "PC16";
  std::uint64_t rate = 0;
  std::uint64_t chans = 0;
  const Value* layers[4] = {platformDefaults_, platformSettings_, nullptr, nullptr};
  if (const Value* conv = a.doc.Find("convert"); conv != nullptr && conv->IsObject()) {
    layers[2] = conv->Find("default");
    layers[3] = conv->Find(CodecName(opt_.platform));
  }
  for (int i = 0; i < 4; ++i) {
    const Value* l = layers[i];
    if (l == nullptr || !l->IsObject()) continue;
    const Object& owner = i < 2 ? *p_.project() : a;
    codec = p_.Str(owner, *l, "codec", codec, d_);
    rate = p_.U64(owner, *l, "sampleRate", rate, d_);
    chans = p_.U64(owner, *l, "channels", chans, d_);
  }
  const std::uint32_t form = p_.Enum(a, a.doc, "loadForm", kLoadForm, 0, d_);
  if (!ParseCodec(codec, &s.codec)) {
    p_.ErrorAt(a, a.doc, "codec '" + codec + "' is not a four-character code", d_);
    return false;
  }
  const Encoder* encoder = enc_->Find(s.codec);
  if (encoder == nullptr) {
    p_.ErrorAt(a, a.doc,
               "asset '" + a.name + "': no encoder for codec '" + codec +
                   "' in this build (built in: PCMF, PC16, ADPC; Opus / custom codecs need an encoder registered with EncoderRegistry::Register)",
               d_);
    return false;
  }
  if (rate > 384000 || chans > 8) {
    p_.ErrorAt(a, a.doc, "sampleRate must be <= 384000 and channels <= 8", d_);
    return false;
  }
  s.sampleRate = static_cast<std::uint32_t>(rate);
  s.channels = static_cast<std::uint32_t>(chans);
  const std::uint64_t defSeek = form == static_cast<std::uint32_t>(bf::LoadForm::PcmResident) ? 0 : 100;
  s.seekIntervalMs = static_cast<std::uint32_t>(p_.U64(a, a.doc, "seekIntervalMs", defSeek, d_));
  if (const Value* lp = a.doc.Find("loop"); lp != nullptr && lp->IsObject()) {
    s.loop = true;
    s.loopStart = p_.U64(a, *lp, "start", 0, d_);
    s.loopEnd = p_.U64(a, *lp, "end", 0, d_);
    if (s.loopEnd <= s.loopStart) p_.ErrorAt(a, *lp, "loop needs start < end (frames, end exclusive)", d_);
  }
  // Source file (per language for voice assets).
  std::string file;
  const Value* fileV = a.doc.Find("file");
  const Value* langs = a.doc.Find("languages");
  if (langs != nullptr) {
    if (language.empty()) {
      p_.ErrorAt(a, *langs, "asset '" + a.name + "' has per-language files but is used by a non-language bank", d_);
      return false;
    }
    const Value* lf = langs->Find(language);
    if (lf == nullptr || !lf->IsString()) {
      p_.ErrorAt(a, *langs, "asset '" + a.name + "' has no file for language '" + language + "'", d_);
      return false;
    }
    fileV = lf;
  }
  if (fileV == nullptr || !fileV->IsString() || fileV->text.empty()) {
    p_.ErrorAt(a, a.doc, "asset '" + a.name + "' needs \"file\" (a WAV path relative to the project)", d_);
    return false;
  }
  file = fileV->text;
  std::vector<std::uint8_t> wav;
  if (!ReadBinary(fs::path(p_.root()) / file, &wav)) {
    p_.ErrorAt(a, *fileV, "cannot read '" + file + "'", d_);
    return false;
  }
  const std::uint64_t key = detail::CacheKey(wav, s, encoder->version);
  if (auto it = memCache_.find(key); it != memCache_.end()) {
    *out = it->second;
    return true;
  }
  if (detail::LoadCached(opt_.cacheDir, key, out)) {
    ++res_->assetsFromCache;
    memCache_.emplace(key, *out);
    return true;
  }
  std::string err;
  if (!detail::ConvertWav(wav, s, *encoder, out, &err)) {
    p_.ErrorAt(a, *fileV, "'" + file + "': " + err, d_);
    return false;
  }
  ++res_->assetsEncoded;
  detail::StoreCached(opt_.cacheDir, key, *out);
  memCache_.emplace(key, *out);
  return true;
}

void Builder::EmitAsset(BankTables& t, const Object& a, const std::string& language, const std::string& bankName) {
  Converted c;
  const std::uint32_t r = t.asst.AddRow();
  t.asst.U32(col::ASST_id, r, a.id);
  if (!ConvertAsset(a, language, &c)) return;
  const std::uint32_t form = p_.Enum(a, a.doc, "loadForm", kLoadForm, 0, d_);
  const EncodedAsset& e = c.enc;
  t.asst.U32(col::ASST_loadForm, r, form);
  t.asst.U32(col::ASST_codec, r, e.codec);
  t.asst.U32(col::ASST_channels, r, c.channels);
  t.asst.U32(col::ASST_sampleRate, r, c.sampleRate);
  t.asst.U64(col::ASST_totalFrames, r, e.totalFrames);
  t.asst.U32(col::ASST_preSkip, r, e.preSkip);
  t.asst.U32(col::ASST_preRoll, r, e.preRoll);
  t.asst.U32(col::ASST_framesPerPacket, r, e.framesPerPacket);
  t.asst.U32(col::ASST_blockAlign, r, e.blockAlign);
  t.asst.U64(col::ASST_loopStart, r, c.loopEnd != 0 ? c.loopStart : 0);
  t.asst.U64(col::ASST_loopEnd, r, c.loopEnd);
  std::uint32_t flags = 0;
  if (p_.Enum(a, a.doc, "stallPolicy", kStall, 0, d_) == 1) flags |= bf::kAssetStallSoundPriority;
  const std::uint32_t vr = p_.Enum(a, a.doc, "virtualReturn", kVirtualReturn, 0, d_);
  if (vr == 1) flags |= bf::kAssetVirtualRestart;
  if (vr == 2) flags |= bf::kAssetVirtualKill;
  const std::uint32_t io = p_.Enum(a, a.doc, "ioPriority", kIoPriority, 0, d_);
  if (io == 1) flags |= bf::kAssetPriorityMusic;
  if (io == 2) flags |= bf::kAssetPriorityVoice;
  t.asst.U32(col::ASST_flags, r, flags);
  // Seek table (stream and compressed forms).
  if (form != static_cast<std::uint32_t>(bf::LoadForm::PcmResident) && !e.seek.empty()) {
    t.asst.U32(col::ASST_seekFirst, r, t.seek.rows());
    t.asst.U32(col::ASST_seekCount, r, static_cast<std::uint32_t>(e.seek.size()));
    for (const SeekPoint& sp : e.seek) {
      const std::uint32_t sr = t.seek.AddRow();
      t.seek.U64(col::SEEK_frame, sr, sp.frame);
      t.seek.U64(col::SEEK_byteOffset, sr, sp.byteOffset);
    }
  }
  auto putExtra = [&]() {
    if (e.extra.empty()) return;
    t.wave.resize(Align64(t.wave.size()), 0);
    t.asst.U64(col::ASST_extraOffset, r, t.wave.size());
    t.asst.U32(col::ASST_extraBytes, r, static_cast<std::uint32_t>(e.extra.size()));
    t.wave.insert(t.wave.end(), e.extra.begin(), e.extra.end());
  };
  if (form != static_cast<std::uint32_t>(bf::LoadForm::Stream)) {
    t.wave.resize(Align64(t.wave.size()), 0);
    t.asst.U64(col::ASST_dataOffset, r, t.wave.size());
    t.asst.U64(col::ASST_dataBytes, r, e.data.size());
    t.wave.insert(t.wave.end(), e.data.begin(), e.data.end());
    putExtra();
    return;
  }
  // Stream: separate sector-aligned file (10.2) + preload chunk (6.1).
  putExtra();
  std::uint64_t sector = 2048;
  for (const Value* l : {platformDefaults_, platformSettings_}) {
    if (l != nullptr && l->IsObject()) sector = p_.U64(*p_.project(), *l, "sectorAlign", sector, d_);
  }
  if (sector < 64 || (sector & (sector - 1)) != 0 || sector > (1u << 20)) {
    p_.ErrorAt(*p_.project(), p_.project()->doc, "sectorAlign must be a power of two in [64, 1 MiB]", d_);
    return;
  }
  char idHex[16];
  std::snprintf(idHex, sizeof(idHex), "%08x", a.id);
  const std::string rel = "streams/" + FileSafe(a.name) + "_" + idHex + (language.empty() ? "" : "." + language) + ".snds";
  std::vector<std::uint8_t> file(sector, 0);
  const bf::StreamFileHeader sh{bf::kStreamMagic, bf::kStreamVersion, a.id, e.codec, c.channels, c.sampleRate,
                                e.totalFrames, sector, e.data.size(), static_cast<std::uint32_t>(sector), 0, 0};
  std::memcpy(file.data(), &sh, sizeof(sh));
  file.insert(file.end(), e.data.begin(), e.data.end());
  file.resize((file.size() + sector - 1) / sector * sector, 0);
  const std::uint32_t sr = t.strm.AddRow();
  t.strm.U32(col::STRM_pathOffset, sr, static_cast<std::uint32_t>(t.spth.size()));
  t.spth.insert(t.spth.end(), rel.begin(), rel.end());
  t.spth.push_back(0);
  t.strm.U64(col::STRM_fileBytes, sr, file.size());
  t.strm.U64(col::STRM_dataOffset, sr, sector);
  t.strm.U32(col::STRM_sectorAlign, sr, static_cast<std::uint32_t>(sector));
  t.asst.U32(col::ASST_stream, r, sr);
  bool already = false;
  for (const std::string& s : res_->streams) already = already || s == rel;
  if (!already) {
    res_->streams.push_back(rel);
    Stage(rel, std::move(file));
  }
  (void)bankName;
  const std::uint64_t preloadMs = p_.U64(a, a.doc, "preloadMs", 100, d_);
  if (preloadMs == 0 || e.data.empty()) return;
  const std::uint64_t want = (preloadMs * c.sampleRate + 999) / 1000;
  std::uint64_t frames = e.totalFrames;
  std::uint64_t bytes = e.data.size();
  if (e.framesPerPacket != 0 && e.blockAlign != 0) {
    const std::uint64_t packets = (want + e.framesPerPacket - 1) / e.framesPerPacket;
    if (packets * e.framesPerPacket < e.totalFrames) {
      frames = packets * e.framesPerPacket;
      bytes = packets * e.blockAlign;
    }
  } else {
    for (const SeekPoint& sp : e.seek) {
      if (sp.frame >= want && sp.frame > 0) {
        frames = sp.frame;
        bytes = sp.byteOffset;
        break;
      }
    }
  }
  if (bytes > 0xFFFFFFFFull || frames > 0xFFFFFFFFull) {
    p_.ErrorAt(a, a.doc, "preload larger than 4 GiB", d_);
    return;
  }
  t.prel.resize(Align64(t.prel.size()), 0);
  t.asst.U64(col::ASST_preloadOffset, r, t.prel.size());
  t.asst.U32(col::ASST_preloadBytes, r, static_cast<std::uint32_t>(bytes));
  t.asst.U32(col::ASST_preloadFrames, r, static_cast<std::uint32_t>(frames));
  t.prel.insert(t.prel.end(), e.data.begin(), e.data.begin() + static_cast<std::ptrdiff_t>(bytes));
}

// ---------------------------------------------------------------------------
// One bank (or one language variant)
// ---------------------------------------------------------------------------
void Builder::BuildBank(const Object& bank, const std::string& language) {
  BankTables t;
  const std::string bn = bank.name;
  auto inBank = [&](const Object* o) { return o->bank == bn; };
  // Events.
  std::vector<const Object*> events;
  for (const Object* e : p_.OfKind(Kind::Event)) {
    if (inBank(e)) events.push_back(e);
  }
  // Bank format 2.0 (§6.0): a nested event's row is after the event nesting it.
  {
    std::map<const Object*, std::vector<const Object*>> nests;
    const std::set<const Object*> here(events.begin(), events.end());
    Diagnostics quiet;  // reference errors are reported with the clips below
    for (const Object* e : events) {
      const Value* tracks = e->doc.Find("tracks");
      for (const Value& tr : (tracks != nullptr && tracks->IsArray()) ? tracks->items : EmptyArray().items) {
        const Value* clips = tr.Find("clips");
        for (const Value& c : (clips != nullptr && clips->IsArray()) ? clips->items : EmptyArray().items) {
          const Value* ev = c.Find("event");
          const Object* n = ev != nullptr ? p_.Resolve(*e, *ev, Kind::Event, quiet) : nullptr;
          if (n != nullptr && n != e && here.count(n) != 0) nests[e].push_back(n);
        }
      }
    }
    events = TopoOrder(events, nests, "nested event");
  }
  std::map<const Object*, std::uint32_t> eventRow;
  for (const Object* e : events) eventRow[e] = static_cast<std::uint32_t>(eventRow.size());

  // ---- buses: closure (parents, send targets), graph order (inputs first) ----
  std::set<const Object*> buses;
  std::vector<const Object*> work;
  auto needBus = [&](const Object* b) {
    if (b != nullptr && buses.insert(b).second) work.push_back(b);
  };
  auto busRef = [&](const Object& from, const Value* v) -> const Object* {
    if (v == nullptr || v->IsNull()) return nullptr;
    const Object* b = p_.Resolve(from, *v, Kind::Bus, d_);
    if (b != nullptr) CheckAccess(from, *v, *b, bn);
    return b;
  };
  std::map<const Object*, const Object*> eventBus;
  for (const Object* e : events) {
    const Value* bv = e->doc.Find("bus");
    const Object* b = bv != nullptr ? busRef(*e, bv) : MasterBus(*e, e->doc);
    eventBus[e] = b;
    needBus(b);
    if (const Value* tracks = e->doc.Find("tracks"); tracks != nullptr && tracks->IsArray()) {
      for (const Value& tr : tracks->items) needBus(busRef(*e, tr.Find("bus")));
    }
  }
  for (const Object* b : p_.OfKind(Kind::Bus)) {
    if (inBank(b)) needBus(b);
  }
  for (const Object* s : p_.OfKind(Kind::Snapshot)) {
    if (!inBank(s)) continue;
    if (const Value* vals = s->doc.Find("values"); vals != nullptr && vals->IsArray()) {
      for (const Value& v : vals->items) needBus(busRef(*s, v.Find("bus")));
    }
  }
  for (const Object* dk : p_.OfKind(Kind::Ducking)) {
    if (!inBank(dk)) continue;
    needBus(busRef(*dk, dk->doc.Find("target")));
    if (p_.Enum(*dk, dk->doc, "kind", kDuckKind, 0, d_) == 0) needBus(busRef(*dk, dk->doc.Find("source")));
  }
  std::map<const Object*, std::vector<const Object*>> outputs;  // bus -> buses it feeds
  while (!work.empty()) {
    const Object* b = work.back();
    work.pop_back();
    if (const Value* pv = b->doc.Find("parent"); pv != nullptr && !pv->IsNull()) {
      const Object* parent = busRef(*b, pv);
      if (parent != nullptr) {
        outputs[b].push_back(parent);
        needBus(parent);
      }
      if (p_.Bool(*b, b->doc, "master", false, d_)) p_.ErrorAt(*b, *pv, "the master bus cannot have a parent", d_);
    }
    if (const Value* sends = b->doc.Find("sends"); sends != nullptr && sends->IsArray()) {
      for (const Value& s : sends->items) {
        const Object* target = busRef(*b, s.Find("bus"));
        if (target == nullptr) {
          if (s.Find("bus") == nullptr) p_.ErrorAt(*b, s, "a send needs \"bus\"", d_);
          continue;
        }
        outputs[b].push_back(target);
        needBus(target);
      }
    }
  }
  // Kahn: a bus is placed once every bus feeding it is placed; ties by name.
  std::map<const Object*, int> indeg;
  for (const Object* b : buses) indeg[b] += 0;
  for (const auto& [b, outs] : outputs) {
    for (const Object* o : outs) ++indeg[o];
  }
  std::vector<const Object*> busOrder;
  auto byName = [](const Object* a, const Object* b) { return a->name < b->name; };
  std::vector<const Object*> ready;
  for (const auto& [b, n] : indeg) {
    if (n == 0) ready.push_back(b);
  }
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), byName);
    const Object* b = ready.front();
    ready.erase(ready.begin());
    busOrder.push_back(b);
    for (const Object* o : outputs[b]) {
      if (--indeg[o] == 0) ready.push_back(o);
    }
  }
  if (busOrder.size() != buses.size()) {
    for (const Object* b : buses) {
      if (indeg[b] > 0) {
        p_.ErrorAt(*b, b->doc, "bus graph cycle (parents / sends) through bus '" + b->name + "'", d_);
        break;
      }
    }
    return;
  }
  std::map<const Object*, std::uint32_t> busRow;
  for (const Object* b : busOrder) busRow[b] = static_cast<std::uint32_t>(busRow.size());
  std::map<const Object*, std::pair<std::uint32_t, std::uint32_t>> busEffects;  // first row, count
  std::map<const Object*, std::pair<std::uint32_t, std::uint32_t>> busSends;
  for (const Object* b : busOrder) {
    const std::uint32_t r = t.buss.AddRow();
    const Value& d = b->doc;
    t.buss.U32(col::BUSS_id, r, b->id);
    const Value* pv = d.Find("parent");
    const Object* parent = (pv != nullptr && !pv->IsNull()) ? p_.Resolve(*b, *pv, Kind::Bus, d_) : nullptr;
    t.buss.U32(col::BUSS_parent, r, parent != nullptr ? busRow[parent] : kNone);
    t.buss.F32(col::BUSS_volumeDb, r, static_cast<float>(p_.Num(*b, d, "volumeDb", 0, d_)));
    const std::uint64_t chans = p_.U64(*b, d, "channels", 0, d_);
    if (chans > SND_MAX_CHANNELS) p_.ErrorAt(*b, d, "bus channels must be 0 (parent's) .. 8", d_);
    t.buss.U32(col::BUSS_channels, r, static_cast<std::uint32_t>(chans));
    std::uint32_t flags = 0;
    if (p_.Bool(*b, d, "master", false, d_) || parent == nullptr) flags |= bf::kBusMaster;  // the root
    if (p_.Bool(*b, d, "output", false, d_)) flags |= bf::kBusOutput;
    if (p_.Bool(*b, d, "lodBypassable", false, d_)) flags |= bf::kBusLodBypassable;
    if (p_.Bool(*b, d, "meter", false, d_)) flags |= bf::kBusMeter;
    t.buss.U32(col::BUSS_flags, r, flags);
    t.buss.U32(col::BUSS_lodPriority, r, static_cast<std::uint32_t>(p_.U64(*b, d, "lodPriority", 0, d_)));
    // Sends.
    const std::uint32_t sendFirst = t.send.rows();
    if (const Value* sends = d.Find("sends"); sends != nullptr && sends->IsArray()) {
      for (const Value& s : sends->items) {
        const Value* tv = s.Find("bus");
        const Object* target = tv != nullptr ? p_.Resolve(*b, *tv, Kind::Bus, d_) : nullptr;
        if (target == nullptr) continue;
        const std::uint32_t sr = t.send.AddRow();
        t.send.U32(col::SEND_targetBus, sr, busRow[target]);
        t.send.F32(col::SEND_levelDb, sr, static_cast<float>(p_.Num(*b, s, "levelDb", 0, d_)));
        t.send.U32(col::SEND_preFader, sr, p_.Bool(*b, s, "preFader", false, d_) ? 1u : 0u);
      }
    }
    if (t.send.rows() > sendFirst) {
      t.buss.U32(col::BUSS_sendFirst, r, sendFirst);
      t.buss.U32(col::BUSS_sendCount, r, t.send.rows() - sendFirst);
    }
    busSends[b] = {sendFirst, t.send.rows() - sendFirst};
    // Effects (chain order).
    const std::uint32_t fxFirst = t.effx.rows();
    if (const Value* fx = d.Find("effects"); fx != nullptr) {
      if (!fx->IsArray()) p_.ErrorAt(*b, *fx, "\"effects\" must be an array", d_);
      for (const Value& e : fx->IsArray() ? fx->items : EmptyArray().items) {
        const std::string type = p_.Str(*b, e, "type", "", d_);
        if (type.empty()) {
          p_.ErrorAt(*b, e, "an effect needs \"type\"", d_);
          continue;
        }
        const std::uint32_t er = t.effx.AddRow();
        t.effx.U32(col::EFFX_effectType, er, HashId(type));
        std::vector<float> params;
        if (const Value* pv2 = e.Find("params"); pv2 != nullptr) {
          for (const Value& x : pv2->IsArray() ? pv2->items : EmptyArray().items) {
            double v = 0;
            if (!x.AsDouble(&v)) p_.ErrorAt(*b, x, "effect parameters are numbers", d_);
            params.push_back(static_cast<float>(v));
          }
        }
        if (!params.empty()) {
          t.effx.U32(col::EFFX_paramFirst, er, t.efpv.rows());
          t.effx.U32(col::EFFX_paramCount, er, static_cast<std::uint32_t>(params.size()));
          for (float v : params) t.efpv.F32(col::EFPV_value, t.efpv.AddRow(), v);
        }
        std::uint64_t blobBytes = 0;
        if (const Value* blob = e.Find("blob"); blob != nullptr) {
          std::vector<std::uint8_t> bytes;
          if (!blob->IsString() || !ReadBinary(fs::path(p_.root()) / blob->text, &bytes)) {
            p_.ErrorAt(*b, *blob, "cannot read effect blob", d_);
          } else {
            t.efbl.resize(Align64(t.efbl.size()), 0);
            t.effx.U64(col::EFFX_blobOffset, er, t.efbl.size());
            t.effx.U64(col::EFFX_blobBytes, er, bytes.size());
            t.efbl.insert(t.efbl.end(), bytes.begin(), bytes.end());
            blobBytes = bytes.size();
          }
        }
        // EFFX.stateBytes / DSPN (3.3): the dsp module's sizing, the same function the
        // runtime's dsp::ComputeDspPoolNeed uses (A1-11). Standard effects are
        // checked against dsp's table (A1-6, bank-format.md §6.4).
        const std::uint32_t fxType = HashId(type);
        const std::uint32_t fxChannels = BusChannels(*b);
        std::uint32_t state = 0;
        const auto fxIndex = static_cast<std::size_t>(&e - fx->items.data());
        const bool firstTime = effectChecked_.insert({b, fxIndex}).second;  // banks share buses: report once
        if (opt_.dsp.stateBytes != nullptr) {
          state = opt_.dsp.stateBytes(opt_.dsp.user, fxType, params.data(), static_cast<std::uint32_t>(params.size()),
                                      fxChannels);
        } else {
          state = DefaultEffectStateBytes(*b, e, type, fxType, params, fxChannels, firstTime,
                                          blobBytes, blobBytes != 0 ? t.efbl.data() + (t.efbl.size() - blobBytes) : nullptr);
        }
        t.effx.U32(col::EFFX_stateBytes, er, state);
        t.effx.U32(col::EFFX_flags, er, static_cast<std::uint32_t>(p_.U64(*b, e, "flags", 0, d_)));
        if (state != 0) {  // unsized (unknown) types are not in DSPN, as in dsp::ComputeDspPoolNeed
          t.dspBytes += Align64(state);
          ++t.dspCount;
        }
      }
    }
    if (t.effx.rows() > fxFirst) {
      t.buss.U32(col::BUSS_effectFirst, r, fxFirst);
      t.buss.U32(col::BUSS_effectCount, r, t.effx.rows() - fxFirst);
    }
    busEffects[b] = {fxFirst, t.effx.rows() - fxFirst};
    AddName(t, b->id, b->name, bf::NameKind::Bus);
    AddGuid(t, *b, bf::NameKind::Bus);
  }

  // ---- categories (closure over parents) ----
  std::set<const Object*> cats;
  std::map<const Object*, const Object*> eventCat;
  for (const Object* e : events) {
    if (const Value* cv = e->doc.Find("category"); cv != nullptr && !cv->IsNull()) {
      const Object* c = p_.Resolve(*e, *cv, Kind::Category, d_);
      if (c != nullptr) {
        CheckAccess(*e, *cv, *c, bn);
        cats.insert(c);
        eventCat[e] = c;
      }
    }
  }
  for (const Object* c : p_.OfKind(Kind::Category)) {
    if (inBank(c)) cats.insert(c);
  }
  for (const Object* dk : p_.OfKind(Kind::Ducking)) {
    if (inBank(dk) && p_.Enum(*dk, dk->doc, "kind", kDuckKind, 0, d_) == 1) {
      if (const Value* sv = dk->doc.Find("source"); sv != nullptr) {
        const Object* c = p_.Resolve(*dk, *sv, Kind::Category, d_);
        if (c != nullptr) cats.insert(c);
      }
    }
  }
  {
    std::vector<const Object*> cw(cats.begin(), cats.end());
    std::set<const Object*> visiting;
    while (!cw.empty()) {
      const Object* c = cw.back();
      cw.pop_back();
      if (const Value* pv = c->doc.Find("parent"); pv != nullptr && !pv->IsNull()) {
        const Object* parent = p_.Resolve(*c, *pv, Kind::Category, d_);
        if (parent != nullptr && cats.insert(parent).second) cw.push_back(parent);
      }
    }
  }
  std::vector<const Object*> catList(cats.begin(), cats.end());
  std::sort(catList.begin(), catList.end(), byName);
  {
    // Bank format 2.0 (§6.0): a category's parent row is after the category.
    std::map<const Object*, std::vector<const Object*>> up;
    Diagnostics quiet;
    for (const Object* c : catList) {
      const Value* pv = c->doc.Find("parent");
      const Object* parent = (pv != nullptr && !pv->IsNull()) ? p_.Resolve(*c, *pv, Kind::Category, quiet) : nullptr;
      if (parent != nullptr && parent != c) up[c].push_back(parent);
    }
    catList = TopoOrder(catList, up, "category parent");
  }
  std::map<const Object*, std::uint32_t> catRow;
  for (const Object* c : catList) catRow[c] = static_cast<std::uint32_t>(catRow.size());
  for (const Object* c : catList) {
    const std::uint32_t r = t.catg.AddRow();
    t.catg.U32(col::CATG_id, r, c->id);
    if (const Value* pv = c->doc.Find("parent"); pv != nullptr && !pv->IsNull()) {
      const Object* parent = p_.Resolve(*c, *pv, Kind::Category, d_);
      if (parent == c) p_.ErrorAt(*c, *pv, "a category cannot be its own parent", d_);
      if (parent != nullptr) t.catg.U32(col::CATG_parent, r, catRow[parent]);
    }
    std::uint32_t flags = 0;
    if (p_.Bool(*c, c->doc, "ignorePause", false, d_)) flags |= bf::kCategoryIgnorePause;
    if (p_.Bool(*c, c->doc, "muteOnFocusLoss", false, d_)) flags |= bf::kCategoryMuteOnFocusLoss;
    t.catg.U32(col::CATG_flags, r, flags);
    t.catg.F32(col::CATG_volumeDb, r, static_cast<float>(p_.Num(*c, c->doc, "volumeDb", 0, d_)));
    AddName(t, c->id, c->name, bf::NameKind::Category);
    AddGuid(t, *c, bf::NameKind::Category);
  }

  // ---- voice limit groups ----
  std::set<const Object*> groups;
  std::map<const Object*, const Object*> eventGroup;
  for (const Object* e : events) {
    if (const Value* gv = e->doc.Find("limitGroup"); gv != nullptr && !gv->IsNull()) {
      const Object* g = p_.Resolve(*e, *gv, Kind::VoiceLimitGroup, d_);
      if (g != nullptr) {
        CheckAccess(*e, *gv, *g, bn);
        groups.insert(g);
        eventGroup[e] = g;
      }
    }
  }
  for (const Object* g : p_.OfKind(Kind::VoiceLimitGroup)) {
    if (inBank(g)) groups.insert(g);
  }
  std::vector<const Object*> groupList(groups.begin(), groups.end());
  std::sort(groupList.begin(), groupList.end(), byName);
  std::map<const Object*, std::uint32_t> groupRow;
  for (const Object* g : groupList) {
    const std::uint32_t r = t.vlgr.AddRow();
    groupRow[g] = r;
    t.vlgr.U32(col::VLGR_id, r, g->id);
    if (g->doc.Find("maxVoices") == nullptr) p_.ErrorAt(*g, g->doc, "a voice limit group needs \"maxVoices\"", d_);
    t.vlgr.U32(col::VLGR_maxVoices, r, static_cast<std::uint32_t>(p_.U64(*g, g->doc, "maxVoices", 0, d_)));
    t.vlgr.U32(col::VLGR_stealMode, r, p_.Enum(*g, g->doc, "steal", kSteal, 0, d_));
    AddName(t, g->id, g->name, bf::NameKind::Other);
    AddGuid(t, *g, bf::NameKind::Other);
  }

  // ---- music objects referenced by music events ----
  std::set<const Object*> playlists;
  std::set<const Object*> segments;
  for (const Object* e : events) {
    const Value* m = e->doc.Find("music");
    if (m == nullptr || !m->IsObject()) continue;
    if (const Value* st = m->Find("states"); st != nullptr && st->IsArray()) {
      for (const Value& s : st->items) {
        if (const Value* pv = s.Find("playlist"); pv != nullptr) {
          const Object* pl = p_.Resolve(*e, *pv, Kind::Playlist, d_);
          if (pl != nullptr) playlists.insert(pl);
        }
      }
    }
    if (const Value* tr = m->Find("transitions"); tr != nullptr && tr->IsArray()) {
      for (const Value& x : tr->items) {
        if (const Value* bv = x.Find("bridge"); bv != nullptr && !bv->IsNull()) {
          const Object* sg = p_.Resolve(*e, *bv, Kind::Segment, d_);
          if (sg != nullptr) segments.insert(sg);
        }
      }
    }
  }
  for (const Object* pl : playlists) {
    if (const Value* en = pl->doc.Find("entries"); en != nullptr && en->IsArray()) {
      for (const Value& x : en->items) {
        if (const Value* sv = x.Find("segment"); sv != nullptr) {
          const Object* sg = p_.Resolve(*pl, *sv, Kind::Segment, d_);
          if (sg != nullptr) segments.insert(sg);
        }
      }
    }
  }
  std::vector<const Object*> segList(segments.begin(), segments.end());
  std::sort(segList.begin(), segList.end(), byName);
  std::vector<const Object*> plList(playlists.begin(), playlists.end());
  std::sort(plList.begin(), plList.end(), byName);

  // ---- assets (events' clips, synth sources, segments, stems) ----
  std::set<const Object*> assets;
  auto assetRef = [&](const Object& from, const Value* v) -> const Object* {
    if (v == nullptr || v->IsNull()) return nullptr;
    const Object* a = p_.Resolve(from, *v, Kind::Asset, d_);
    if (a != nullptr) {
      CheckAccess(from, *v, *a, bn);
      assets.insert(a);
    }
    return a;
  };
  for (const Object* e : events) {
    if (const Value* tracks = e->doc.Find("tracks"); tracks != nullptr && tracks->IsArray()) {
      for (const Value& tr : tracks->items) {
        if (const Value* clips = tr.Find("clips"); clips != nullptr && clips->IsArray()) {
          for (const Value& c : clips->items) {
            assetRef(*e, c.Find("asset"));
            if (const Value* syn = c.Find("synth"); syn != nullptr) assetRef(*e, syn->Find("asset"));
          }
        }
      }
    }
  }
  for (const Object* sg : segList) {
    if (sg->doc.Find("asset") == nullptr) p_.ErrorAt(*sg, sg->doc, "a segment needs \"asset\"", d_);
    assetRef(*sg, sg->doc.Find("asset"));
    if (const Value* stems = sg->doc.Find("stems"); stems != nullptr && stems->IsArray()) {
      for (const Value& s : stems->items) assetRef(*sg, s.Find("asset"));
    }
  }
  for (const Object* a : p_.OfKind(Kind::Asset)) {
    if (inBank(a)) assets.insert(a);
  }
  std::vector<const Object*> assetList(assets.begin(), assets.end());
  std::sort(assetList.begin(), assetList.end(), byName);
  std::map<const Object*, std::uint32_t> assetRow;
  for (const Object* a : assetList) {
    assetRow[a] = t.asst.rows();
    EmitAsset(t, *a, language, bn);
    AddName(t, a->id, a->name, bf::NameKind::Other);
    AddGuid(t, *a, bf::NameKind::Other);
  }
  std::set<const Object*> params;

  // ---- segments, playlists ----
  std::map<const Object*, std::uint32_t> segRow;
  for (const Object* sg : segList) {
    const std::uint32_t r = t.mseg.AddRow();
    segRow[sg] = r;
    const Value& d = sg->doc;
    const Object* a = p_.Resolve(*sg, d.Find("asset") != nullptr ? *d.Find("asset") : d, Kind::Asset, d_);
    const std::uint32_t ar = a != nullptr ? assetRow[a] : 0;
    // Source frames -> converted frames (the asset may be resampled).
    std::uint32_t rateNum = 1;
    std::uint32_t rateDen = 1;
    if (a != nullptr) {
      Converted c;
      if (ConvertAsset(*a, language, &c) && c.sourceRate != 0) {
        rateNum = c.sampleRate;
        rateDen = c.sourceRate;
      }
    }
    auto frames = [&](std::uint64_t f) { return f * rateNum / rateDen; };
    t.mseg.U32(col::MSEG_id, r, sg->id);
    t.mseg.U32(col::MSEG_asset, r, ar);
    t.mseg.U32(col::MSEG_tempoFirst, r, t.mtmp.rows());
    const Value* tempo = d.Find("tempo");
    if (tempo != nullptr && tempo->IsArray() && !tempo->items.empty()) {
      std::int64_t lastTick = -1;
      for (const Value& tp : tempo->items) {
        const std::uint32_t tr = t.mtmp.AddRow();
        std::int64_t tick = 0;
        if (const Value* tv = tp.Find("tick"); tv != nullptr && !tv->AsI64(&tick)) {
          p_.ErrorAt(*sg, *tv, "\"tick\" must be an integer (960 per beat)", d_);
        }
        if (tick <= lastTick) p_.ErrorAt(*sg, tp, "tempo points must have increasing ticks", d_);
        lastTick = tick;
        t.mtmp.I64(col::MTMP_tick, tr, tick);
        const double bpm = p_.Num(*sg, tp, "bpm", 120, d_);
        if (!(bpm > 0)) p_.ErrorAt(*sg, tp, "bpm must be > 0", d_);
        t.mtmp.F32(col::MTMP_bpm, tr, static_cast<float>(bpm));
        t.mtmp.U32(col::MTMP_beatsPerBar, tr, static_cast<std::uint32_t>(p_.U64(*sg, tp, "beatsPerBar", 4, d_)));
        t.mtmp.U32(col::MTMP_beatUnit, tr, static_cast<std::uint32_t>(p_.U64(*sg, tp, "beatUnit", 4, d_)));
      }
    } else {
      const std::uint32_t tr = t.mtmp.AddRow();
      t.mtmp.I64(col::MTMP_tick, tr, 0);
      t.mtmp.F32(col::MTMP_bpm, tr, 120.0f);
    }
    t.mseg.U32(col::MSEG_tempoCount, r, t.mtmp.rows() - t.mseg.GetU32(col::MSEG_tempoFirst, r));
    const std::uint64_t bars = p_.U64(*sg, d, "bars", 0, d_);
    if (bars == 0) p_.ErrorAt(*sg, d, "a segment needs \"bars\" >= 1", d_);
    t.mseg.U32(col::MSEG_bars, r, static_cast<std::uint32_t>(bars));
    const std::uint64_t entry = p_.U64(*sg, d, "entryFrame", 0, d_);
    t.mseg.U64(col::MSEG_entryFrame, r, frames(entry));
    t.mseg.U64(col::MSEG_exitFrame, r, frames(p_.U64(*sg, d, "exitFrame", 0, d_)));
    if (const Value* mk = d.Find("markers"); mk != nullptr && mk->IsArray() && !mk->items.empty()) {
      t.mseg.U32(col::MSEG_markerFirst, r, t.mmrk.rows());
      for (const Value& m : mk->items) {
        const std::string mn = p_.Str(*sg, m, "name", "", d_);
        if (mn.empty()) p_.ErrorAt(*sg, m, "a marker needs \"name\"", d_);
        const std::uint32_t mr = t.mmrk.AddRow();
        t.mmrk.U32(col::MMRK_id, mr, HashId(mn));
        t.mmrk.U64(col::MMRK_frame, mr, frames(p_.U64(*sg, m, "frame", 0, d_)));
        t.mmrk.U32(col::MMRK_kind, mr, p_.Enum(*sg, m, "kind", kMarkerKind, 0, d_));
        AddName(t, HashId(mn), mn, bf::NameKind::Other);
      }
      t.mseg.U32(col::MSEG_markerCount, r, static_cast<std::uint32_t>(mk->items.size()));
    }
    if (p_.Bool(*sg, d, "entryPreload", false, d_)) {
      // Format 1.0 has one preload range per asset, at its head.
      const bool stream = a != nullptr && p_.Enum(*a, a->doc, "loadForm", kLoadForm, 0, d_) == 3;
      if (stream && entry == 0) {
        t.mseg.U32(col::MSEG_flags, r, 1u);
      } else {
        p_.WarnAt(*sg, *d.Find("entryPreload"),
                  "entry preload needs a stream asset with entryFrame 0 in bank format 1.0 (ignored)", d_);
      }
    }
    AddName(t, sg->id, sg->name, bf::NameKind::Other);
    AddGuid(t, *sg, bf::NameKind::Other);
  }
  // Stems after all segments (they reference segment rows).
  for (const Object* sg : segList) {
    const Value* stems = sg->doc.Find("stems");
    if (stems == nullptr || !stems->IsArray()) continue;
    for (const Value& s : stems->items) {
      const std::uint32_t r = t.mstm.AddRow();
      t.mstm.U32(col::MSTM_segment, r, segRow[sg]);
      const std::uint64_t first = p_.U64(*sg, s, "channelFirst", 0, d_);
      const std::uint64_t count = p_.U64(*sg, s, "channelCount", 2, d_);
      {
        // The stem's channels must exist in its (converted) asset (bank-format.md §8 MSTM).
        const Value* av = s.Find("asset");
        const Value* sv = sg->doc.Find("asset");
        const Value* ref = av != nullptr ? av : sv;
        const Object* a = ref != nullptr ? p_.Resolve(*sg, *ref, Kind::Asset, d_) : nullptr;
        Converted c;
        if (a != nullptr && ConvertAsset(*a, language, &c) && (first > c.channels || count > c.channels - first)) {
          p_.ErrorAt(*sg, s,
                     "stem channels [" + std::to_string(first) + ", +" + std::to_string(count) + ") exceed the " +
                         std::to_string(c.channels) + " channels of asset '" + a->name + "' (after conversion)",
                     d_);
        }
      }
      t.mstm.U32(col::MSTM_channelFirst, r, static_cast<std::uint32_t>(first));
      t.mstm.U32(col::MSTM_channelCount, r, static_cast<std::uint32_t>(count));
      if (const Value* cv = s.Find("volumeCurve"); cv != nullptr) {
        t.mstm.U32(col::MSTM_volumeCurve, r, EmitCurve(t, *sg, *cv, false, 0, &params));
      }
      if (const Value* av = s.Find("asset"); av != nullptr) {
        const Object* a = p_.Resolve(*sg, *av, Kind::Asset, d_);
        if (a != nullptr) t.mstm.U32(col::MSTM_asset, r, assetRow[a]);
      }
    }
  }
  std::map<const Object*, std::uint32_t> plRow;
  for (const Object* pl : plList) {
    const std::uint32_t r = t.mpls.AddRow();
    plRow[pl] = r;
    t.mpls.U32(col::MPLS_id, r, pl->id);
    t.mpls.U32(col::MPLS_entryFirst, r, t.mple.rows());
    const Value* en = pl->doc.Find("entries");
    if (en == nullptr || !en->IsArray() || en->items.empty()) p_.ErrorAt(*pl, pl->doc, "a playlist needs \"entries\"", d_);
    for (const Value& x : (en != nullptr && en->IsArray()) ? en->items : EmptyArray().items) {
      const Value* sv = x.Find("segment");
      const Object* sg = sv != nullptr ? p_.Resolve(*pl, *sv, Kind::Segment, d_) : nullptr;
      if (sg == nullptr) {
        if (sv == nullptr) p_.ErrorAt(*pl, x, "a playlist entry needs \"segment\"", d_);
        continue;
      }
      const std::uint32_t er = t.mple.AddRow();
      t.mple.U32(col::MPLE_segment, er, segRow[sg]);
      t.mple.F32(col::MPLE_weight, er, static_cast<float>(p_.Num(*pl, x, "weight", 1, d_)));
      t.mple.U32(col::MPLE_repeat, er, static_cast<std::uint32_t>(p_.U64(*pl, x, "repeat", 1, d_)));
    }
    t.mpls.U32(col::MPLS_entryCount, r, t.mple.rows() - t.mpls.GetU32(col::MPLS_entryFirst, r));
    t.mpls.U32(col::MPLS_mode, r, p_.Enum(*pl, pl->doc, "mode", kPlaylistMode, 0, d_));
    t.mpls.U32(col::MPLS_loopCount, r, static_cast<std::uint32_t>(p_.U64(*pl, pl->doc, "loopCount", 0, d_)));
    AddName(t, pl->id, pl->name, bf::NameKind::Other);
    AddGuid(t, *pl, bf::NameKind::Other);
  }

  // ---- events ----
  for (std::size_t i = 0; i < events.size(); ++i) t.evnt.AddRow();
  for (const Object* e : events) {
    const std::uint32_t r = eventRow[e];
    const Value& d = e->doc;
    t.evnt.U32(col::EVNT_id, r, e->id);
    t.evnt.U32(col::EVNT_bus, r, eventBus[e] != nullptr ? busRow[eventBus[e]] : 0);
    if (eventCat.count(e) != 0) t.evnt.U32(col::EVNT_category, r, catRow[eventCat[e]]);
    if (eventGroup.count(e) != 0) t.evnt.U32(col::EVNT_limitGroup, r, groupRow[eventGroup[e]]);
    t.evnt.U32(col::EVNT_sequenceKind, r, p_.Enum(*e, d, "sequence", kSequence, 0, d_));
    const std::uint64_t prio = p_.U64(*e, d, "priority", 128, d_);
    if (prio > 255) p_.ErrorAt(*e, *d.Find("priority"), "priority must be 0..255", d_);
    t.evnt.U32(col::EVNT_priority, r, static_cast<std::uint32_t>(prio));
    t.evnt.U32(col::EVNT_maxInstances, r, static_cast<std::uint32_t>(p_.U64(*e, d, "maxInstances", 0, d_)));
    t.evnt.F32(col::EVNT_retriggerMs, r, static_cast<float>(p_.Num(*e, d, "retriggerMs", 0, d_)));
    t.evnt.F32(col::EVNT_fadeOutMs, r, static_cast<float>(p_.Num(*e, d, "fadeOutMs", 0, d_)));
    t.evnt.F32(col::EVNT_comboWindowMs, r, static_cast<float>(p_.Num(*e, d, "comboWindowMs", 0, d_)));
    t.evnt.F32(col::EVNT_maxDistance, r, static_cast<float>(p_.Num(*e, d, "maxDistance", 0, d_)));
    t.evnt.U32(col::EVNT_virtualReturn, r, p_.Enum(*e, d, "virtualReturn", kVirtualReturn, 0, d_));
    if (const Value* cone = d.Find("cone"); cone != nullptr) {
      t.evnt.F32(col::EVNT_coneInnerDeg, r, static_cast<float>(p_.Num(*e, *cone, "innerDeg", 0, d_)));
      t.evnt.F32(col::EVNT_coneOuterDeg, r, static_cast<float>(p_.Num(*e, *cone, "outerDeg", 0, d_)));
      t.evnt.F32(col::EVNT_coneOuterDb, r, static_cast<float>(p_.Num(*e, *cone, "outerDb", 0, d_)));
    }
    std::uint32_t flags = 0;
    if (p_.Bool(*e, d, "is3D", false, d_)) flags |= bf::kEvent3D;
    if (p_.Bool(*e, d, "ignorePause", false, d_)) flags |= bf::kEventIgnorePause;
    if (p_.Bool(*e, d, "stinger", false, d_)) flags |= bf::kEventStinger;
    // Distance curves (4.5).
    if (const Value* dc = d.Find("distanceCurves"); dc != nullptr && dc->IsObject()) {
      static const char* const keys[] = {"volume", "lowpass", "spread", "reverbSend", "blend3D"};
      static const std::uint16_t cols[] = {col::EVNT_curveVolume, col::EVNT_curveLowpass, col::EVNT_curveSpread,
                                           col::EVNT_curveReverbSend, col::EVNT_curveBlend3D};
      static const std::uint32_t targets[] = {0, 2, 7, 8, 9};
      for (const auto& m : dc->members) {
        if (m.first == "notes") continue;  // free-form at every level
        int k = -1;
        for (int j = 0; j < 5; ++j) k = m.first == keys[j] ? j : k;
        if (k < 0) {
          p_.ErrorAt(*e, m.second, "unknown distance curve \"" + m.first + "\" (volume, lowpass, spread, reverbSend, blend3D)", d_);
          continue;
        }
        t.evnt.U32(cols[k], r, EmitCurve(t, *e, m.second, true, targets[k], &params));
      }
    }
    // Parameter curves.
    if (const Value* cv = d.Find("curves"); cv != nullptr && cv->IsArray() && !cv->items.empty()) {
      const std::uint32_t first = t.curv.rows();
      for (const Value& c : cv->items) EmitCurve(t, *e, c, false, 0, &params);
      t.evnt.U32(col::EVNT_curveFirst, r, first);
      t.evnt.U32(col::EVNT_curveCount, r, t.curv.rows() - first);
    }
    // Tracks, clips, actions.
    const Value* tracks = d.Find("tracks");
    const std::uint32_t trackFirst = t.trak.rows();
    for (const Value& tr : (tracks != nullptr && tracks->IsArray()) ? tracks->items : EmptyArray().items) {
      const std::uint32_t tk = t.trak.AddRow();
      t.trak.U32(col::TRAK_event, tk, r);
      t.trak.U32(col::TRAK_flags, tk, static_cast<std::uint32_t>(p_.U64(*e, tr, "flags", 0, d_)));
      if (const Value* bv = tr.Find("bus"); bv != nullptr && !bv->IsNull()) {
        const Object* b = p_.Resolve(*e, *bv, Kind::Bus, d_);
        if (b != nullptr) t.trak.U32(col::TRAK_bus, tk, busRow[b]);
      }
      if (const Value* lp = tr.Find("layerParameter"); lp != nullptr && !lp->IsNull()) {
        const Object* param = p_.Resolve(*e, *lp, Kind::Parameter, d_);
        if (param != nullptr) {
          t.trak.U32(col::TRAK_layerParam, tk, param->id);
          params.insert(param);
        }
      }
      const std::uint32_t clipFirst = t.clip.rows();
      const Value* clips = tr.Find("clips");
      for (const Value& c : (clips != nullptr && clips->IsArray()) ? clips->items : EmptyArray().items) {
        const Value* av = c.Find("asset");
        const Value* ev = c.Find("event");
        const Value* sv = c.Find("synth");
        if ((av != nullptr) + (ev != nullptr) + (sv != nullptr) != 1) {
          p_.ErrorAt(*e, c, "a clip has exactly one of \"asset\", \"event\" or \"synth\"", d_);
          continue;
        }
        const std::uint32_t cr = t.clip.AddRow();
        if (av != nullptr) {
          const Object* a = p_.Resolve(*e, *av, Kind::Asset, d_);
          t.clip.U32(col::CLIP_kind, cr, 0);
          if (a != nullptr) {
            t.clip.U32(col::CLIP_target, cr, assetRow[a]);
            if (p_.Enum(*a, a->doc, "loadForm", kLoadForm, 0, d_) == 3 && p_.U64(*a, a->doc, "preloadMs", 100, d_) == 0) {
              flags |= bf::kEventNeedsPriming;
            }
          }
        } else if (ev != nullptr) {
          const Object* n = p_.Resolve(*e, *ev, Kind::Event, d_);
          t.clip.U32(col::CLIP_kind, cr, 1);
          if (n != nullptr && eventRow.count(n) == 0) {
            p_.ErrorAt(*e, *ev, "nested event '" + n->name + "' must be in the same bank ('" + bn + "')", d_);
          } else if (n != nullptr) {
            t.clip.U32(col::CLIP_target, cr, eventRow[n]);
          }
        } else {
          const std::uint32_t kind = p_.Enum(*e, *sv, "kind", kSynthKind, 0, d_);
          if (kind == 3) p_.ErrorAt(*e, *sv, "patch synthesis is stage 6 (PTCH layout not defined in bank format 1.0)", d_);
          const std::uint32_t sr = t.synp.AddRow();
          t.synp.U32(col::SYNP_kind, sr, kind);
          t.synp.U32(col::SYNP_waveform, sr, p_.Enum(*e, *sv, "waveform", kWaveform, 0, d_));
          t.synp.F32(col::SYNP_frequencyHz, sr, static_cast<float>(p_.Num(*e, *sv, "frequencyHz", 440, d_)));
          t.synp.F32(col::SYNP_duty, sr, static_cast<float>(p_.Num(*e, *sv, "duty", 0.5, d_)));
          t.synp.U32(col::SYNP_noiseColor, sr, p_.Enum(*e, *sv, "noiseColor", kNoise, 0, d_));
          t.synp.U32(col::SYNP_channels, sr, static_cast<std::uint32_t>(p_.U64(*e, *sv, "channels", 1, d_)));
          if (const Value* sa = sv->Find("asset"); sa != nullptr) {
            const Object* a = p_.Resolve(*e, *sa, Kind::Asset, d_);
            if (a != nullptr) t.synp.U32(col::SYNP_asset, sr, assetRow[a]);
          } else if (kind == 2 || kind == 4) {
            p_.ErrorAt(*e, *sv, "granular / wavetable synthesis needs \"asset\"", d_);
          }
          t.clip.U32(col::CLIP_kind, cr, 2);
          t.clip.U32(col::CLIP_target, cr, sr);
        }
        t.clip.F32(col::CLIP_startMs, cr, static_cast<float>(p_.Num(*e, c, "startMs", 0, d_)));
        t.clip.F32(col::CLIP_weight, cr, static_cast<float>(p_.Num(*e, c, "weight", 1, d_)));
        t.clip.F32(col::CLIP_volumeDb, cr, static_cast<float>(p_.Num(*e, c, "volumeDb", 0, d_)));
        t.clip.F32(col::CLIP_pitchCents, cr, static_cast<float>(p_.Num(*e, c, "pitchCents", 0, d_)));
        const Value& rnd = c.Find("random") != nullptr ? *c.Find("random") : Empty();
        t.clip.F32(col::CLIP_randVolumeDb, cr, static_cast<float>(p_.Num(*e, rnd, "volumeDb", 0, d_)));
        t.clip.F32(col::CLIP_randPitchCents, cr, static_cast<float>(p_.Num(*e, rnd, "pitchCents", 0, d_)));
        t.clip.F32(col::CLIP_randPan, cr, static_cast<float>(p_.Num(*e, rnd, "pan", 0, d_)));
        t.clip.F32(col::CLIP_randStartMs, cr, static_cast<float>(p_.Num(*e, rnd, "startMs", 0, d_)));
        t.clip.F32(col::CLIP_randFilterHz, cr, static_cast<float>(p_.Num(*e, rnd, "filterHz", 0, d_)));
        if (const Value* sw = c.Find("switchValue"); sw != nullptr) {
          std::uint64_t n = 0;
          if (sw->IsString()) {
            t.clip.U32(col::CLIP_switchValue, cr, HashId(sw->text));
          } else if (sw->AsU64(&n) && n <= 0xFFFFFFFFu) {
            t.clip.U32(col::CLIP_switchValue, cr, static_cast<std::uint32_t>(n));
          } else {
            p_.ErrorAt(*e, *sw, "\"switchValue\" is a name or a 32-bit integer", d_);
          }
        }
        if (const Value* ly = c.Find("layer"); ly != nullptr) {
          double lo = 0, hi = 0;
          if (!ly->IsArray() || ly->items.size() != 2 || !ly->items[0].AsDouble(&lo) || !ly->items[1].AsDouble(&hi) ||
              lo > hi) {
            p_.ErrorAt(*e, *ly, "\"layer\" is [min, max] with min <= max", d_);
          }
          t.clip.F32(col::CLIP_layerMin, cr, static_cast<float>(lo));
          t.clip.F32(col::CLIP_layerMax, cr, static_cast<float>(hi));
        }
        t.clip.F32(col::CLIP_fadeInMs, cr, static_cast<float>(p_.Num(*e, c, "fadeInMs", 0, d_)));
        t.clip.F32(col::CLIP_fadeOutMs, cr, static_cast<float>(p_.Num(*e, c, "fadeOutMs", 0, d_)));
        t.clip.U32(col::CLIP_loopCount, cr, static_cast<std::uint32_t>(p_.U64(*e, c, "loopCount", 1, d_)));
      }
      t.trak.U32(col::TRAK_clipFirst, tk, clipFirst);
      t.trak.U32(col::TRAK_clipCount, tk, t.clip.rows() - clipFirst);
      const Value* acts = tr.Find("actions");
      if (acts != nullptr && acts->IsArray() && !acts->items.empty()) {
        t.trak.U32(col::TRAK_actionFirst, tk, t.actn.rows());
        for (const Value& a : acts->items) {
          const std::uint32_t ar = t.actn.AddRow();
          const std::uint32_t kind = p_.Enum(*e, a, "action", kAction, 0, d_);
          t.actn.F32(col::ACTN_timeMs, ar, static_cast<float>(p_.Num(*e, a, "timeMs", 0, d_)));
          t.actn.U32(col::ACTN_action, ar, kind);
          t.actn.F32(col::ACTN_value, ar, static_cast<float>(p_.Num(*e, a, "value", 0, d_)));
          const Kind targetKind = kind <= 1 ? Kind::Event : (kind == 2 ? Kind::Parameter : Kind::Snapshot);
          const Value* tv = a.Find("target");
          const Object* target = tv != nullptr ? p_.Resolve(*e, *tv, targetKind, d_) : nullptr;
          if (tv == nullptr) p_.ErrorAt(*e, a, "an action needs \"target\"", d_);
          if (target != nullptr) {
            CheckAccess(*e, *tv, *target, bn);  // runtime-id references follow the bank rule too (§3)
            t.actn.U32(col::ACTN_target, ar, target->id);
            if (targetKind == Kind::Parameter) params.insert(target);
          }
        }
        t.trak.U32(col::TRAK_actionCount, tk, static_cast<std::uint32_t>(acts->items.size()));
      }
    }
    if (t.trak.rows() > trackFirst) {
      t.evnt.U32(col::EVNT_trackFirst, r, trackFirst);
      t.evnt.U32(col::EVNT_trackCount, r, t.trak.rows() - trackFirst);
    }
    // Music (8.3, 8.5).
    if (const Value* m = d.Find("music"); m != nullptr) {
      flags |= bf::kEventMusic;
      std::map<std::string, std::uint32_t> stateRow;
      const Value* states = m->Find("states");
      if (states == nullptr || !states->IsArray() || states->items.empty()) {
        p_.ErrorAt(*e, *m, "a music event needs \"states\"", d_);
      }
      for (const Value& s : (states != nullptr && states->IsArray()) ? states->items : EmptyArray().items) {
        const std::string sn = p_.Str(*e, s, "name", "", d_);
        if (sn.empty() || sn == "*") {
          p_.ErrorAt(*e, s, "a music state needs a \"name\" (not \"*\")", d_);
          continue;
        }
        if (stateRow.count(sn) != 0) p_.ErrorAt(*e, s, "duplicate music state '" + sn + "'", d_);
        const std::uint32_t sr = t.msta.AddRow();
        stateRow[sn] = sr;
        t.msta.U32(col::MSTA_id, sr, HashId(sn));
        t.msta.U32(col::MSTA_musicEvent, sr, r);
        const Value* pv = s.Find("playlist");
        const Object* pl = pv != nullptr ? p_.Resolve(*e, *pv, Kind::Playlist, d_) : nullptr;
        if (pv == nullptr) p_.ErrorAt(*e, s, "a music state needs \"playlist\"", d_);
        if (pl != nullptr) t.msta.U32(col::MSTA_playlist, sr, plRow[pl]);
        AddName(t, HashId(sn), sn, bf::NameKind::State);
      }
      const std::string initial = p_.Str(*e, *m, "initialState", "", d_);
      if (!initial.empty()) {
        if (stateRow.count(initial) == 0) {
          p_.ErrorAt(*e, *m->Find("initialState"), "initialState '" + initial + "' is not a state of this event", d_);
        } else {
          t.evnt.U32(col::EVNT_musicState, r, stateRow[initial]);
        }
      } else if (!stateRow.empty()) {
        t.evnt.U32(col::EVNT_musicState, r, 0 + t.msta.rows() - static_cast<std::uint32_t>(stateRow.size()));
      }
      // Transition rule table, in match order; warn about unreachable rows (8.5).
      struct RuleKey {
        std::string from, to;
      };
      std::vector<RuleKey> seen;
      const Value* trs = m->Find("transitions");
      for (const Value& x : (trs != nullptr && trs->IsArray()) ? trs->items : EmptyArray().items) {
        const std::string from = p_.Str(*e, x, "from", "*", d_);
        const std::string to = p_.Str(*e, x, "to", "*", d_);
        for (const std::string* s : {&from, &to}) {
          if (*s != "*" && stateRow.count(*s) == 0) p_.ErrorAt(*e, x, "transition state '" + *s + "' is not a state of this event", d_);
        }
        for (const RuleKey& k : seen) {
          if ((k.from == "*" || k.from == from) && (k.to == "*" || k.to == to)) {
            p_.WarnAt(*e, x,
                      "transition " + from + " -> " + to + " can never match: an earlier rule " + k.from + " -> " +
                          k.to + " covers it",
                      d_);
            break;
          }
        }
        seen.push_back(RuleKey{from, to});
        const std::uint32_t tr = t.mtrn.AddRow();
        t.mtrn.U32(col::MTRN_musicEvent, tr, r);
        t.mtrn.U32(col::MTRN_fromState, tr, from == "*" ? 0u : HashId(from));
        t.mtrn.U32(col::MTRN_toState, tr, to == "*" ? 0u : HashId(to));
        t.mtrn.U32(col::MTRN_timing, tr, p_.Enum(*e, x, "timing", kTiming, 2, d_));
        t.mtrn.U32(col::MTRN_timingCount, tr, static_cast<std::uint32_t>(p_.U64(*e, x, "timingCount", 0, d_)));
        t.mtrn.F32(col::MTRN_fadeOutBeats, tr, static_cast<float>(p_.Num(*e, x, "fadeOutBeats", 0, d_)));
        t.mtrn.F32(col::MTRN_fadeInBeats, tr, static_cast<float>(p_.Num(*e, x, "fadeInBeats", 0, d_)));
        if (const Value* bv = x.Find("bridge"); bv != nullptr && !bv->IsNull()) {
          const Object* sg = p_.Resolve(*e, *bv, Kind::Segment, d_);
          if (sg != nullptr) t.mtrn.U32(col::MTRN_bridgeSegment, tr, segRow[sg]);
        }
        t.mtrn.U32(col::MTRN_startPosition, tr, p_.Enum(*e, x, "start", kStartPos, 0, d_));
        const std::string marker = p_.Str(*e, x, "startMarker", "", d_);
        if (!marker.empty()) t.mtrn.U32(col::MTRN_startMarker, tr, HashId(marker));
        t.mtrn.U32(col::MTRN_interrupt, tr, p_.Enum(*e, x, "interrupt", kInterrupt, 0, d_));
        t.mtrn.U32(col::MTRN_lateRule, tr, p_.Enum(*e, x, "late", kLate, 0, d_));
        t.mtrn.U32(col::MTRN_flags, tr, p_.Bool(*e, x, "prepare", false, d_) ? 1u : 0u);
      }
      const Value* stingers = m->Find("stingers");
      for (const Value& s : (stingers != nullptr && stingers->IsArray()) ? stingers->items : EmptyArray().items) {
        const std::string sn = p_.Str(*e, s, "name", "", d_);
        const Value* ev = s.Find("event");
        const Object* se = ev != nullptr ? p_.Resolve(*e, *ev, Kind::Event, d_) : nullptr;
        if (sn.empty() || ev == nullptr) p_.ErrorAt(*e, s, "a stinger needs \"name\" and \"event\"", d_);
        if (se != nullptr && eventRow.count(se) == 0) {
          p_.ErrorAt(*e, *ev, "stinger event '" + se->name + "' must be in the same bank ('" + bn + "')", d_);
          continue;
        }
        const std::uint32_t sr = t.mstg.AddRow();
        t.mstg.U32(col::MSTG_id, sr, HashId(sn));
        if (se != nullptr) t.mstg.U32(col::MSTG_event, sr, eventRow[se]);
        t.mstg.U32(col::MSTG_timing, sr, p_.Enum(*e, s, "timing", kTiming, 1, d_));
        t.mstg.U32(col::MSTG_timingCount, sr, static_cast<std::uint32_t>(p_.U64(*e, s, "timingCount", 0, d_)));
        t.mstg.U32(col::MSTG_overlap, sr, p_.Enum(*e, s, "overlap", kOverlap, 0, d_));
        AddName(t, HashId(sn), sn, bf::NameKind::Other);
      }
    }
    t.evnt.U32(col::EVNT_flags, r, flags);
    AddName(t, e->id, e->name, bf::NameKind::Event);
    AddGuid(t, *e, bf::NameKind::Event);
  }

  // ---- parameters (referenced + assigned) ----
  for (const Object* prm : p_.OfKind(Kind::Parameter)) {
    if (inBank(prm)) params.insert(prm);
  }
  std::vector<const Object*> paramList(params.begin(), params.end());
  std::sort(paramList.begin(), paramList.end(), byName);
  for (const Object* prm : paramList) {
    const std::uint32_t r = t.parm.AddRow();
    const Value& d = prm->doc;
    t.parm.U32(col::PARM_id, r, prm->id);
    t.parm.U32(col::PARM_kind, r, p_.Enum(*prm, d, "kind", kParamKind, 0, d_));
    std::uint32_t mask = 7;
    if (const Value* sc = d.Find("scope"); sc != nullptr) {
      mask = 0;
      static const char* const scopes[] = {"global", "emitter", "playback", nullptr};
      for (const Value& s : sc->IsArray() ? sc->items : EmptyArray().items) {
        bool ok = false;
        for (std::uint32_t i = 0; scopes[i] != nullptr; ++i) {
          if (s.IsString() && s.text == scopes[i]) {
            mask |= 1u << i;
            ok = true;
          }
        }
        if (!ok) p_.ErrorAt(*prm, s, "scope entries are \"global\", \"emitter\" or \"playback\"", d_);
      }
      if (mask == 0) p_.ErrorAt(*prm, *sc, "\"scope\" needs at least one scope", d_);
    }
    t.parm.U32(col::PARM_scopeMask, r, mask);
    const double lo = p_.Num(*prm, d, "min", 0, d_);
    const double hi = p_.Num(*prm, d, "max", 1, d_);
    const double def = p_.Num(*prm, d, "default", lo, d_);
    if (lo > hi || def < lo || def > hi) p_.ErrorAt(*prm, d, "parameter needs min <= default <= max", d_);
    t.parm.F32(col::PARM_minValue, r, static_cast<float>(lo));
    t.parm.F32(col::PARM_maxValue, r, static_cast<float>(hi));
    t.parm.F32(col::PARM_defaultValue, r, static_cast<float>(def));
    t.parm.F32(col::PARM_rampRate, r, static_cast<float>(p_.Num(*prm, d, "rampRate", 0, d_)));
    AddName(t, prm->id, prm->name, bf::NameKind::Param);
    AddGuid(t, *prm, bf::NameKind::Param);
  }

  // ---- snapshots, ducking ----
  for (const Object* s : p_.OfKind(Kind::Snapshot)) {
    if (!inBank(s)) continue;
    const std::uint32_t r = t.snap.AddRow();
    t.snap.U32(col::SNAP_id, r, s->id);
    t.snap.U32(col::SNAP_valueFirst, r, t.snpv.rows());
    const Value* vals = s->doc.Find("values");
    for (const Value& v : (vals != nullptr && vals->IsArray()) ? vals->items : EmptyArray().items) {
      const std::uint32_t kind = p_.Enum(*s, v, "kind", kSnapKind, 0, d_);
      const Value* bv = v.Find("bus");
      const Object* b = bv != nullptr ? p_.Resolve(*s, *bv, Kind::Bus, d_) : nullptr;
      if (bv == nullptr) p_.ErrorAt(*s, v, "a snapshot value needs \"bus\"", d_);
      if (b == nullptr) continue;
      std::uint32_t row = busRow[b];
      std::uint32_t paramIndex = 0;
      if (kind == 1) {
        const std::uint64_t fx = p_.U64(*s, v, "effect", 0, d_);
        paramIndex = static_cast<std::uint32_t>(p_.U64(*s, v, "param", 0, d_));
        if (fx >= busEffects[b].second) {
          p_.ErrorAt(*s, v, "bus '" + b->name + "' has no effect " + std::to_string(fx), d_);
          continue;
        }
        row = busEffects[b].first + static_cast<std::uint32_t>(fx);
        if (paramIndex >= t.effx.GetU32(col::EFFX_paramCount, row)) {
          p_.ErrorAt(*s, v, "effect " + std::to_string(fx) + " of bus '" + b->name + "' has no parameter " +
                                std::to_string(paramIndex), d_);
          continue;
        }
      } else if (kind == 2) {
        const std::uint64_t sd = p_.U64(*s, v, "send", 0, d_);
        if (sd >= busSends[b].second) {
          p_.ErrorAt(*s, v, "bus '" + b->name + "' has no send " + std::to_string(sd), d_);
          continue;
        }
        row = busSends[b].first + static_cast<std::uint32_t>(sd);
      }
      const std::uint32_t vr = t.snpv.AddRow();
      t.snpv.U32(col::SNPV_targetKind, vr, kind);
      t.snpv.U32(col::SNPV_targetRow, vr, row);
      t.snpv.U32(col::SNPV_paramIndex, vr, paramIndex);
      t.snpv.F32(col::SNPV_value, vr, static_cast<float>(p_.Num(*s, v, "value", 0, d_)));
    }
    t.snap.U32(col::SNAP_valueCount, r, t.snpv.rows() - t.snap.GetU32(col::SNAP_valueFirst, r));
    AddName(t, s->id, s->name, bf::NameKind::Snapshot);
    AddGuid(t, *s, bf::NameKind::Snapshot);
  }
  for (const Object* dk : p_.OfKind(Kind::Ducking)) {
    if (!inBank(dk)) continue;
    const std::uint32_t kind = p_.Enum(*dk, dk->doc, "kind", kDuckKind, 0, d_);
    const Value* sv = dk->doc.Find("source");
    const Value* tv = dk->doc.Find("target");
    if (sv == nullptr || tv == nullptr || dk->doc.Find("amountDb") == nullptr) {
      p_.ErrorAt(*dk, dk->doc, "ducking needs \"source\", \"target\" and \"amountDb\"", d_);
      continue;
    }
    const Object* src = p_.Resolve(*dk, *sv, kind == 0 ? Kind::Bus : Kind::Category, d_);
    const Object* dst = p_.Resolve(*dk, *tv, Kind::Bus, d_);
    if (src == nullptr || dst == nullptr) continue;
    const std::uint32_t r = t.duck.AddRow();
    t.duck.U32(col::DUCK_kind, r, kind);
    t.duck.U32(col::DUCK_source, r, kind == 0 ? busRow[src] : catRow[src]);
    t.duck.U32(col::DUCK_targetBus, r, busRow[dst]);
    t.duck.F32(col::DUCK_amountDb, r, static_cast<float>(p_.Num(*dk, dk->doc, "amountDb", 0, d_)));
    t.duck.F32(col::DUCK_attackMs, r, static_cast<float>(p_.Num(*dk, dk->doc, "attackMs", 10, d_)));
    t.duck.F32(col::DUCK_releaseMs, r, static_cast<float>(p_.Num(*dk, dk->doc, "releaseMs", 200, d_)));
    t.duck.F32(col::DUCK_thresholdDb, r, static_cast<float>(p_.Num(*dk, dk->doc, "thresholdDb", 0, d_)));
  }

  // ---- physics material tables (9.1, 9.3) -> PHYS ----
  std::vector<const Object*> mats;
  for (const Object* m : p_.OfKind(Kind::MaterialTable)) {
    if (inBank(m)) mats.push_back(m);
  }
  if (!mats.empty()) {
    std::vector<std::uint8_t> blob(16 + mats.size() * 64, 0);
    std::vector<bf::PhysTable> tables;
    auto eventId = [&](const Object& m, const Value* v) -> std::uint32_t {
      if (v == nullptr || v->IsNull()) return 0;
      const Object* ev = p_.Resolve(m, *v, Kind::Event, d_);
      if (ev != nullptr) CheckAccess(m, *v, *ev, bn);
      return ev != nullptr ? ev->id : 0;
    };
    auto put = [&](const void* p, std::size_t n) {
      const std::size_t at = blob.size();
      blob.resize(at + n);
      std::memcpy(blob.data() + at, p, n);
      return static_cast<std::uint64_t>(at);
    };
    for (const Object* m : mats) {
      const Value& d = m->doc;
      std::vector<std::string> names;
      const Value* mv = d.Find("materials");
      for (const Value& x : (mv != nullptr && mv->IsArray()) ? mv->items : EmptyArray().items) {
        if (!x.IsString() || x.text.empty()) {
          p_.ErrorAt(*m, x, "materials are non-empty strings", d_);
          continue;
        }
        if (std::find(names.begin(), names.end(), x.text) != names.end()) p_.ErrorAt(*m, x, "duplicate material", d_);
        names.push_back(x.text);
      }
      const std::uint32_t n = static_cast<std::uint32_t>(names.size());
      if (n == 0) p_.ErrorAt(*m, d, "a material table needs \"materials\"", d_);
      auto index = [&](const Value& v) -> int {
        for (std::uint32_t i = 0; i < n; ++i) {
          if (v.IsString() && v.text == names[i]) return static_cast<int>(i);
        }
        p_.ErrorAt(*m, v, "unknown material " + (v.IsString() ? "'" + v.text + "'" : std::string()), d_);
        return -1;
      };
      std::vector<std::uint32_t> single(n * 4, 0), pair(std::size_t{n} * n, 0), foot(n, 0);
      std::vector<bf::PhysCurves> curves(n, bf::PhysCurves{{0.1f, 1.0f, 10.0f, 100.0f}, {0, 0}, {-30.0f, 0.0f}});
      if (const Value* sv = d.Find("single"); sv != nullptr && sv->IsObject()) {
        for (const auto& kv : sv->members) {
          if (kv.first == "notes") continue;  // free-form at every level
          Value key = json::MakeString(kv.first);
          key.line = kv.second.line;
          key.column = kv.second.column;
          const int i = index(key);
          if (i < 0) continue;
          for (std::uint32_t k = 0; k < 4; ++k) single[static_cast<std::size_t>(i) * 4 + k] = eventId(*m, kv.second.Find(kContact[k]));
        }
      }
      if (const Value* pv = d.Find("pairs"); pv != nullptr && pv->IsArray()) {
        for (const Value& x : pv->items) {
          const Value* av = x.Find("a");
          const Value* bv = x.Find("b");
          if (av == nullptr || bv == nullptr) {
            p_.ErrorAt(*m, x, "a pair needs \"a\", \"b\" and \"event\"", d_);
            continue;
          }
          const int a = index(*av);
          const int b = index(*bv);
          if (a < 0 || b < 0) continue;
          const std::uint32_t lo = static_cast<std::uint32_t>(std::min(a, b));
          const std::uint32_t hi = static_cast<std::uint32_t>(std::max(a, b));
          pair[std::size_t{lo} * n + hi] = eventId(*m, x.Find("event"));  // table[min*N+max] (9.3)
        }
      }
      if (const Value* fv = d.Find("footsteps"); fv != nullptr && fv->IsObject()) {
        for (const auto& kv : fv->members) {
          if (kv.first == "notes") continue;
          Value key = json::MakeString(kv.first);
          key.line = kv.second.line;
          key.column = kv.second.column;
          const int i = index(key);
          if (i >= 0) foot[static_cast<std::size_t>(i)] = eventId(*m, &kv.second);
        }
      }
      bf::PhysTable pt{};
      pt.id = m->id;
      pt.materialCount = n;
      if (const Value* dv = d.Find("defaults"); dv != nullptr && dv->IsObject()) {
        for (std::uint32_t k = 0; k < 4; ++k) pt.defaultEvent[k] = eventId(*m, dv->Find(kContact[k]));
      }
      if (const Value* cv = d.Find("curves"); cv != nullptr && cv->IsObject()) {
        for (const auto& kv : cv->members) {
          if (kv.first == "notes") continue;
          Value key = json::MakeString(kv.first);
          key.line = kv.second.line;
          key.column = kv.second.column;
          const int i = index(key);
          if (i < 0) continue;
          auto arr = [&](const char* k, float* dst, std::size_t cnt) {
            const Value* a = kv.second.Find(k);
            if (a == nullptr) return;
            if (!a->IsArray() || a->items.size() != cnt) {
              p_.ErrorAt(*m, *a, std::string("\"") + k + "\" needs " + std::to_string(cnt) + " numbers", d_);
              return;
            }
            for (std::size_t j = 0; j < cnt; ++j) {
              double v = 0;
              a->items[j].AsDouble(&v);
              dst[j] = static_cast<float>(v);
            }
          };
          bf::PhysCurves& pc = curves[static_cast<std::size_t>(i)];
          arr("impulseToVelocity", pc.impulseToVelocity, 4);
          arr("massToPitchCents", pc.massToPitchCents, 2);
          arr("speedToVolumeDb", pc.speedToVolumeDb, 2);
        }
      }
      pt.singleOffset = put(single.data(), single.size() * 4);
      pt.pairOffset = put(pair.data(), pair.size() * 4);
      pt.footstepOffset = put(foot.data(), foot.size() * 4);
      pt.curveOffset = put(curves.data(), curves.size() * sizeof(bf::PhysCurves));
      tables.push_back(pt);
      AddName(t, m->id, m->name, bf::NameKind::Other);
      AddGuid(t, *m, bf::NameKind::Other);
    }
    const bf::PhysChunkHeader ph{static_cast<std::uint32_t>(tables.size()), 0, 16};
    std::memcpy(blob.data(), &ph, sizeof(ph));
    std::memcpy(blob.data() + 16, tables.data(), tables.size() * sizeof(bf::PhysTable));
    t.phys = std::move(blob);
  }

  // ---- names: bank + aliases (10.1) ----
  std::map<Id, std::pair<Id, std::uint32_t>> aliasRows;  // oldId -> (id, NameKind)
  AddName(t, HashId(bn), bn, bf::NameKind::Other);
  AddGuid(t, bank, bf::NameKind::Other);
  for (const Object& o : p_.objects()) {
    const Value* al = o.doc.Find("aliases");
    if (al == nullptr || !al->IsArray()) continue;
    // Aliases of objects that have a row in this bank.
    const bool here = (o.kind == Kind::Event && eventRow.count(&o)) || (o.kind == Kind::Bus && busRow.count(&o)) ||
                      (o.kind == Kind::Category && catRow.count(&o)) || (o.kind == Kind::Parameter && params.count(&o)) ||
                      (o.kind == Kind::Asset && assetRow.count(&o)) || (o.kind == Kind::Snapshot && inBank(&o));
    if (!here) continue;
    for (const Value& a : al->items) {
      if (!a.IsString()) continue;
      AddName(t, o.id, a.text, bf::NameKind::Alias);
      const bf::NameKind k = o.kind == Kind::Event      ? bf::NameKind::Event
                             : o.kind == Kind::Bus      ? bf::NameKind::Bus
                             : o.kind == Kind::Parameter ? bf::NameKind::Param
                             : o.kind == Kind::Category ? bf::NameKind::Category
                             : o.kind == Kind::Snapshot ? bf::NameKind::Snapshot
                                                        : bf::NameKind::Other;
      const Id oldId = HashId(a.text);
      auto ins = aliasRows.emplace(oldId, std::make_pair(o.id, static_cast<std::uint32_t>(k)));
      if (!ins.second && ins.first->second.first != o.id) {
        p_.ErrorAt(o, a, "alias '" + a.text + "' is also an alias of another object (id " +
                             detail::Hex32(ins.first->second.first) + ")", d_);
      }
    }
  }
  // ALIS (bank format 2.0): old id -> current id, sorted by oldId, kept in
  // release banks. Chains (A -> B, B -> C) are folded so that every row names
  // the current id (the runtime resolves one step). With the rule above an
  // old name is never a current name, so chains cannot occur; folding keeps
  // the invariant even if that rule is relaxed.
  for (auto& [oldId, target] : aliasRows) {
    Id cur = target.first;
    for (std::size_t steps = 0; steps < aliasRows.size(); ++steps) {
      auto next = aliasRows.find(cur);
      if (next == aliasRows.end()) break;
      cur = next->second.first;
    }
    if (cur == oldId) {
      p_.ErrorAt(bank, bank.doc, "rename aliases form a cycle through id " + detail::Hex32(oldId), d_);
      continue;
    }
    const std::uint32_t r = t.alis.AddRow();  // std::map: ascending oldId
    t.alis.U32(col::ALIS_oldId, r, oldId);
    t.alis.U32(col::ALIS_id, r, cur);
    t.alis.U32(col::ALIS_kind, r, target.second);
  }

  if (d_.errors() > 0) return;

  // ---- assemble ----
  BankBuild b;
  b.bank = &bank;
  b.language = language;
  b.bankId = HashId(bn);
  b.file = bn + (language.empty() ? "" : "." + language) + ".sndb";
  for (TableBuilder* tb : {&t.evnt, &t.trak, &t.clip, &t.actn, &t.buss, &t.send, &t.effx, &t.efpv, &t.parm, &t.curv,
                           &t.cpnt, &t.catg, &t.snap, &t.snpv, &t.vlgr, &t.duck, &t.asst, &t.seek, &t.strm, &t.synp,
                           &t.mseg, &t.mpls, &t.mple, &t.mstm, &t.msta, &t.mtrn, &t.mstg, &t.mmrk, &t.mtmp, &t.alis}) {
    b.writer.AddTable(*tb);
  }
  if (!t.wave.empty()) b.writer.AddBlob(ch::WAVE, t.wave);
  if (!t.prel.empty()) b.writer.AddBlob(ch::PREL, t.prel);
  if (!t.spth.empty()) b.writer.AddBlob(ch::SPTH, t.spth);
  if (!t.efbl.empty()) b.writer.AddBlob(ch::EFBL, t.efbl);
  if (!t.phys.empty()) b.writer.AddBlob(ch::PHYS, t.phys);
  if (t.dspCount > 0) {
    const bf::DspPoolNeed need{t.dspBytes, t.dspCount, 0};
    std::vector<std::uint8_t> nb(sizeof(need));
    std::memcpy(nb.data(), &need, sizeof(need));
    b.writer.AddBlob(ch::DSPN, nb);
  }
  if (!opt_.stripStrings) {
    b.writer.AddBlob(ch::STRS, t.strs);
    b.writer.AddTable(t.name);
  }
  if (opt_.devBuild) b.writer.AddTable(t.guid);
  b.header.platform = opt_.platform;
  b.header.bankId = b.bankId;
  b.header.languageId = language.empty() ? 0 : HashId(language);
  b.header.flags = (opt_.devBuild ? bf::kFileDevBuild : 0u) | (opt_.stripStrings ? 0u : bf::kFileHasStrings) |
                   (language.empty() ? 0u : bf::kFileLanguageBank);
  std::vector<std::string> deps = bankDeps_[bn];
  std::sort(deps.begin(), deps.end());
  deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
  for (const std::string& dep : deps) {
    b.header.dependencies.push_back(bf::Dependency{HashId(dep), 0, bankHash_[dep]});
  }
  b.contentHash = b.writer.ContentHash(b.header);
  if (language.empty()) bankHash_[bn] = b.contentHash;
  // ID table entries.
  for (const auto& [o, row] : eventRow) AddIdEntry("event", o->name, o->guidText, b.file);
  for (const auto& [o, row] : busRow) AddIdEntry("bus", o->name, o->guidText, b.file);
  for (const auto& [o, row] : catRow) AddIdEntry("category", o->name, o->guidText, b.file);
  for (const auto& [o, row] : groupRow) AddIdEntry("voiceLimitGroup", o->name, o->guidText, b.file);
  for (const auto& [o, row] : assetRow) AddIdEntry("asset", o->name, o->guidText, b.file);
  for (const auto& [o, row] : segRow) AddIdEntry("segment", o->name, o->guidText, b.file);
  for (const auto& [o, row] : plRow) AddIdEntry("playlist", o->name, o->guidText, b.file);
  for (const Object* o : params) AddIdEntry("parameter", o->name, o->guidText, b.file);
  for (const Object* o : mats) AddIdEntry("materialTable", o->name, o->guidText, b.file);
  for (const Object* s : p_.OfKind(Kind::Snapshot)) {
    if (inBank(s)) AddIdEntry("snapshot", s->name, s->guidText, b.file);
  }
  AddIdEntry("bank", bn, bank.guidText, b.file);
  for (const Object* e : events) {
    const Value* m = e->doc.Find("music");
    if (m == nullptr) continue;
    for (const char* list : {"states", "stingers"}) {
      const Value* arr = m->Find(list);
      for (const Value& s : (arr != nullptr && arr->IsArray()) ? arr->items : EmptyArray().items) {
        if (const Value* n = s.Find("name"); n != nullptr && n->IsString()) {
          AddIdEntry(std::string(list) == "states" ? "state" : "stinger", n->text, "", b.file);
        }
      }
    }
  }
  banks_.push_back(std::move(b));
}

void Builder::AddIdEntry(const std::string& kind, const std::string& name, const std::string& guid,
                         const std::string& bank, const std::string& aliasOf) {
  IdEntry& e = ids_[{kind, name}];
  e.kind = kind;
  e.name = name;
  e.id = HashId(name);
  e.guid = guid;
  e.aliasOf = aliasOf;
  if (!bank.empty()) e.banks.insert(bank);
}

void Builder::WriteIdOutputs() {
  // Aliases (rename history, 10.1).
  for (const Object& o : p_.objects()) {
    const Value* al = o.doc.Find("aliases");
    if (al == nullptr || !al->IsArray()) continue;
    for (const Value& a : al->items) {
      if (a.IsString()) AddIdEntry(std::string(detail::KindName(o.kind)), a.text, "", "", o.name);
    }
  }
  if (const Object* proj = p_.project(); proj != nullptr) {
    for (const std::string& l : languages_) AddIdEntry("language", l, "", "");
  }
  Value root = json::MakeObject();
  json::Add(root, "generator", json::MakeString("sonus-build"));
  json::Add(root, "projectBuildHash", json::MakeString(std::to_string(res_->projectBuildHash)));
  Value list = json::MakeArray();
  for (const auto& [key, e] : ids_) {
    Value v = json::MakeObject();
    json::Add(v, "kind", json::MakeString(e.kind));
    json::Add(v, "name", json::MakeString(e.name));
    json::Add(v, "id", json::MakeString(detail::Hex32(e.id)));
    if (!e.guid.empty()) json::Add(v, "guid", json::MakeString(e.guid));
    if (!e.aliasOf.empty()) json::Add(v, "aliasOf", json::MakeString(e.aliasOf));
    Value banks = json::MakeArray();
    for (const std::string& b : e.banks) banks.items.push_back(json::MakeString(b));
    json::Add(v, "banks", std::move(banks));
    list.items.push_back(std::move(v));
  }
  json::Add(root, "ids", std::move(list));
  const std::string text = json::Write(root);
  Stage("ids.json", std::vector<std::uint8_t>(text.begin(), text.end()));
  if (opt_.idsHeader.empty()) return;
  // C++ header for game code: constants equal to "..."_id, checked at compile time.
  std::string h =
      "// Generated by sonus-build from the project. Do not edit.\n"
      "// Each constant equals snd::HashId(\"<name>\") (\"<name>\"_id); aliases map old names to\n"
      "// the current id (10.1).\n"
      "#pragma once\n\n#include \"snd/id.hpp\"\n\nnamespace snd_ids {\n";
  std::string currentKind;
  std::set<std::string> used;
  std::string asserts;
  for (const auto& [key, e] : ids_) {
    if (e.kind != currentKind) {
      if (!currentKind.empty()) h += "}  // namespace " + currentKind + "\n";
      currentKind = e.kind;
      used.clear();
      h += "namespace " + currentKind + " {\n";  // kind names: a fixed set of lower-case words
    }
    std::string ident = HeaderIdent(e.name);
    for (int n = 2; !used.insert(ident).second; ++n) ident = HeaderIdent(e.name) + "_" + std::to_string(n);
    const std::string escaped = json::Quote(e.name);
    if (e.aliasOf.empty()) {
      h += "inline constexpr snd::Id " + ident + " = " + detail::Hex32(e.id) + "u;  // " + escaped + "\n";
      asserts += "static_assert(" + currentKind + "::" + ident + " == snd::HashId(" + escaped + "));\n";
    } else {
      h += "inline constexpr snd::Id " + ident + " = " + detail::Hex32(HashId(e.aliasOf)) + "u;  // alias " +
           escaped + " -> " + json::Quote(e.aliasOf) + "\n";
    }
  }
  if (!currentKind.empty()) h += "}  // namespace " + currentKind + "\n";
  h += "\n" + asserts + "\n}  // namespace snd_ids\n";
  Stage(opt_.idsHeader, std::vector<std::uint8_t>(h.begin(), h.end()));
}

void Builder::Stage(const std::string& rel, std::vector<std::uint8_t> bytes) {
  pending_.push_back(PendingFile{rel, std::move(bytes)});
}

// Two phases so that a failure never leaves the output half replaced:
//  1. every changed file is written under a temporary name next to its target
//     (and every target is checked to be a regular file or absent);
//  2. only if all of that succeeded, the temporary files are renamed over the
//     targets. Any failure in phase 1 removes the temporary files and keeps the
//     old outputs. A failure inside phase 2 (rename refused by the OS after
//     the checks passed) is reported; outputs renamed before it are new, the
//     rest are old and their temporary files are removed (README).
bool Builder::Commit() {
  struct Job {
    fs::path target;
    fs::path tmp;
    const PendingFile* file;
  };
  std::vector<Job> jobs;
  std::error_code ec;
  for (const PendingFile& f : pending_) {
    const fs::path path = fs::path(opt_.outDir) / f.rel;
    std::vector<std::uint8_t> old;
    if (ReadBinary(path, &old) && old == f.bytes) {
      ++res_->filesUnchanged;
      continue;
    }
    jobs.push_back(Job{path, fs::path(path.string() + ".sonus-tmp"), &f});
  }
  auto removeTemps = [&]() {
    for (const Job& j : jobs) fs::remove(j.tmp, ec);
  };
  for (const Job& j : jobs) {
    const fs::file_status st = fs::symlink_status(j.target, ec);
    if (fs::exists(st) && !fs::is_regular_file(st)) {
      d_.Error(j.file->rel, 0, 0, "cannot replace " + j.target.string() + ": it exists and is not a regular file");
      removeTemps();
      return false;
    }
    fs::create_directories(j.target.parent_path(), ec);
    if (!detail::WriteFileBytes(j.tmp, j.file->bytes)) {
      d_.Error(j.file->rel, 0, 0, "cannot write output file " + j.tmp.string());
      removeTemps();
      return false;
    }
  }
  for (std::size_t i = 0; i < jobs.size(); ++i) {
    fs::rename(jobs[i].tmp, jobs[i].target, ec);
    if (ec) {
      d_.Error(jobs[i].file->rel, 0, 0,
               "cannot replace output file " + jobs[i].target.string() + " (" + std::to_string(i) + " of " +
                   std::to_string(jobs.size()) + " files were already replaced)");
      removeTemps();
      return false;
    }
    ++res_->filesWritten;
    for (BuiltBank& b : res_->banks) {
      if (b.path == jobs[i].file->rel) b.written = true;
    }
  }
  return d_.errors() == 0;
}

}  // namespace

bool ParsePlatform(const std::string& name, std::uint32_t* fourcc) {
  static const char* const known[] = {"ANY_", "WIN_", "LNX_", "MAC_", "IOS_", "AND_"};
  for (const char* k : known) {
    if (name == k) return ParseCodec(name, fourcc);
  }
  return false;
}

bool Build(const BuildOptions& options, Diagnostics& diag, BuildResult* result) {
  BuildResult local;
  BuildResult* r = result != nullptr ? result : &local;
  *r = BuildResult{};
  if (options.projectDir.empty() || options.outDir.empty()) {
    diag.Error("", 0, 0, "project and output directories are required");
    return false;
  }
  Builder b(options, diag, r);
  return b.Run();
}

}  // namespace snd::buildtool
