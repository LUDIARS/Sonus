// codec module (design.md 12.1): PCM, ADPCM, Opus decoders (ICodec), decoder pool.
// Dependencies (frozen, root CMakeLists.txt): base.
//
// Stage-0 stub. The owner of this module adds its headers under
// codec/include/snd/codec/ and its sources under codec/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_CODEC_CODEC_MODULE_HPP
#define SND_CODEC_CODEC_MODULE_HPP

namespace snd::codec {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::codec

#endif  // SND_CODEC_CODEC_MODULE_HPP
