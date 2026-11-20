// Checking and building the document (design.md 10, 11.2; D-171 candidate).
//
// CheckDocument / BuildDocument run the build tool (snd::buildtool::Build) on the
// document as it is in memory (unsaved edits included) through a ProjectSource: the
// problems an editor shows are the build's diagnostics, with the same rules and the same
// words. Each problem is tied back to the object (GUID) and the member (path) it is about.
// Unchanged objects are fed with their file text, so line and column are the file's;
// changed objects with the text Save would write.
//
// The banks are built in memory (and optionally also written to a directory, like
// sonus-build). Group P uses them for the live link: DiffBuilds compares two builds and
// says what an edit changed in the banks:
//   - ValueEdit: one cell (bank, chunk fourcc, column, row) whose value changed while the
//     table kept its shape, with the live address the runtime resolves (runtimeId of the
//     owning object + index, livelink SetValue, protocol §5) and whether the runtime
//     accepts a live write of that column (core's CheckEdit rules). -> SetValue.
//   - ChunkEdit: a chunk whose shape changed (rows added / removed), a blob chunk (PHYS,
//     WAVE, XPCH ...), or cells the runtime cannot write live. -> ReplaceChunk (or a reload).
//   - banks added / removed / with other dependencies: a reload.
// ClassifyDelta: None (nothing reaches a bank: notes, editor positions), Value (only live
// cells) or Structure (anything else: rebuild + ReplaceChunk / hot reload).
//
// LocateField gives the cells of one member of one object without an edit (the table of
// FieldColumns below: which bank, chunk, row and column a member is written to).
#ifndef SND_TOOL_DOC_BUILD_HPP
#define SND_TOOL_DOC_BUILD_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "snd/buildtool/build.hpp"
#include "snd/buildtool/source.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/project.hpp"

namespace snd::tool {

// A built bank read with the contract's checker (snd/format/bank_check.hpp: the same
// rules as the runtime's ParseBank; the tool does not depend on the bank module).
class ParsedBank {
 public:
  bool Parse(const std::vector<std::uint8_t>& bytes);
  bool ok() const { return ok_; }
  std::uint32_t bankId() const;
  std::uint32_t languageId() const;
  std::uint32_t dependencyCount() const;
  std::uint32_t dependencyBankId(std::uint32_t i) const;
  std::uint32_t Rows(std::uint32_t fourcc) const;
  bool HasColumn(std::uint32_t fourcc, std::uint32_t column) const;
  // Cell bits (absent column: its default). false if the table / column / row does not exist.
  bool Cell(std::uint32_t fourcc, std::uint32_t column, std::uint32_t row, std::uint64_t* bits) const;
  std::uint32_t U32(std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) const;  // kNone if absent
  float F32(std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) const;
  // Any chunk by fourcc (known or not): pointer and size, {nullptr, 0} if absent.
  const std::uint8_t* Chunk(std::uint32_t fourcc, std::uint64_t* bytes) const;
  bool Guid(Id id, std::uint64_t* lo, std::uint64_t* hi) const;  // GUID chunk (dev builds)
  // First row of an id table (column 0) with `id`, kNone if none.
  std::uint32_t FindId(std::uint32_t fourcc, Id id) const;

 private:
  std::vector<std::uint8_t> storage_;
  const std::uint8_t* data_ = nullptr;
  std::uint64_t bytes_ = 0;
  bool ok_ = false;
  struct Checked;
  std::shared_ptr<const Checked> checked_;
};

// The document as a build tool source (object files from memory, other files from the
// document's binary files or directory).
class DocumentSource final : public buildtool::ProjectSource {
 public:
  explicit DocumentSource(const ProjectDocument& doc) : doc_(doc) {}
  void ListObjectFiles(std::vector<std::string>* out) const override;
  bool ReadFile(const std::string& path, std::vector<std::uint8_t>* out) const override;

 private:
  const ProjectDocument& doc_;
};

struct DocIssue {
  bool error = true;
  std::string file;       // project-relative ("" = project wide)
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string message;    // the build's message
  std::string guid;       // object of `file` ("" if none)
  std::string path;       // member at line:column ("" = the object / unknown)
  std::string text;       // "file:line:col: error: message" (buildtool Diagnostics::Format)
};

struct DocBuildOptions {
  std::uint32_t platform = 0x5F594E41u;  // 'ANY_' (bank_format::kPlatformAny)
  bool devBuild = true;
  bool warningsAsErrors = false;
  std::string outDir;    // "" = memory only; else the outputs are also written there
  std::string cacheDir;  // differential build cache ("" = none)
  std::uint32_t dspSampleRate = 48000;
  std::uint32_t dspMaxFrames = 256;
  const buildtool::EncoderRegistry* encoders = nullptr;
};

struct BankImage {
  std::string name;
  std::string language;  // "" for normal banks
  std::string file;      // "<name>[.<language>].sndb"
  Id bankId = 0;
  std::uint64_t contentHash = 0;
  std::vector<std::uint8_t> bytes;
  // Parsed view of `bytes` (64-byte aligned copy), null if it does not parse.
  const ParsedBank* view() const;

