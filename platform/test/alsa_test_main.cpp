// main() of platform_alsa_test: exits with 77 (CTest SKIP_RETURN_CODE) when
// libasound cannot open the private null PCM, otherwise runs the tests.
#include <cstdio>

#include "snd/test/test.hpp"

bool AlsaNullPcmUsable();

int main(int argc, char** argv) {
  if (!AlsaNullPcmUsable()) {
    std::printf("SKIP: libasound cannot open a private 'null' PCM (snd_pcm_open_lconf); ALSA tests not run\n");
    return 77;
  }
  return snd::test::RunAll(argc, argv);
}
