cmake_minimum_required(VERSION 3.24)

project(sonus
  VERSION 0.1.0
  DESCRIPTION "Sonus: data-oriented sound runtime for games"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SND_BUILD_TESTS     "Build unit tests, mocks and the test framework" ON)
option(SND_NO_EXCEPTIONS   "Compile C++ with -fno-exceptions -fno-rtti" ON)
option(SND_WERROR          "Treat warnings as errors" ON)
option(SND_LIVELINK        "Compile the live link (development builds only)" ON)
option(SND_BUILD_TOOL      "Build the authoring GUI (not available yet)" OFF)
set(SND_SANITIZE "" CACHE STRING "Sanitizers: empty, 'address,undefined' or 'thread'")
# RT safety detection hooks (spec 14). ON by default when tests are built,
# compiled out (zero cost) otherwise. See contract/internal/include/snd/internal/rt_check.hpp.
if(SND_BUILD_TESTS)
  set(_snd_rt_default ON)
else()
  set(_snd_rt_default OFF)
endif()
option(SND_RT_CHECKS "Enable RT-safety detection hooks" ${_snd_rt_default})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SonusOptions)
include(SonusModule)

if(SND_BUILD_TESTS)
  enable_testing()
endif()

# ---------------------------------------------------------------------------
# Module dependency table (design.md 12.1). FROZEN: changing it is a contract
# change (spec/interface/CONTRACTS.md). Module CMakeLists never list their own
# dependencies; snd_add_module() reads them from here.
# ---------------------------------------------------------------------------
set(SND_MODULE_DEPS_base      "")
set(SND_MODULE_DEPS_platform  "base")
set(SND_MODULE_DEPS_jobs      "base;platform")
set(SND_MODULE_DEPS_io        "base;jobs")
set(SND_MODULE_DEPS_codec     "base")
set(SND_MODULE_DEPS_dsp       "base;platform")
set(SND_MODULE_DEPS_bank      "base;io")
set(SND_MODULE_DEPS_core      "base;jobs;dsp;bank")
set(SND_MODULE_DEPS_source    "core;codec;io")
set(SND_MODULE_DEPS_music     "core")
set(SND_MODULE_DEPS_api       "core;music")
set(SND_MODULE_DEPS_physics   "api")
set(SND_MODULE_DEPS_livelink  "core")
set(SND_MODULE_DEPS_buildtool "codec")
set(SND_MODULE_DEPS_tool      "buildtool;dsp;core;livelink")
# Which contract layer each module may include. "public" = include/snd/*.h,
# snd/ext/*; "full" = public + snd/internal/* + snd/format/*.
set(SND_MODULE_CONTRACT_physics "public")

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(contract)
if(SND_BUILD_TESTS)
  add_subdirectory(tests)            # framework + mocks first: module tests use them
endif()

foreach(_m base platform jobs io codec dsp bank core source music api physics livelink buildtool)
  add_subdirectory(${_m})
endforeach()

if(SND_BUILD_TOOL)
  add_subdirectory(tool)
endif()

if(SND_BUILD_TESTS)
  add_subdirectory(tests/integration)
endif()
