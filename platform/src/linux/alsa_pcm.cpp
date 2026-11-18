#include "linux/alsa_pcm.hpp"

#include <cerrno>
#include <cstring>

#include "common/stream_policy.hpp"
#include "snd/types.h"

namespace snd::platform::detail {

namespace {
snd_pcm_format_t ToAlsa(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::Float32: return SND_PCM_FORMAT_FLOAT;  // native endian
    case SampleFormat::Int32: return SND_PCM_FORMAT_S32;
    case SampleFormat::Int16: return SND_PCM_FORMAT_S16;
  }
  return SND_PCM_FORMAT_UNKNOWN;
}

// RAII for the parameter containers (heap allocated by ALSA).
struct HwParams {
  snd_pcm_hw_params_t* p = nullptr;
  HwParams() noexcept { snd_pcm_hw_params_malloc(&p); }
  ~HwParams() { if (p != nullptr) snd_pcm_hw_params_free(p); }
  HwParams(const HwParams&) = delete;
  HwParams& operator=(const HwParams&) = delete;
};
struct SwParams {
  snd_pcm_sw_params_t* p = nullptr;
  SwParams() noexcept { snd_pcm_sw_params_malloc(&p); }
  ~SwParams() { if (p != nullptr) snd_pcm_sw_params_free(p); }
  SwParams(const SwParams&) = delete;
  SwParams& operator=(const SwParams&) = delete;
};
}  // namespace

int OpenAlsaPcm(const char* name, const char* configText, snd_pcm_t** out) noexcept {
  *out = nullptr;
  if (name == nullptr || name[0] == '\0') name = "default";
  if (configText == nullptr) {
    return snd_pcm_open(out, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  }
  snd_config_t* top = nullptr;
  snd_input_t* in = nullptr;
  int rc = snd_config_top(&top);
  if (rc < 0) return rc;
  rc = snd_input_buffer_open(&in, configText, static_cast<ssize_t>(std::strlen(configText)));
  if (rc >= 0) {
    rc = snd_config_load(top, in);
    snd_input_close(in);
  }
  if (rc >= 0) rc = snd_pcm_open_lconf(out, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK, top);
  snd_config_delete(top);  // the PCM keeps its own copy of what it needs
  return rc;
}

int ConfigureAlsaPcm(snd_pcm_t* pcm, const AlsaRequest& request, AlsaStream* out) noexcept {
  HwParams hw;
  SwParams sw;
  if (hw.p == nullptr || sw.p == nullptr) return -ENOMEM;
  int rc = snd_pcm_hw_params_any(pcm, hw.p);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params_set_access(pcm, hw.p, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) return rc;
  bool haveFormat = false;
  for (SampleFormat f : kFormatPreference) {
    if (snd_pcm_hw_params_test_format(pcm, hw.p, ToAlsa(f)) == 0 &&
        snd_pcm_hw_params_set_format(pcm, hw.p, ToAlsa(f)) == 0) {
      out->format = f;
      haveFormat = true;
      break;
    }
  }
  if (!haveFormat) return -EINVAL;
  unsigned int channels = request.channels != 0u ? request.channels : 2u;
  rc = snd_pcm_hw_params_set_channels_near(pcm, hw.p, &channels);
  if (rc < 0) return rc;
  if (channels == 0u || channels > SND_MAX_CHANNELS) return -EINVAL;
  snd_pcm_hw_params_set_rate_resample(pcm, hw.p, 1);  // plug devices may resample
  unsigned int rate = request.sampleRate != 0u ? request.sampleRate : 48000u;
  rc = snd_pcm_hw_params_set_rate_near(pcm, hw.p, &rate, nullptr);
  if (rc < 0) return rc;
  snd_pcm_uframes_t period = request.periodFrames != 0u ? request.periodFrames : DefaultPeriodFrames(rate);
  rc = snd_pcm_hw_params_set_period_size_near(pcm, hw.p, &period, nullptr);
  if (rc < 0) return rc;
  snd_pcm_uframes_t buffer = period * (request.periods >= 2u ? request.periods : 2u);
  rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw.p, &buffer);
  if (rc < 0) return rc;
  rc = snd_pcm_hw_params(pcm, hw.p);
  if (rc < 0) return rc;
  snd_pcm_hw_params_get_period_size(hw.p, &period, nullptr);
  snd_pcm_hw_params_get_buffer_size(hw.p, &buffer);
  if (period == 0u || buffer < period) return -EINVAL;

  rc = snd_pcm_sw_params_current(pcm, sw.p);
  if (rc < 0) return rc;
  // Start when the buffer is full (the device thread prefills it), wake when one period is free.
  rc = snd_pcm_sw_params_set_start_threshold(pcm, sw.p, buffer);
  if (rc < 0) return rc;
  rc = snd_pcm_sw_params_set_avail_min(pcm, sw.p, period);
  if (rc < 0) return rc;
  rc = snd_pcm_sw_params(pcm, sw.p);
  if (rc < 0) return rc;

  out->sampleRate = rate;
  out->channels = channels;
  out->periodFrames = static_cast<std::uint32_t>(period);
  out->bufferFrames = static_cast<std::uint32_t>(buffer);
  return 0;
}

}  // namespace snd::platform::detail
