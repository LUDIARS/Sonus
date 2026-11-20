// Where the build reads the project from and where it puts its outputs, when not
// the file system (the authoring tool builds the document it holds in memory, D-170
// candidate in spec/handoff/parallel/C-notes/model.md).
//
//   BuildOptions o;
//   o.source = &myDocumentSource;   // the *.json objects and the files they name
//   o.memoryOutput = &files;        // banks / ids.json / sonus_ids.h / streams in memory
//   Build(o, diag, &result);
//
// The rules and the diagnostics are the same as for a project directory: the
// source lists project-relative paths ('/' separators) and returns their bytes,
// exactly what the directory walk of Project::Load would have read (the build
// sorts the list, skips names that do not end in ".json", and strips a UTF-8 BOM).
#ifndef SND_BUILDTOOL_SOURCE_HPP
#define SND_BUILDTOOL_SOURCE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::buildtool {

class ProjectSource {
 public:
  virtual ~ProjectSource() = default;
  // Every project object file (project-relative, '/' separated), any order.
  virtual void ListObjectFiles(std::vector<std::string>* out) const = 0;
  // Bytes of a project-relative file: object files, WAV files, effect blobs.
  // false when the file does not exist or cannot be read.
  virtual bool ReadFile(const std::string& path, std::vector<std::uint8_t>* out) const = 0;
};

// One output of an in-memory build (BuildOptions::memoryOutput).
struct OutputFile {
  std::string path;  // relative to the output directory ("Main.sndb", "streams/x_1234abcd.snds", "ids.json")
  std::vector<std::uint8_t> bytes;
};

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_SOURCE_HPP
