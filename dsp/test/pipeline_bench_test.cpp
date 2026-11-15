// Voice pipeline (resample -> voice filter -> pan -> mix) as core will run it:
// golden output of a small mix, RT safety of the whole path, and a timing of
// 128 voices at 48 kHz / 256 frames (design.md 14: < 30 % of the block budget).
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/dsp/pan.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/simd.hpp"
#include "snd/test/golden.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

namespace {

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

// Pre-rendered source material (a looped table per voice, source rate 44.1 kHz).
struct Voice {
  std::uint32_t channels;
  const std::vector<float>* table;
  std::uint64_t pos;
  dsptest::AlignedMem resampler;
  snd::FilterState filter;
  snd::PanMatrix pan;
  float pitch;
  float azimuth;
};

struct Pipeline {
  ResamplerQuality quality;
  std::vector<std::unique_ptr<Voice>> voices;
  std::vector<float> table;
  Audio in;      // per-voice source scratch (pad + frames)
  Audio voiceOut;
  Audio bus;
  Pipeline(ResamplerQuality q, std::uint32_t count, std::uint32_t channels)
      : quality(q), table(44100), in(channels, 2048, ResamplerPadFrames(q)), voiceOut(channels, kBlock),
        bus(2, kBlock) {
    for (std::size_t i = 0; i < table.size(); ++i) {
      const double t = static_cast<double>(i) / 44100.0;
      table[i] = static_cast<float>(0.2 * std::sin(2 * dsptest::kPi * 441.0 * t) +
                                    0.1 * std::sin(2 * dsptest::kPi * 3087.0 * t));
    }
    for (std::uint32_t v = 0; v < count; ++v) {
      auto vc = std::make_unique<Voice>();
      vc->channels = channels;
      vc->table = &table;
      vc->pos = v * 977u;
      vc->resampler.Reset(ResamplerStateBytes(q, channels), dsptest::kDirty);
      ResamplerInit(vc->resampler.get(), q, channels);
      vc->pitch = 0.75f + 0.5f * static_cast<float>(v % 17) / 16.0f;
      vc->azimuth = -3.0f + 6.0f * static_cast<float>(v) / static_cast<float>(count);
      VoiceFilterInit(vc->filter, snd::FilterTarget{2000.0f + 100.0f * (v % 50), 0.0f});
      ComputePanMatrix(channels, SpeakerLayout::Stereo, vc->azimuth, 0.2f, &vc->pan);
      voices.push_back(std::move(vc));
    }
  }

  // One block for every voice into the stereo bus (what Render does per chunk).
  void RenderBlock(std::uint64_t block) {
    SND_RT_SCOPE();
    Clear(bus.p(), 2, kBlock);
    const std::uint32_t pad = ResamplerPadFrames(quality);
    float* base[8];
    for (std::uint32_t c = 0; c < in.channels(); ++c) base[c] = in.Base(c);
    for (auto& vp : voices) {
      Voice& v = *vp;
      const float r0 = 44100.0f / 48000.0f * v.pitch;
      const float wob = 1.0f + 0.01f * static_cast<float>((block % 8u)) / 8.0f;
      const float r1 = r0 * wob;
      const std::uint32_t need = ResamplerInputFrames(v.resampler.get(), kBlock, r0, r1);
      const std::uint32_t n = v.table->size();
      for (std::uint32_t c = 0; c < v.channels; ++c) {
        float* dst = in.Base(c) + pad;
        std::uint32_t p = static_cast<std::uint32_t>(v.pos % n);
        for (std::uint32_t i = 0; i < need; ++i) {
          dst[i] = (*v.table)[p];
          p = p + 1u == n ? 0u : p + 1u;
        }
      }
      v.pos += need;
      ResamplerProcess(v.resampler.get(), base, need, voiceOut.p(), kBlock, r0, r1);
      VoiceFilterProcess(v.filter, voiceOut.p(), v.channels, kBlock,
                         snd::FilterTarget{1500.0f + 50.0f * static_cast<float>(block % 20u), 80.0f}, kRate);
      snd::PanMatrix next;
      ComputePanMatrix(v.channels, SpeakerLayout::Stereo, v.azimuth + 0.01f * static_cast<float>(block),
                       0.2f, &next);
      ApplyPanMatrix(voiceOut.cp(), v.channels, bus.p(), 2, kBlock, v.pan, next, true);
      v.pan = next;
    }
  }
};

}  // namespace

SND_TEST(Pipeline, GoldenMix) {
  Pipeline p(ResamplerQuality::Sinc, 8, 1);
  Audio out(2, 8 * kBlock);
  for (std::uint32_t b = 0; b < 8; ++b) {
    p.RenderBlock(b);
    for (std::uint32_t c = 0; c < 2; ++c) std::memcpy(out.ch(c) + b * kBlock, p.bus.ch(c), kBlock * sizeof(float));
  }
  snd::test::GoldenTolerance tol;
  tol.maxAbs = 2e-4f;
  tol.maxRms = 2e-5f;
  SND_CHECK_GOLDEN("pipeline_8voices", dsptest::ToBuffer(out, 8 * kBlock), tol);
}

SND_TEST(Pipeline, Bench128Voices) {
  const double budgetUs = 1e6 * kBlock / kRate;
  struct Case {
    ResamplerQuality q;
    std::uint32_t channels;
    const char* name;
  };
  const Case cases[] = {{ResamplerQuality::Sinc, 1, "sinc mono"},
                        {ResamplerQuality::Sinc, 2, "sinc stereo"},
                        {ResamplerQuality::Cubic, 1, "cubic mono"},
                        {ResamplerQuality::Linear, 1, "linear mono"}};
  for (const Case& c : cases) {
    Pipeline p(c.q, 128, c.channels);
    for (int b = 0; b < 4; ++b) p.RenderBlock(static_cast<std::uint64_t>(b));  // warm up
    constexpr int kBlocks = 100;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < kBlocks; ++b) p.RenderBlock(static_cast<std::uint64_t>(b));
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kBlocks;
    snd::test::Note("128 voices %-12s (%s): %.1f us/block = %.1f %% of the %.0f us block", c.name,
                    snd::simd::kPathName, us, 100.0 * us / budgetUs, budgetUs);
    SND_CHECK(!HasNonFinite(p.bus.ch(0), kBlock));
  }
}
