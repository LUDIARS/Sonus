# Helpers for module, test and header-check targets.
#
# Module layout (every module directory under the repository root):
#   <module>/CMakeLists.txt          calls snd_add_module(<module> SOURCES ...)
#   <module>/include/snd/<module>/   headers other modules may include
#   <module>/src/                    private implementation
#   <module>/test/CMakeLists.txt     calls snd_add_test(...)
#
# Dependencies come from SND_MODULE_DEPS_<module> in the root CMakeLists.txt
# (design.md 12.1). They are linked PRIVATE except for the foundation modules
# `base` and `platform`, which propagate (everyone needs handles, SPSC, SIMD).
# Consequence: a module's public headers may only expose types from the
# contract, base and platform. Anything else is an implementation detail.

set(SND_FOUNDATION_MODULES base platform)

function(snd_add_module name)
  cmake_parse_arguments(M "HEADER_ONLY" "" "SOURCES" ${ARGN})
  set(target sonus_${name})
  if(NOT DEFINED SND_MODULE_DEPS_${name})
    message(FATAL_ERROR "Module '${name}' is not in the 12.1 dependency table (root CMakeLists.txt)")
  endif()

  add_library(${target} STATIC ${M_SOURCES})
  add_library(sonus::${name} ALIAS ${target})
  target_include_directories(${target}
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  snd_apply_options(${target})

  if(DEFINED SND_MODULE_CONTRACT_${name} AND SND_MODULE_CONTRACT_${name} STREQUAL "public")
    target_link_libraries(${target} PUBLIC sonus_contract_public)
  else()
    target_link_libraries(${target} PUBLIC sonus_contract)
  endif()

  foreach(dep IN LISTS SND_MODULE_DEPS_${name})
    if(dep IN_LIST SND_FOUNDATION_MODULES)
      target_link_libraries(${target} PUBLIC sonus_${dep})
    else()
      target_link_libraries(${target} PRIVATE sonus_${dep})
    endif()
  endforeach()

  set_property(GLOBAL APPEND PROPERTY SND_ALL_MODULES ${name})

  if(SND_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
    add_subdirectory(test)
  endif()
endfunction()

# snd_add_test(<name> SOURCES <files...> [LINK <targets...>] [GOLDEN_DIR <dir>] [NO_RT_HOOKS])
# Builds one test executable using the in-repo runner and registers it with CTest.
# GOLDEN_DIR: directory holding reference audio (*.sndg) for snd::test::CompareGolden().
function(snd_add_test name)
  cmake_parse_arguments(T "NO_RT_HOOKS" "GOLDEN_DIR" "SOURCES;LINK" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  snd_apply_options(${name})
  target_link_libraries(${name} PRIVATE sonus_testkit_main sonus_testkit ${T_LINK})
  if(NOT T_NO_RT_HOOKS)
    target_link_libraries(${name} PRIVATE sonus_testkit_rthooks)
  endif()
  if(T_GOLDEN_DIR)
    target_compile_definitions(${name} PRIVATE SND_GOLDEN_DIR="${T_GOLDEN_DIR}")
  else()
    target_compile_definitions(${name} PRIVATE SND_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

# snd_add_header_checks(<target> BASE_DIR <dir> HEADERS <relative paths...> [C_TOO] [LINK <targets>])
# Generates one translation unit per header that includes only that header
# (self-containment check). With C_TOO, .h headers are also compiled as C11.
function(snd_add_header_checks target)
  cmake_parse_arguments(H "C_TOO" "BASE_DIR" "HEADERS;LINK" ${ARGN})
  set(srcs "")
  foreach(h IN LISTS H_HEADERS)
    string(MAKE_C_IDENTIFIER "${h}" id)
    set(cpp "${CMAKE_CURRENT_BINARY_DIR}/${target}/${id}.cpp")
    file(CONFIGURE OUTPUT "${cpp}" CONTENT "#include <${h}>\n#include <${h}> /* include guard check */\nint snd_header_check_${id}_cpp = 0;\n")
    list(APPEND srcs "${cpp}")
    if(H_C_TOO AND h MATCHES "\\.h$")
      set(c "${CMAKE_CURRENT_BINARY_DIR}/${target}/${id}.c")
      file(CONFIGURE OUTPUT "${c}" CONTENT "#include <${h}>\n#include <${h}>\nint snd_header_check_${id}_c = 0;\n")
      list(APPEND srcs "${c}")
    endif()
  endforeach()
  add_library(${target} OBJECT ${srcs})
  snd_apply_options(${target})
  target_link_libraries(${target} PRIVATE ${H_LINK})
endfunction()
