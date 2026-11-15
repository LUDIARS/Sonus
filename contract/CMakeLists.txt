# Frozen contract (design.md 12.2). Header-only. Changing any file below is a
# contract change: spec/interface/CONTRACTS.md describes the procedure.
#
#   sonus_contract_public  public C/C++ API + extension interfaces (include/snd/*.h, snd/ext/*)
#   sonus_contract         public + internal IF, schema, commands, formats

add_library(sonus_contract_public INTERFACE)
add_library(sonus::contract_public ALIAS sonus_contract_public)
target_include_directories(sonus_contract_public INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/public/include)
target_compile_features(sonus_contract_public INTERFACE cxx_std_20 c_std_11)

add_library(sonus_contract INTERFACE)
add_library(sonus::contract ALIAS sonus_contract)
target_include_directories(sonus_contract INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/internal/include)
target_link_libraries(sonus_contract INTERFACE sonus_contract_public)

set(SND_PUBLIC_C_HEADERS
  snd/version.h
  snd/id.h
  snd/types.h
  snd/ext.h
  snd/config.h
  snd/sonus.h
  snd/physics.h)
set(SND_PUBLIC_CXX_HEADERS
  snd/id.hpp
  snd/sonus.hpp
  snd/physics.hpp
  snd/ext/types.hpp
  snd/ext/allocator.hpp
  snd/ext/job_system.hpp
  snd/ext/file_system.hpp
  snd/ext/codec.hpp
  snd/ext/dsp_effect.hpp
  snd/ext/output_device.hpp)
set(SND_INTERNAL_HEADERS
  snd/internal/rt_check.hpp
  snd/internal/common.hpp
  snd/internal/schema.hpp
  snd/internal/commands.hpp
  snd/internal/notify.hpp
  snd/internal/source_if.hpp
  snd/internal/music_if.hpp
  snd/format/bank_format.hpp
  snd/format/livelink_protocol.hpp)
set(SND_CONTRACT_HEADERS ${SND_PUBLIC_C_HEADERS} ${SND_PUBLIC_CXX_HEADERS} ${SND_INTERNAL_HEADERS}
    CACHE INTERNAL "frozen contract headers")

if(SND_BUILD_TESTS)
  # Each header alone in its own translation unit (C++20; .h also as C11).
  snd_add_header_checks(sonus_contract_header_checks
    HEADERS ${SND_PUBLIC_C_HEADERS} ${SND_PUBLIC_CXX_HEADERS} ${SND_INTERNAL_HEADERS}
    C_TOO
    LINK sonus_contract)
  add_subdirectory(test)
endif()
