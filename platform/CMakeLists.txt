# platform module (design.md 12.1). Owner: base/platform/jobs team (stage 2: output backends).
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_platform).
#
# Output backends are chosen at build time (CONTRACTS.md D-61):
#   Linux    ALSA        SND_OUTPUT_ALSA = AUTO (use if found) | ON (require) | OFF
#   Windows  WASAPI      always (ole32, avrt)
#   macOS    Core Audio  always (AudioToolbox, CoreAudio, CoreFoundation)
#   iOS      Core Audio  always (+ AVFoundation; src/apple/*.mm is Objective-C++ with ARC)
# The silent timer device is always built.
set(SND_OUTPUT_ALSA "AUTO" CACHE STRING "ALSA output backend on Linux: AUTO, ON or OFF")
set_property(CACHE SND_OUTPUT_ALSA PROPERTY STRINGS AUTO ON OFF)

set(_snd_platform_sources
  src/platform_module.cpp
  src/clock_thread.cpp
  src/semaphore.cpp
  src/cpu.cpp
  src/platform_output_device.cpp
  src/timer_output_device.cpp
  src/output_device_factory.cpp
  src/load_monitor.cpp
  src/driver_core.cpp
  src/audio_driver.cpp
  src/workgroup_common.cpp
  src/common/device_callback.cpp
  src/common/sample_format.cpp
  src/common/stream_policy.cpp)

set(SND_PLATFORM_ALSA_ENABLED OFF)
set(_snd_platform_libs "")
set(_snd_platform_defs "")
if(WIN32)
  list(APPEND _snd_platform_sources
    src/windows/mmcss_workgroup.cpp
    src/windows/wasapi_format.cpp
    src/windows/wasapi_notification_client.cpp
    src/windows/wasapi_enumerate.cpp
    src/windows/wasapi_output_device.cpp
    src/windows/wide_string.cpp)
  list(APPEND _snd_platform_libs ole32 avrt)
elseif(APPLE)
  list(APPEND _snd_platform_sources
    src/apple/os_workgroup.cpp
    src/apple/coreaudio_output_device.cpp
    src/apple/coreaudio_unit.cpp)
  list(APPEND _snd_platform_libs "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND _snd_platform_sources src/apple/ios_audio_session.mm)
    list(APPEND _snd_platform_libs "-framework AVFoundation" "-framework Foundation")
  else()
    list(APPEND _snd_platform_sources src/apple/macos_device_watch.cpp src/apple/macos_devices.cpp)
  endif()
else()
  list(APPEND _snd_platform_sources src/workgroup_generic.cpp)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT SND_OUTPUT_ALSA STREQUAL "OFF")
    if(SND_OUTPUT_ALSA STREQUAL "ON")
      find_package(ALSA REQUIRED)
    else()
      find_package(ALSA QUIET)
    endif()
    if(ALSA_FOUND)
      set(SND_PLATFORM_ALSA_ENABLED ON)
      list(APPEND _snd_platform_sources
        src/linux/alsa_pcm.cpp
        src/linux/alsa_enumerate.cpp
        src/linux/alsa_output_device.cpp)
      list(APPEND _snd_platform_libs ALSA::ALSA)
      list(APPEND _snd_platform_defs SND_PLATFORM_HAVE_ALSA=1)
      message(STATUS "Sonus: ALSA output backend enabled (${ALSA_LIBRARY})")
    else()
      message(STATUS "Sonus: ALSA not found, ALSA output backend disabled (SND_OUTPUT_ALSA=AUTO)")
    endif()
  endif()
endif()
# Visible to platform/test/CMakeLists.txt (added from snd_add_module below).
set(SND_PLATFORM_ALSA_ENABLED ${SND_PLATFORM_ALSA_ENABLED} CACHE INTERNAL "ALSA backend compiled into sonus_platform")

snd_add_module(platform SOURCES ${_snd_platform_sources})
find_package(Threads REQUIRED)
target_link_libraries(sonus_platform PUBLIC Threads::Threads)
if(_snd_platform_libs)
  target_link_libraries(sonus_platform PRIVATE ${_snd_platform_libs})
endif()
if(_snd_platform_defs)
  target_compile_definitions(sonus_platform PRIVATE ${_snd_platform_defs})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # Objective-C++ with ARC (the root enables OBJCXX on iOS). The CXX-only
  # options of snd_apply_options do not reach OBJCXX sources, so repeat them.
  set_source_files_properties(src/apple/ios_audio_session.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra;-Werror;-fno-exceptions;-fno-rtti")
endif()

# SIMD abstraction (header only, snd/platform/simd.hpp): force the scalar path everywhere.
option(SND_SIMD_SCALAR "Force the scalar SIMD path (snd/platform/simd.hpp)" OFF)
if(SND_SIMD_SCALAR)
  target_compile_definitions(sonus_platform PUBLIC SND_SIMD_FORCE_SCALAR=1)
endif()
