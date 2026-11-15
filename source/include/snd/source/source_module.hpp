// source module (design.md 12.1): Sample, Stream, Oscillator, Noise, Granular, Patch, Callback, UserBuffer, Input (ISourceKind).
// Dependencies (frozen, root CMakeLists.txt): core (internal IF), codec, io.
//
// Stage-0 stub. The owner of this module adds its headers under
// source/include/snd/source/ and its sources under source/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_SOURCE_SOURCE_MODULE_HPP
#define SND_SOURCE_SOURCE_MODULE_HPP

namespace snd::source {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::source

#endif  // SND_SOURCE_SOURCE_MODULE_HPP
