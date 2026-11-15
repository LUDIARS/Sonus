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
  LINK sonus_base)
