# Bundled third-party code (CONTRACTS.md D-56). Only libopus, and only when
# SND_CODEC_OPUS is ON. See third_party/opus/SONUS_README.md for the version,
# the files that were left out and the build settings.
#
# The library is built from source with this file (the upstream build system
# is not bundled). Sonus' warning flags and -Werror are NOT applied: this is
# not our code. The sanitizer flags of the configuration are applied so that
# ASan / UBSan / TSan also see what libopus does inside a decoder slot.

if(NOT SND_CODEC_OPUS)
  return()
endif()

set(_opus_dir ${CMAKE_CURRENT_SOURCE_DIR}/opus)

# Reads "VAR = \ file \ file ..." lists from the upstream *_sources.mk files
# (the same lists the upstream autotools / CMake / meson builds use).
function(_snd_opus_sources mkfile var out)
  file(READ ${_opus_dir}/${mkfile} _text)
  # The block runs from "VAR =" to the next empty line.
  string(REGEX MATCH "(^|\n)${var} =[^\n]*(\n[^\n]+)*" _block "${_text}")
  if(NOT _block)
    message(FATAL_ERROR "libopus: ${var} not found in ${mkfile}")
  endif()
  string(REGEX REPLACE "(^|\n)${var} =" "" _block "${_block}")
  string(REPLACE "\\" "" _block "${_block}")
  string(REGEX MATCHALL "[A-Za-z0-9_/]+\\.c" _files "${_block}")
  list(TRANSFORM _files PREPEND ${_opus_dir}/)
  set(${out} ${_files} PARENT_SCOPE)
endfunction()

_snd_opus_sources(celt_sources.mk CELT_SOURCES _celt)
_snd_opus_sources(silk_sources.mk SILK_SOURCES _silk)
_snd_opus_sources(silk_sources.mk SILK_SOURCES_FLOAT _silk_float)
_snd_opus_sources(opus_sources.mk OPUS_SOURCES _opus)
_snd_opus_sources(opus_sources.mk OPUS_SOURCES_FLOAT _opus_float)
# Ambisonics (projection, mapping family 2/3) is not used by Sonus.
list(FILTER _opus EXCLUDE REGEX "opus_projection_(en|de)coder\\.c$|mapping_matrix\\.c$")

add_library(sonus_opus STATIC ${_celt} ${_silk} ${_silk_float} ${_opus} ${_opus_float})
add_library(sonus::opus ALIAS sonus_opus)
target_include_directories(sonus_opus
  PUBLIC  ${_opus_dir}/include
  PRIVATE ${_opus_dir} ${_opus_dir}/celt ${_opus_dir}/silk ${_opus_dir}/silk/float)
target_compile_definitions(sonus_opus PRIVATE
  OPUS_BUILD
  VAR_ARRAYS          # scratch arrays live on the stack (C99 VLAs): no malloc, thread-safe
  ENABLE_HARDENING    # upstream default: cheap internal consistency checks
  HAVE_LRINTF HAVE_LRINT
  PACKAGE_VERSION="1.5.2")
# Floating-point build, portable C: no OPUS_X86_* / OPUS_ARM_* intrinsics and no
# run-time CPU detection (identical results on every x86-64 / ARM64 machine).
# -w: libopus is not held to Sonus' warning policy. -ffp-contract=off: no FMA
# contraction, so encodes are reproducible across -mfma / non-FMA builds.
target_compile_options(sonus_opus PRIVATE -w -ffp-contract=off ${SND_SANITIZE_FLAGS})
set_target_properties(sonus_opus PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
