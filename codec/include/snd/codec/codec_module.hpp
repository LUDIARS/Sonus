// codec module (design.md 12.1): PCM, ADPCM decoders (ICodec), codec registry,
// decoder pool, build-time encoders. Dependencies (frozen, root CMakeLists.txt): base.
//
//   snd/codec/codec.hpp         built-in decoders, header checks, seek helpers (runtime)
//   snd/codec/registry.hpp      fourcc -> ICodec (built-ins + Config.plugins.codecs, D-31)
//   snd/codec/decoder_pool.hpp  fixed decoder state slots (3.3, D-14)
//   snd/codec/encode.hpp        encoders, header and seek-table generation (buildtool only)
// See codec/README.md.
#ifndef SND_CODEC_CODEC_MODULE_HPP
#define SND_CODEC_CODEC_MODULE_HPP

namespace snd::codec {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::codec

#endif  // SND_CODEC_CODEC_MODULE_HPP
