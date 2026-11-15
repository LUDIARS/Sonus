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
