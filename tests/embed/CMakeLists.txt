# Sonus as a subproject (CONTRACTS.md D-100): ctest configures and builds the small parent
# project in host/ (C++17, exceptions, -Werror) that takes Sonus in with add_subdirectory,
# then runs its two programs (C API on a silent device; the tool's core without Tela
# connected to it over the live link). The nested build uses this build's compilers,
# generator and build type, with Sonus' defaults for a subproject (no sanitizers).
add_test(NAME embed_subproject
         COMMAND ${CMAKE_COMMAND}
                 -DSONUS_DIR=${PROJECT_SOURCE_DIR}
                 -DHOST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/host
                 -DBIN_DIR=${CMAKE_CURRENT_BINARY_DIR}/host-build
                 -DGENERATOR=${CMAKE_GENERATOR}
                 -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                 -DC_COMPILER=${CMAKE_C_COMPILER}
                 -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                 -DBUILD_TYPE=$<CONFIG>
                 -DCODEC_OPUS=${SND_CODEC_OPUS}
                 -DLIVELINK=${SND_LIVELINK}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_embed.cmake)
# The nested build compiles Sonus again (2 jobs).
set_tests_properties(embed_subproject PROPERTIES TIMEOUT 3600 PROCESSORS 2)
