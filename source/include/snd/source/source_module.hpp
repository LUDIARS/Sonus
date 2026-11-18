// source module (design.md 12.1): Sample, Stream, Oscillator, Noise, Granular, Patch, Callback, UserBuffer, Input (ISourceKind).
// Dependencies (frozen, root CMakeLists.txt): core (internal IF), codec, io.
//
// Stage 2: the Sample and Stream kinds (runtime.hpp: snd::source::Runtime).
// The other kinds (Oscillator, Noise, Granular, Patch, Callback, UserBuffer,
// Input) come in later stages. Only types from the contract (contract/), base
// and platform appear in the public headers (D-02).
#ifndef SND_SOURCE_SOURCE_MODULE_HPP
#define SND_SOURCE_SOURCE_MODULE_HPP

namespace snd::source {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::source

#endif  // SND_SOURCE_SOURCE_MODULE_HPP
