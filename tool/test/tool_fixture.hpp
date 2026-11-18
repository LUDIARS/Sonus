// Fixture for the tool's tests: a small project on disk (built with the build
// tool), a real core System with the live link server over the in-memory
// transport (tests/integration/livelink_fixture.hpp), and helpers to run the
// tool's Session against it.
//
// Project "Mix" (bank "Main"):
//   buses  Master (master, meter), Sfx (parent Master, effect snd.fx.delay
//          [Time 10, Feedback 0, DryDb 0, WetDb -96, DampingHz 20000, PingPong 0,
//          MaxDelay 100]), Music (parent Master)
//   category SE; parameter Knob (global, 0..1, default 0.25)
//   asset "dc": mono 48 kHz PCMF, 4800 frames of kDc
//   events Tone (Sfx, category SE, loops), Pad (Music, loops), Blip (Sfx, once)
// With the delay's wet path at -96 dB the steady output of a DC input is
// kDc * gains * (1 + 10^(-96/20)): bus and category volumes act exactly.
#ifndef SND_TOOL_TEST_TOOL_FIXTURE_HPP
#define SND_TOOL_TEST_TOOL_FIXTURE_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "livelink_fixture.hpp"
#include "snd/buildtool/wav.hpp"
#include "snd/platform/clock.hpp"
#include "snd/tool/session.hpp"

namespace tooltest {

namespace fs = std::filesystem;
namespace bt = snd::buildtool;
using lltest::kDc;

inline double Ratio(float db) { return std::pow(10.0, db / 20.0); }
inline std::uint64_t Now() { return snd::platform::MonotonicNanoseconds(); }

class ProjectDir {
 public:
  explicit ProjectDir(const std::string& name) {
    static int counter = 0;
    std::error_code ec;
    root_ = fs::temp_directory_path(ec) /
            ("sonus-tool-" + name + "-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    fs::remove_all(root_, ec);
    fs::create_directories(dir(), ec);
  }
  ~ProjectDir() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  ProjectDir(const ProjectDir&) = delete;
  ProjectDir& operator=(const ProjectDir&) = delete;
  fs::path root() const { return root_; }
  fs::path dir() const { return root_ / "project"; }
  fs::path out() const { return root_ / "out"; }
  void Write(const std::string& rel, const std::string& text) const {
    std::error_code ec;
    fs::create_directories((dir() / rel).parent_path(), ec);
    std::ofstream f(dir() / rel, std::ios::binary | std::ios::trunc);
    f << text;
  }
  void WriteBytes(const std::string& rel, const std::vector<std::uint8_t>& b) const {
    std::error_code ec;
    fs::create_directories((dir() / rel).parent_path(), ec);
    std::ofstream f(dir() / rel, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  }

 private:
  fs::path root_;
};

inline std::vector<std::uint8_t> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string G(std::uint32_t n) {
  char b[48];
  std::snprintf(b, sizeof b, "7001c000-0000-4000-8000-%012x", n);
  return b;
}

struct MixVariant {
  float sfxDb = 0.0f;           // Sfx volumeDb in the project
  bool extraBus = false;        // adds bus "Extra" (not in the default bank)
};

inline void WriteMixProject(const ProjectDir& p, const MixVariant& v = MixVariant{}) {
  p.Write("project.json", "{\"type\": \"project\", \"guid\": \"" + G(1) + "\", \"name\": \"Mix\", \"formatVersion\": 1}");
  p.Write("banks/Main.json", "{\"type\": \"bank\", \"guid\": \"" + G(2) + "\", \"name\": \"Main\"}");
  p.Write("buses/Master.json", "{\"type\": \"bus\", \"guid\": \"" + G(3) +
                                   "\", \"name\": \"Master\", \"bank\": \"Main\", \"master\": true, \"meter\": true}");
  char sfx[512];
  std::snprintf(sfx, sizeof sfx,
                "{\"type\": \"bus\", \"guid\": \"%s\", \"name\": \"Sfx\", \"bank\": \"Main\", \"parent\": \"Master\", "
                "\"meter\": true, \"volumeDb\": %g, \"effects\": [{\"type\": \"snd.fx.delay\", "
                "\"params\": [10, 0, 0, -96, 20000, 0, 100]}]}",
                G(4).c_str(), static_cast<double>(v.sfxDb));
  p.Write("buses/Sfx.json", sfx);
  p.Write("buses/Music.json", "{\"type\": \"bus\", \"guid\": \"" + G(5) +
                                  "\", \"name\": \"Music\", \"bank\": \"Main\", \"parent\": \"Master\", \"meter\": true}");
  if (v.extraBus) {
    p.Write("buses/Extra.json", "{\"type\": \"bus\", \"guid\": \"" + G(6) +
                                    "\", \"name\": \"Extra\", \"bank\": \"Main\", \"parent\": \"Master\"}");
  }
  p.Write("categories/SE.json", "{\"type\": \"category\", \"guid\": \"" + G(7) + "\", \"name\": \"SE\", \"bank\": \"Main\"}");
  p.Write("parameters/Knob.json", "{\"type\": \"parameter\", \"guid\": \"" + G(8) +
                                      "\", \"name\": \"Knob\", \"bank\": \"Main\", \"min\": 0, \"max\": 1, \"default\": 0.25}");
  bt::PcmData pcm;
  pcm.channels = 1;
  pcm.sampleRate = 48000;
  pcm.frames = 4800;
  pcm.samples.assign(4800, kDc);
  p.WriteBytes("audio/dc.wav", bt::WriteWav(pcm, bt::WavSampleFormat::Float32));
  p.Write("assets/dc.json", "{\"type\": \"asset\", \"guid\": \"" + G(9) +
                                "\", \"name\": \"dc\", \"bank\": \"Main\", \"file\": \"audio/dc.wav\", \"loadForm\": "
                                "\"pcm\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}}");
  auto event = [&](std::uint32_t g, const char* name, const char* bus, const char* extra, int loops) {
    char b[512];
    std::snprintf(b, sizeof b,
                  "{\"type\": \"event\", \"guid\": \"%s\", \"name\": \"%s\", \"bank\": \"Main\", \"bus\": \"%s\"%s, "
                  "\"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": %d}]}]}",
                  G(g).c_str(), name, bus, extra, loops);
    p.Write(std::string("events/") + name + ".json", b);
  };
  event(10, "Tone", "Sfx", ", \"category\": \"SE\"", 0);
  event(11, "Pad", "Music", "", 0);
  event(12, "Blip", "Sfx", "", 1);
}

