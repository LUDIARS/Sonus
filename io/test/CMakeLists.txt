snd_add_test(io_test
  SOURCES
    io_smoke_test.cpp
    stream_pool_test.cpp
    io_scheduler_test.cpp
    default_file_system_test.cpp
  LINK sonus_io sonus_jobs sonus_mocks)

# Threaded stress test in its own executable (TSan run: -DSND_SANITIZE=thread).
snd_add_test(io_stress_test
  SOURCES io_stress_test.cpp
  LINK sonus_io sonus_jobs sonus_mocks)

# Every public header of io compiles on its own (with only base + contract visible).
snd_add_header_checks(io_header_check
  HEADERS
    snd/io/io.hpp snd/io/io_module.hpp snd/io/stream_pool.hpp snd/io/io_scheduler.hpp
    snd/io/stream_read_planner.hpp snd/io/default_file_system.hpp
  LINK sonus_io)

# Independent (adversarial) tests: model-based, edge cases, contract checks.
snd_add_test(io_adv_test
  SOURCES
    io_model_test.cpp
    io_scheduler_edge_test.cpp
  LINK sonus_io sonus_jobs sonus_mocks)
