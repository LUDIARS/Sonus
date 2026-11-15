// sonus-build: project (JSON, one object per file) -> banks (design.md 10).
// Usage: see buildtool/README.md or `sonus-build --help`.
#include <cstdio>
#include <cstring>
#include <string>

#include "snd/buildtool/build.hpp"
#include "snd/format/bank_format.hpp"

namespace {

void Usage(FILE* f) {
  std::fprintf(f,
               "usage: sonus-build --project <dir> --out <dir> [options]\n"
               "  --platform <TAG>    ANY_ (default), WIN_, LNX_, MAC_, IOS_, AND_\n"
               "  --release           no GUID chunk, no dev flag (default: dev build)\n"
               "  --strip-strings     omit STRS/NAME (names) from the banks\n"
               "  --cache <dir>       differential build cache for converted waveforms\n"
               "  --ids-header <rel>  game header path inside --out (default sonus_ids.h, '' = none)\n"
               "  --quiet             only print diagnostics\n"
               "  --werror            treat warnings as errors (nothing is written)\n"
               "bank format %u.%u\n",
               snd::bank_format::kFormatMajor, snd::bank_format::kFormatMinor);
}

}  // namespace

int main(int argc, char** argv) {
  snd::buildtool::BuildOptions o;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "sonus-build: %s needs a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    const char* v = nullptr;
    if (a == "--help" || a == "-h") {
      Usage(stdout);
      return 0;
    } else if (a == "--project") {
      if ((v = value("--project")) == nullptr) return 2;
      o.projectDir = v;
    } else if (a == "--out") {
      if ((v = value("--out")) == nullptr) return 2;
      o.outDir = v;
    } else if (a == "--cache") {
      if ((v = value("--cache")) == nullptr) return 2;
      o.cacheDir = v;
    } else if (a == "--ids-header") {
      if ((v = value("--ids-header")) == nullptr) return 2;
      o.idsHeader = v;
    } else if (a == "--platform") {
      if ((v = value("--platform")) == nullptr) return 2;
      if (!snd::buildtool::ParsePlatform(v, &o.platform)) {
        std::fprintf(stderr, "sonus-build: unknown platform '%s'\n", v);
        return 2;
      }
    } else if (a == "--release") {
      o.devBuild = false;
    } else if (a == "--strip-strings") {
      o.stripStrings = true;
    } else if (a == "--werror") {
      o.warningsAsErrors = true;
    } else if (a == "--quiet") {
      quiet = true;
    } else {
      std::fprintf(stderr, "sonus-build: unknown option '%s'\n", a.c_str());
      Usage(stderr);
      return 2;
    }
  }
  if (o.projectDir.empty() || o.outDir.empty()) {
    Usage(stderr);
    return 2;
  }
  snd::buildtool::Diagnostics diag;
  snd::buildtool::BuildResult r;
  const bool ok = snd::buildtool::Build(o, diag, &r);
  for (const snd::buildtool::Diagnostic& d : diag.items()) {
    std::fprintf(stderr, "%s\n", snd::buildtool::Diagnostics::Format(d).c_str());
  }
  if (!ok) {
    std::fprintf(stderr, "sonus-build: %u error(s), %u warning(s); nothing written\n", diag.errors(), diag.warnings());
    return 1;
  }
  if (!quiet) {
    for (const snd::buildtool::BuiltBank& b : r.banks) {
      std::printf("%s %s (%llu bytes, content %016llx)\n", b.written ? "wrote    " : "unchanged", b.path.c_str(),
                  static_cast<unsigned long long>(b.bytes), static_cast<unsigned long long>(b.contentHash));
    }
    std::printf("assets: %u encoded, %u from cache; files: %u written, %u unchanged; %u warning(s)\n",
                r.assetsEncoded, r.assetsFromCache, r.filesWritten, r.filesUnchanged, diag.warnings());
  }
  return 0;
}
