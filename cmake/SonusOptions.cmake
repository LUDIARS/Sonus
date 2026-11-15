# Compiler options shared by every Sonus target.
#
# snd_apply_options(<target>) adds warnings, the exception/RTTI policy,
# sanitizers and the RT-check definition. Applied by snd_add_module(),
# snd_add_test() and the contract checks.

set(SND_C_WARNINGS   -Wall -Wextra -Wpedantic)
set(SND_CXX_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                     -Wcast-align -Woverloaded-virtual)
if(SND_WERROR)
  list(APPEND SND_C_WARNINGS -Werror)
  list(APPEND SND_CXX_WARNINGS -Werror)
endif()

set(SND_SANITIZE_FLAGS "")
if(SND_SANITIZE)
  set(SND_SANITIZE_FLAGS "-fsanitize=${SND_SANITIZE}" -fno-omit-frame-pointer)
  if(SND_SANITIZE MATCHES "undefined")
    # vptr checks need RTTI; the library is built without it.
    list(APPEND SND_SANITIZE_FLAGS -fno-sanitize=vptr -fno-sanitize-recover=all)
  endif()
  # Sanitizer runtimes interpose malloc/pthread themselves; the test kit then
  # only uses operator new hooks and explicit SND_RT_NOTE_* hooks.
  add_compile_definitions(SND_SANITIZER_ACTIVE=1)
  add_link_options(${SND_SANITIZE_FLAGS})
endif()

if(SND_RT_CHECKS)
  add_compile_definitions(SND_RT_CHECKS=1)
else()
  add_compile_definitions(SND_RT_CHECKS=0)
endif()
if(SND_LIVELINK)
  add_compile_definitions(SND_LIVELINK=1)
else()
  add_compile_definitions(SND_LIVELINK=0)
endif()

function(snd_apply_options target)
  # Determinism (design.md 2.5): no implicit FMA contraction by the compiler.
  # Explicit FMA intrinsics (snd/platform/simd.hpp AVX2+FMA / NEON) are unaffected.
  target_compile_options(${target} PRIVATE -ffp-contract=off)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${SND_C_WARNINGS}>
    $<$<COMPILE_LANGUAGE:CXX>:${SND_CXX_WARNINGS}>
    ${SND_SANITIZE_FLAGS})
  if(SND_NO_EXCEPTIONS)
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
  endif()
endfunction()
