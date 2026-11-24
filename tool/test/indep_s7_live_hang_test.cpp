// Independent tests of group P: a recording with a huge block number (a well-formed record stream
// whose numbers are absurd: a corrupt or hostile file that passes the checksum) must not make the
// replay run for ever (P-test 記録と再生「壊れたファイル（巨大な数）」). Each replay runs in a child
// process (fork, POSIX only) that the test kills after a time limit.
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "snd/format/livelink_protocol.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/recording_report.hpp"

using namespace snd::tool;
namespace ll = snd::livelink;

namespace {

void Put(std::vector<std::uint8_t>& b, const void* p, std::size_t n) {
  const auto* c = static_cast<const std::uint8_t*>(p);
  b.insert(b.end(), c, c + n);
}
void Lod(std::vector<std::uint8_t>& b, std::uint64_t block) {
  const ll::RecordLod r{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, block};
  Put(b, &r, sizeof r);
}
void EmptyArena(std::vector<std::uint8_t>& b, std::uint64_t block) {
  const ll::RecordArena a{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block, 16u, 0u};
  Put(b, &a, sizeof a);
  const ll::ArenaSections s{0u, 0u, 0u, 0u};
  Put(b, &s, sizeof s);
}

// Runs Replay on `r` in a child process: true if it returned within `seconds`.
bool ReplayEnds(const Recording& r, int seconds, int* childStatus) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    snd::tool::ReplayRun run;
    snd::tool::preview::Replay(r, {}, snd::tool::preview::ReplayOptions{}, &run);
    ::_exit(run.ok ? 0 : 3);
  }
  if (pid < 0) return false;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  for (;;) {
    int st = 0;
    const pid_t w = ::waitpid(pid, &st, WNOHANG);
    if (w == pid) {
      *childStatus = st;
      return true;
    }
    if (std::chrono::steady_clock::now() > end) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &st, 0);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

Recording Make(std::vector<std::uint8_t> records) {
  Recording r;
  r.info.sampleRate = 48000;
  r.info.blockSize = 256;
  r.records = std::move(records);
  return r;
}

}  // namespace

// The baseline: the same shapes with small numbers replay at once.
SND_TEST(IndepS7LiveHang, SmallBlockNumbersReplayAtOnce) {
  std::vector<std::uint8_t> b;
  Lod(b, 3);
  EmptyArena(b, 3);
  EmptyArena(b, 40);
  const Recording r = Make(b);
  SND_REQUIRE(Summarize(r.records.data(), r.records.size()).valid);
  int st = 0;
  SND_CHECK(ReplayEnds(r, 60, &st));
}

SND_TEST(IndepS7LiveHang, Defect_ReplayOfAHugeBlockNumberDoesNotEnd) {
  {  // the last recorded block is huge: Blocks() is about 1e15
    std::vector<std::uint8_t> b;
    Lod(b, 0);
    EmptyArena(b, 0);
    EmptyArena(b, 1000000000000000ull);
    const Recording r = Make(b);
    SND_REQUIRE(Summarize(r.records.data(), r.records.size()).valid);
    int st = 0;
    SND_CHECK_MSG(ReplayEnds(r, 20, &st), "the replay of a recording whose last block is 1e15 was still running after 20 s");
  }
  {  // the first recorded block is huge: the replay renders up to it before Start
    std::vector<std::uint8_t> b;
    Lod(b, 1000000000000000ull);
    EmptyArena(b, 1000000000000000ull);
    const Recording r = Make(b);
    SND_REQUIRE(Summarize(r.records.data(), r.records.size()).valid);
    int st = 0;
    SND_CHECK_MSG(ReplayEnds(r, 20, &st), "the replay of a recording whose first block is 1e15 was still running after 20 s");
  }
}
