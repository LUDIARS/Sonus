// main() of platform_device_play_test: it plays sound, so it runs only when
// SND_DEVICE_TEST=1; otherwise it exits with 77 (CTest SKIP_RETURN_CODE).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "snd/test/test.hpp"

int main(int argc, char** argv) {
  const char* v = std::getenv("SND_DEVICE_TEST");
  if (v == nullptr || std::strcmp(v, "1") != 0) {
    std::printf("SKIP: plays sound on the real output device; set SND_DEVICE_TEST=1 to run\n");
    return 77;
  }
  return snd::test::RunAll(argc, argv);
}
