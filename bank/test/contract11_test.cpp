// Contract 1.1 in the bank manager: SND_ERR_BANK_ALREADY_LOADED (D-50), the
// language name rules of snd_declare_language (D-50).
#include <cstring>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/mock/deterministic_job_system.hpp"
#include "snd/mock/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"
