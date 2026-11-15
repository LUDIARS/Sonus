snd_add_test(platform_test
  SOURCES platform_smoke_test.cpp
  LINK sonus_platform sonus_mocks)

# clock, threads, semaphore, FTZ/DAZ, CPU features, timer output device (base/platform/jobs team)
snd_add_test(platform_runtime_test
  SOURCES platform_runtime_test.cpp
  LINK sonus_platform sonus_mocks)
snd_add_header_checks(platform_header_check
  HEADERS
    snd/platform/clock.hpp snd/platform/thread.hpp snd/platform/semaphore.hpp
    snd/platform/cpu.hpp snd/platform/output_device.hpp snd/platform/platform_module.hpp
  LINK sonus_platform)

# --- Independent (adversarial) tests ------------------------------------------------------
snd_add_test(platform_adv_test SOURCES adv_platform_test.cpp LINK sonus_platform sonus_mocks)
# Reproduces a defect (fails until fixed): deadline arithmetic of the timer device wraps.
snd_add_test(platform_defect_timer_test SOURCES defect_timer_test.cpp LINK sonus_platform sonus_mocks)