// A built project and a live game running its bank.
class LiveRig {
 public:
  explicit LiveRig(const MixVariant& gameProject = MixVariant{}, snd::livelink::ServerDesc sd = {})
      : proj_("rig") {
    WriteMixProject(proj_, gameProject);
    ok = gameProj.Load(proj_.dir().string()) && gameProj.Build(proj_.out().string());
    if (!ok || gameProj.banks().empty()) {
      for (const auto& i : gameProj.issues()) std::printf("project: %s %s\n", i.where.c_str(), i.message.c_str());
      ok = false;
      return;
    }
    bank = ReadAll(gameProj.banks()[0].path);
    f.Load(bank, "Main.sndb");
    ok = f.StartServer(sd) == SND_OK;
  }

  // Plays `event` and renders until the output settles.
  snd_playback_handle Play(const char* event) {
    const snd_playback_handle h = f.sys->Play(snd::HashId(event), snd_emitter_handle{0}, nullptr);
    f.Steps(16);
    return h;
  }
  snd::tool::ConnectionDesc Conn(std::uint64_t hash) {
    snd::tool::ConnectionDesc d;
    d.connector = &f.link.connector();
    d.projectBuildHash = hash;
    d.reconnectDelayMs = 20;
    d.name = "tool-test";
    return d;
  }
  // Renders blocks and ticks the session until pred() (true) or the timeout. The timeout is
  // long on purpose: under sanitizers and parallel ctest a few seconds are not enough.
  template <class Pred>
  bool Run(snd::tool::Session& s, Pred&& pred, std::uint32_t timeoutMs = 60000) {
    return f.StepUntil([&] {
      s.Tick(Now());
      return pred();
    }, timeoutMs);
  }
  // Renders `blocks` more blocks while ticking.
  void Settle(snd::tool::Session& s, std::uint32_t blocks = 8) {
    for (std::uint32_t i = 0; i < blocks; ++i) {
      s.Tick(Now());
      f.Step();
    }
  }
  float Level() const { return f.LastLeft(); }

  ProjectDir proj_;
  snd::tool::Project gameProj;
  std::vector<std::uint8_t> bank;
  lltest::Fixture f;
  bool ok = false;
};

// The same project as the game's (or a variant) for the tool, built in its own directory.
struct ToolProject {
  explicit ToolProject(const MixVariant& v = MixVariant{}) : dir("toolproj") {
    WriteMixProject(dir, v);
    ok = project.Load(dir.dir().string()) && project.Build(dir.out().string());
  }
  ProjectDir dir;
  snd::tool::Project project;
  bool ok = false;
};

// Index of a bus / category / parameter by name.
inline std::int32_t Bus(const snd::tool::Project& p, const char* name) { return p.FindBus(snd::HashId(name)); }
inline std::int32_t Cat(const snd::tool::Project& p, const char* name) { return p.FindCategory(snd::HashId(name)); }

}  // namespace tooltest

#endif  // SND_TOOL_TEST_TOOL_FIXTURE_HPP