  BankImage();
  ~BankImage();
  BankImage(const BankImage&);
  BankImage& operator=(const BankImage&);
  BankImage(BankImage&&) noexcept;
  BankImage& operator=(BankImage&&) noexcept;

 private:
  mutable std::shared_ptr<ParsedBank> parsed_;
};

struct DocBuild {
  bool ok = false;
  std::vector<DocIssue> issues;
  std::vector<BankImage> banks;
  std::vector<buildtool::OutputFile> files;  // streams, ids.json, sonus_ids.h
  std::uint64_t buildHash = 0;               // Hello.projectBuildHash (11.2)
  std::uint64_t documentRevision = 0;
  const BankImage* FindBank(const std::string& name, const std::string& language = std::string()) const;
  std::uint32_t errorCount() const;
};

DocBuild BuildDocument(const ProjectDocument& doc, const DocBuildOptions& options = {});
// The build's problems only (in memory, nothing written).
std::vector<DocIssue> CheckDocument(const ProjectDocument& doc);
// Problems about one object.
std::vector<DocIssue> IssuesOf(const std::vector<DocIssue>& issues, const std::string& guid);

// --- live mapping ---------------------------------------------------------------------------
// livelink ValueType (protocol §5).
enum class CellType : std::uint32_t { F32 = 0, U32 = 1, I32 = 2, Id = 3, U64 = 4 /* not sendable */ };

struct CellRef {
  std::string bank;
  std::string language;
  Id bankId = 0;
  std::uint32_t chunk = 0;   // bank_format chunk fourcc
  std::uint32_t column = 0;  // column id
  std::uint32_t row = 0;
  // Live address (SetValue): owning object's runtime id and index (core ResolveRow).
  bool addressable = false;
  Id runtimeId = 0;
  std::uint32_t index = 0;
  Guid guid;                 // owning object's GUID (bank GUID chunk), zero if unknown
  CellType type = CellType::F32;
  bool liveWritable = false;  // the runtime accepts SetValue of this column (core CheckEdit)
  std::uint64_t bits = 0;     // value in the bank (raw bits)
};

struct ValueEdit {
  CellRef cell;          // cell.bits = new value
  std::uint64_t oldBits = 0;
};

struct ChunkEdit {
  std::string bank;
  std::string language;
  Id bankId = 0;
  std::uint32_t fourcc = 0;
  bool removed = false;
  std::vector<std::uint8_t> bytes;  // the new chunk (empty if removed)
};

enum class EditClass : std::uint8_t { None = 0, Value = 1, Structure = 2 };

struct LiveDelta {
  std::vector<ValueEdit> values;
  std::vector<ChunkEdit> chunks;
  std::vector<std::string> banksAdded;    // file names
  std::vector<std::string> banksRemoved;
  std::vector<std::string> banksRelinked;  // dependencies / identity changed
  EditClass kind = EditClass::None;
};

LiveDelta DiffBuilds(const DocBuild& before, const DocBuild& after);
// Builds the document before and after (`before` a copy of the document) and diffs.
EditClass ClassifyEdit(const ProjectDocument& before, const ProjectDocument& after, LiveDelta* delta = nullptr);

// Cells of member `path` of object `guid` in every bank it is in (FieldColumns table).
std::vector<CellRef> LocateField(const DocBuild& build, const ProjectDocument& doc, const std::string& guid,
                                 const std::string& path);
// The (kind, member pattern) -> (chunk, column) table LocateField uses ('#' = an index).
struct FieldColumn {
  const char* kind;
  const char* pattern;  // "tracks[#].clips[#].startMs", "effects[#].params[#]"
  std::uint32_t chunk;
  std::uint16_t column;
};
const std::vector<FieldColumn>& FieldColumns();

float BitsToFloat(std::uint64_t bits);

}  // namespace snd::tool

#endif  // SND_TOOL_DOC_BUILD_HPP
