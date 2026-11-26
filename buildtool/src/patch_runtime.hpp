#pragma once
#include "snd/buildtool/patch.hpp"

namespace snd::buildtool::patch {
// Runtime programs (D-06). Returns the offending JSON value for diagnostics.
bool CompileInline(const json::Value& synth, std::vector<std::uint8_t>& bytes,
                   const json::Value*& at, std::string& message);
bool EncodeRuntime(const Program& program, std::vector<std::uint8_t>& bytes);
}  // namespace snd::buildtool::patch
