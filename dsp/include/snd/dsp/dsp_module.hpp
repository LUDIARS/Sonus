// dsp module (design.md 12.1): standard effects, resamplers, filters, panning, downmix, meters.
// Dependencies (frozen, root CMakeLists.txt): base, platform.
//
// Stage-0 stub. The owner of this module adds its headers under
// dsp/include/snd/dsp/ and its sources under dsp/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_DSP_DSP_MODULE_HPP
#define SND_DSP_DSP_MODULE_HPP

namespace snd::dsp {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_DSP_MODULE_HPP
