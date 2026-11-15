// Build diagnostics: "file:line:col: error: reason" (design.md 10).
#ifndef SND_BUILDTOOL_DIAGNOSTICS_HPP
#define SND_BUILDTOOL_DIAGNOSTICS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::buildtool {

enum class Severity : std::uint8_t { Warning, Error };

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string file;  // project-relative path ("" = project wide)
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string message;
};

class Diagnostics {
 public:
  void Error(std::string file, std::uint32_t line, std::uint32_t column, std::string message) {
    items_.push_back(Diagnostic{Severity::Error, std::move(file), line, column, std::move(message)});
    ++errors_;
  }
  void Warning(std::string file, std::uint32_t line, std::uint32_t column, std::string message) {
    items_.push_back(Diagnostic{Severity::Warning, std::move(file), line, column, std::move(message)});
    ++warnings_;
  }
  std::uint32_t errors() const { return errors_; }
  std::uint32_t warnings() const { return warnings_; }
  const std::vector<Diagnostic>& items() const { return items_; }
  // True if any diagnostic message contains `text` (tests).
  bool Contains(const std::string& text) const {
    for (const Diagnostic& d : items_) {
      if (d.message.find(text) != std::string::npos) return true;
    }
    return false;
  }
  static std::string Format(const Diagnostic& d) {
    std::string s = d.file.empty() ? std::string("<project>") : d.file;
    if (d.line > 0) s += ":" + std::to_string(d.line) + ":" + std::to_string(d.column);
    s += d.severity == Severity::Error ? ": error: " : ": warning: ";
    return s + d.message;
  }

 private:
  std::vector<Diagnostic> items_;
  std::uint32_t errors_ = 0;
  std::uint32_t warnings_ = 0;
};

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_DIAGNOSTICS_HPP
