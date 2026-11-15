snd_add_test(bank_test
  SOURCES bank_smoke_test.cpp validate_test.cpp manager_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Fuzz: valid banks with random (fixed-seed) mutations; run under ASan/UBSan too.
snd_add_test(bank_fuzz_test
  SOURCES fuzz_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)
set_tests_properties(bank_fuzz_test PROPERTIES TIMEOUT 600)

# Round trip with buildtool's output (buildtool must not depend on bank, so the
# check lives here; the test executable links both).
snd_add_test(bank_roundtrip_test
  SOURCES roundtrip_test.cpp
  LINK sonus_bank sonus_buildtool sonus_mocks)
target_include_directories(bank_roundtrip_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
target_compile_definitions(bank_roundtrip_test PRIVATE
  SND_BUILDTOOL_TEST_DATA="${PROJECT_SOURCE_DIR}/buildtool/test/data")

# --- Independent tests (test owner, not the implementer) -----------------------------
# Structure-aware fuzz: re-signed boundary mutations of every structural field,
# invariants re-checked against the spec, every accessor walked (run under ASan/UBSan).
snd_add_test(bank_struct_fuzz_test
  SOURCES struct_fuzz_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_buildtool sonus_mocks)
# Differential check against buildtool's self-check (buildtool/src/self_check.hpp; since contract 1.1 both run snd/format/bank_check.hpp).
target_include_directories(bank_struct_fuzz_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/src)
set_tests_properties(bank_struct_fuzz_test PROPERTIES TIMEOUT 1200)

# Manager edge cases (cancel at every step, alloc failure at every n, queue,
# dependencies, languages). Tests named Defect_* fail on purpose (see file).
snd_add_test(bank_manager_edge_test
  SOURCES manager_edge_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Header bytes [52,128) are outside every checksum (format defect): tests named
# SpecDefect_* fail on purpose.
snd_add_test(bank_header_gap_test
  SOURCES header_gap_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Dev override layer (11.2).
snd_add_test(bank_overlay_test
  SOURCES overlay_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Real worker threads (thread-pool IJobSystem) + IO / core / game threads: run under TSan.
snd_add_test(bank_manager_thread_test
  SOURCES manager_thread_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)
find_package(Threads REQUIRED)
target_link_libraries(bank_manager_thread_test PRIVATE Threads::Threads)

# buildtool -> bank read-back: every documented project field comes back from
# the columns, streams / seek / preload, release banks, built-bank fuzz.
# Tests named Defect_* fail on purpose.
snd_add_test(bank_build_readback_test
  SOURCES build_readback_test.cpp
  LINK sonus_bank sonus_buildtool sonus_mocks)
target_include_directories(bank_build_readback_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(bank_build_readback_test PROPERTIES TIMEOUT 600)

# ParseBank targeted corner cases (chunk layout, columns, versions, cycles).
snd_add_test(bank_validate_edge_test
  SOURCES validate_edge_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Re-check of the manager fixes: cancel x failure kinds, random op sequences,
# duplicate-bank check, language guess vs header.
snd_add_test(bank_manager_fix_test
  SOURCES manager_fix_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Stage 1.5: BankManager over the io module's IoScheduler (deterministic driver).
snd_add_test(bank_scheduler_io_test
  SOURCES scheduler_io_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_io sonus_mocks)

# Stage 1.5: BankManager + IoScheduler (IO thread) + jobs::WorkerPool + memory FS
# with latency / failures. Run under TSan; repeated 20 times for acceptance.
snd_add_test(bank_scheduler_thread_test
  SOURCES scheduler_thread_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_io sonus_jobs sonus_mocks)
target_link_libraries(bank_scheduler_thread_test PRIVATE Threads::Threads)

# Contract 1.1 in the bank manager (D-46, D-50).
snd_add_test(bank_contract11_test SOURCES contract11_test.cpp LINK sonus_bank sonus_mocks)

# --- Independent tests (contract 1.1 / stage 1.5) ------------------------------------
# BankManager over IoScheduler: Shutdown at each stage, Destroy with a read in
# flight, duplicate rules, language names, image integrity.
snd_add_test(bank_indep_io11_test SOURCES indep_bank_io11_test.cpp fixture_bank.cpp LINK sonus_bank sonus_io sonus_mocks)
find_package(Threads REQUIRED)
target_link_libraries(bank_indep_io11_test PRIVATE Threads::Threads)
# Was a defect (slot order decided duplicates); fixed in c65fd9a (decision C: request order).
snd_add_test(bank_indep_duplicate_order_test SOURCES indep_bank_duplicate_order_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_io sonus_mocks)
# Real IO thread + worker pool + memory FS faults; content / duplicate / generation
# invariants checked while it runs. TSan, and ctest --repeat until-fail:20.
snd_add_test(bank_indep_io11_thread_test SOURCES indep_bank_io11_thread_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_io sonus_jobs sonus_mocks)
target_link_libraries(bank_indep_io11_thread_test PRIVATE Threads::Threads)
# The shared validator snd/format/bank_check.hpp (D-53): truncations, flips, re-signed lies.
snd_add_test(bank_indep_check11_test SOURCES indep_bank_check11_test.cpp fixture_bank.cpp LINK sonus_bank sonus_mocks)
set_tests_properties(bank_indep_check11_test PROPERTIES TIMEOUT 600)
# Was a defect; fixed in c65fd9a (decision D: wait for the old slot's Release).
snd_add_test(bank_indep_reload_unloading_test SOURCES indep_bank_reload_unloading_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_io sonus_mocks)
