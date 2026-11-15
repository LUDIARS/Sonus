snd_add_test(jobs_test
  SOURCES
    jobs_smoke_test.cpp
    job_contract_test.cpp
    worker_pool_test.cpp
    rt_workers_test.cpp
    job_pump_test.cpp
  LINK sonus_jobs sonus_mocks)

snd_add_header_checks(jobs_header_check
  HEADERS
    snd/jobs/jobs_module.hpp snd/jobs/rt_workers.hpp snd/jobs/worker_pool.hpp snd/jobs/job_service.hpp
  LINK sonus_jobs)

# --- Independent (adversarial) tests ------------------------------------------------------
snd_add_test(jobs_adv_rt_workers_test SOURCES adv_rt_workers_test.cpp LINK sonus_jobs sonus_mocks)
snd_add_test(jobs_adv_worker_pool_test SOURCES adv_worker_pool_test.cpp LINK sonus_jobs sonus_mocks)
# Reproduces a defect (fails, intermittently): IsQuiescent() can be true while a
# worker is about to run a Late chunk it already claimed.
snd_add_test(jobs_defect_rt_quiescent_test SOURCES defect_rt_quiescent_test.cpp LINK sonus_jobs sonus_mocks)
# Reproduces a defect (fails intermittently): remaining_ decremented after the Done CAS.
snd_add_test(jobs_defect_rt_remaining_test SOURCES defect_rt_remaining_test.cpp LINK sonus_jobs sonus_mocks)

# Contract 1.1 D-47: RT worker stack (stage 1.5 integration).
snd_add_test(jobs_rt_stack_test SOURCES rt_stack_test.cpp LINK sonus_jobs sonus_mocks)

# --- Independent tests, contract 1.1 / stage 1.5 integration ---------------------------
# Defect_* fail on purpose: D-43's "StragglersInFlight() == 0" does not cover the
# late chunk of the last Run (rt_workers.hpp SAFE POINT).
snd_add_test(Defect_jobs_d43_stragglers_test SOURCES defect_d43_stragglers_test.cpp LINK sonus_jobs sonus_mocks)
snd_add_test(jobs_indep_rt_contract11_test SOURCES indep_rt_contract11_test.cpp LINK sonus_jobs sonus_mocks)
