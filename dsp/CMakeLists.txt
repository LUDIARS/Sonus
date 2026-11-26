# dsp module (design.md 12.1). Owner: dsp team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_dsp).
set(SND_DSP_SOURCES
  src/dsp_module.cpp
  src/kernels.cpp
  src/resampler.cpp
  src/filter.cpp
  src/pan.cpp
  src/meter.cpp
  src/effects.cpp
  src/fx_reverb.cpp
  src/fx_delay.cpp
  src/fx_dynamics.cpp
  src/fx_filter.cpp
  # Stage 7-D: FFT, time stretch, convolution (D-208..D-215).
  src/fft.cpp
  src/time_stretch.cpp
  src/fx_convolution.cpp)
snd_add_module(dsp SOURCES ${SND_DSP_SOURCES})
