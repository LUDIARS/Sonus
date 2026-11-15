snd_add_test(base_test
  SOURCES
    base_smoke_test.cpp
    memory_test.cpp
    handle_test.cpp
    queue_test.cpp
    notify_test.cpp
  LINK sonus_base sonus_mocks)

# Every public header of base compiles on its own.
snd_add_header_checks(base_header_check
  HEADERS
    snd/base/allocator.hpp snd/base/arena.hpp snd/base/atomic.hpp snd/base/base.hpp
    snd/base/base_module.hpp snd/base/chain.hpp snd/base/handle.hpp snd/base/hash.hpp
    snd/base/log.hpp snd/base/mpmc.hpp snd/base/mutex.hpp snd/base/notify.hpp
    snd/base/pool.hpp snd/base/sparse_set.hpp snd/base/spsc.hpp
    snd/base/crc32c.hpp snd/base/endian.hpp
  LINK sonus_base)

# --- Independent (adversarial) tests: expected to pass --------------------------------
snd_add_test(base_adv_queue_test SOURCES adv_queue_test.cpp LINK sonus_base sonus_mocks)
snd_add_test(base_adv_notify_test SOURCES adv_notify_test.cpp LINK sonus_base sonus_mocks)
snd_add_test(base_adv_tables_test SOURCES adv_tables_test.cpp LINK sonus_base sonus_mocks)

# --- Independent tests that reproduce defects (fail until the defect is fixed) ----------
foreach(_defect HANDLE SNAPSHOT ALLOC SIZE LOG)
  string(TOLOWER ${_defect} _lower)
  snd_add_test(base_defect_${_lower}_test SOURCES defect_base_test.cpp LINK sonus_base sonus_mocks)
  target_compile_definitions(base_defect_${_lower}_test PRIVATE SND_DEFECT_${_defect}=1)
endforeach()

# Generic parts moved into base from the modules (stage 1.5, A1-12).
snd_add_test(base_common_parts_test SOURCES common_parts_test.cpp LINK sonus_base)
