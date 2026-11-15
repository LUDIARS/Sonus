// sonus-build: project (JSON, one object per file) -> bank (design.md 10).
// Stage-0 stub: prints the formats it will produce.
#include <cstdio>

#include "snd/buildtool/buildtool_module.hpp"
#include "snd/format/bank_format.hpp"

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::printf("sonus-build (%s): bank format %u.%u - not implemented yet\n",
              snd::buildtool::ModuleName(), snd::bank_format::kFormatMajor,
              snd::bank_format::kFormatMinor);
  return 0;
}
