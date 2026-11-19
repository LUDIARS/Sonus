# A game engine's build that takes Sonus in with add_subdirectory (CONTRACTS.md D-100).
# Configured and built by tests/embed/run_embed.cmake (ctest: embed_subproject).
#
# The parent is deliberately unlike Sonus: C++17, exceptions ON, its own warnings with
# -Werror set for the whole directory (so they reach Sonus' targets too). It checks:
#   - Sonus as a subproject builds without tests and without changing the parent's
#     C++ standard or build type;
#   - Sonus' targets still get -fno-exceptions (only theirs);
#   - Sonus::sonus brings snd/sonus.h without C++20 (embed_game.cpp is C++17);
#   - Sonus::tool_core links without Tela and carries cxx_std_20 to its users.
cmake_minimum_required(VERSION 3.24)
project(sonus_embed_host LANGUAGES C CXX)

if(NOT SONUS_DIR)
  message(FATAL_ERROR "pass -DSONUS_DIR=<Sonus source directory>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)

set(_build_type_before "${CMAKE_BUILD_TYPE}")
set(SND_BUILD_TOOL_CORE ON)
add_subdirectory(${SONUS_DIR} sonus)

# --- Sonus left the parent alone ----------------------------------------------------
if(NOT CMAKE_CXX_STANDARD EQUAL 17)
  message(FATAL_ERROR "Sonus changed the parent's CMAKE_CXX_STANDARD to ${CMAKE_CXX_STANDARD}")
endif()
if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "${_build_type_before}")
  message(FATAL_ERROR "Sonus changed CMAKE_BUILD_TYPE ('${_build_type_before}' -> '${CMAKE_BUILD_TYPE}')")
endif()
if(SND_BUILD_TESTS)
  message(FATAL_ERROR "SND_BUILD_TESTS must default to OFF in a subproject")
endif()
if(TARGET sonus_testkit OR TARGET sonus_mocks)
  message(FATAL_ERROR "a subproject must not build Sonus' test kit by default")
endif()
foreach(_t Sonus::sonus Sonus::tool_core)
  if(NOT TARGET ${_t})
    message(FATAL_ERROR "missing target ${_t}")
  endif()
endforeach()
if(TARGET sonus_tool_ui OR TARGET sonus-tool)
  message(FATAL_ERROR "SND_BUILD_TOOL_CORE alone must not build the Tela UI")
endif()
get_target_property(_opts sonus_core COMPILE_OPTIONS)
if(NOT "${_opts}" MATCHES "-fno-exceptions")
  message(FATAL_ERROR "Sonus' targets lost -fno-exceptions: ${_opts}")
endif()
get_directory_property(_dir_defs COMPILE_DEFINITIONS)
if("${_dir_defs}" MATCHES "SND_")
  message(FATAL_ERROR "Sonus leaked compile definitions into the parent: ${_dir_defs}")
endif()

# --- the game: C API only, C++17 ----------------------------------------------------
add_library(embed_game STATIC embed_game.cpp)
target_link_libraries(embed_game PRIVATE Sonus::sonus)
target_compile_options(embed_game PRIVATE -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

add_executable(embed_c_host c_host.cpp)
target_link_libraries(embed_c_host PRIVATE embed_game)

# --- the editor: the tool's core (C++20 through Sonus::tool_core) next to the game -----
add_executable(embed_tool_host tool_host.cpp)
target_link_libraries(embed_tool_host PRIVATE embed_game Sonus::tool_core)
