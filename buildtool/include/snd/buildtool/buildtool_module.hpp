// buildtool module (design.md 12.1): project -> bank conversion CLI (sonus-build).
// Dependencies (frozen, root CMakeLists.txt): bank format definitions (contract), codec.
//
// Stage-0 stub. The owner of this module adds its headers under
// buildtool/include/snd/buildtool/ and its sources under buildtool/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_BUILDTOOL_BUILDTOOL_MODULE_HPP
#define SND_BUILDTOOL_BUILDTOOL_MODULE_HPP

namespace snd::buildtool {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_BUILDTOOL_MODULE_HPP
