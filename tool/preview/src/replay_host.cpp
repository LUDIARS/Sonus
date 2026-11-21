// The replay host (design.md 2.5, 11.3), group P.
#include "snd/tool/preview/replay_host.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>

#include "runtime_host.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/base/allocator.hpp"
#include "snd/buildtool/wav.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/replayer.hpp"

namespace snd::tool::preview {
namespace ll = livelink;
namespace fs = std::filesystem;
namespace bf = bank_format;

std::vector<ReportBank> BanksOf(const DocBuild& build) {
  std::vector<ReportBank> out;
  for (const BankImage& b : build.banks) {
    ReportBank r;
    r.file = b.file;
    r.bankId = b.bankId;
    const ParsedBank* v = b.view();
    r.languageId = v != nullptr ? v->languageId() : 0u;
    r.contentHash = b.contentHash;
    r.bytes = b.bytes;
    out.push_back(std::move(r));
  }
  return out;
}

bool ReadBankDirectory(const std::string& dir, std::vector<ReportBank>* out, std::string* error) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && e.path().extension() == ".sndb") files.push_back(e.path());
  }
  if (ec) {
    if (error != nullptr) *error = "cannot read " + dir + ": " + ec.message();
    return false;
  }
  std::sort(files.begin(), files.end());
  out->clear();
  for (const fs::path& p : files) {
    std::ifstream in(p, std::ios::binary);
    ReportBank b;
    b.file = p.filename().string();
    b.bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.bytes.size() < sizeof(bf::FileHeader)) {
      if (error != nullptr) *error = "not a bank: " + p.string();
      return false;
    }
    bf::FileHeader h{};
    std::memcpy(&h, b.bytes.data(), sizeof h);
    b.bankId = h.bankId;
    b.languageId = (h.flags & bf::kFileLanguageBank) != 0u ? h.languageId : 0u;
    b.contentHash = h.contentHash;
    out->push_back(std::move(b));
  }
  return true;
}

namespace {

// Loads the non-language banks, dependencies first.
bool LoadBanks(RuntimeHost& host, const std::vector<ReportBank>& banks, std::uint32_t timeoutMs, std::string* error) {
  std::vector<const ReportBank*> left;
  for (const ReportBank& b : banks) {
    if (b.languageId == 0u) left.push_back(&b);
  }
  std::set<Id> done;
  while (!left.empty()) {
    bool progress = false;
    for (std::size_t i = 0; i < left.size();) {
      bool ready = true;
      const std::vector<std::uint8_t>& bytes = left[i]->bytes;
      if (bytes.size() >= sizeof(bf::FileHeader)) {
        bf::FileHeader h{};
        std::memcpy(&h, bytes.data(), sizeof h);
        for (std::uint32_t d = 0; d < h.dependencyCount; ++d) {
          const std::uint64_t at = h.dependencyOffset + std::uint64_t{d} * sizeof(bf::Dependency);
          if (at + sizeof(bf::Dependency) > bytes.size()) break;
          bf::Dependency dep{};
          std::memcpy(&dep, bytes.data() + at, sizeof dep);
          ready = ready && done.count(dep.bankId) != 0u;
        }
      }
      if (!ready && progress) {
        ++i;
        continue;
      }
      if (!ready) {
        ++i;
        continue;
      }
      host.fs().Put("replay/" + left[i]->file, left[i]->bytes);
      if (host.LoadBank("replay/" + left[i]->file, timeoutMs, error).value == 0u) return false;
      done.insert(left[i]->bankId);
      left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
      progress = true;
    }
    if (!progress) {  // missing dependency: load the rest as they are (the runtime reports it)
      for (const ReportBank* b : left) {
        host.fs().Put("replay/" + b->file, b->bytes);
        if (host.LoadBank("replay/" + b->file, timeoutMs, error).value == 0u) return false;
      }
      left.clear();
    }
  }
  return true;
}

}  // namespace

bool Replay(const Recording& rec, const std::vector<ReportBank>& banks, const ReplayOptions& o, ReplayRun* out) {
  *out = ReplayRun{};
  auto fail = [&](std::int32_t r, const std::string& m) {
    out->ok = false;
    out->result = r;
    out->error = m;
    return false;
  };
  const snd_config cfg = o.haveConfig ? o.config : ReplayConfigFor(rec);
  snd_config replayCfg = cfg;
  replayCfg.livelink.enabled = 0;
  ll::ReplayDesc rd;
  rd.config = &replayCfg;
  rd.allocator = &base::DefaultAllocator();
  rd.record = rec.records.data();
  rd.bytes = rec.records.size();
  rd.structEditor = o.structEditor;
  rd.applyStartState = o.applyStartState;
  rd.maxDecisions = o.maxDecisions;
  ll::Replayer* r = nullptr;
  const int cr = ll::Replayer::Create(rd, &r);
  if (cr != SND_OK) {
    return fail(cr, cr == SND_ERR_CORRUPT            ? "the recording is malformed"
                    : cr == SND_ERR_VERSION_MISMATCH ? "the recording's fallback capacities differ from the Config"
                                                     : "the replay could not be set up");
  }
  struct Guard {
    ll::Replayer* r;
    ~Guard() { ll::Replayer::Destroy(r); }
  } guard{r};
  RuntimeHost host;
  RuntimeHostDesc hd;
  hd.config = replayCfg;
  hd.hooks = HostHooks::Replay;
  hd.replayer = r;
  hd.output = HostOutput::Manual;
  std::string err;
  if (host.Create(hd, &err) != SND_OK) return fail(SND_ERR_INVALID_ARGUMENT, "runtime: " + err);
  if (!LoadBanks(host, banks, o.bankTimeoutMs, &err)) return fail(SND_ERR_IO, err);
  const ll::ReplayInfo info = r->Info();
  // Keep the recorded block numbers: the recorded first block is rendered `lead` blocks after
  // Start, as the n-th block of this runtime.
  const std::uint64_t lead = info.leadBlocks;
  const std::uint32_t ch = host.Channels();
  std::vector<float> block(std::size_t{host.BlockFrames()} * ch);
  out->blockNumbersKept = host.BlocksRendered() + lead <= info.firstBlock;
  while (host.BlocksRendered() + lead < info.firstBlock) host.Step(block.data());
  r->Start();
  ll::OutputDigest digest;
  const std::uint64_t total = r->Blocks() + o.extraBlocks;
  for (std::uint64_t i = 0; i < total; ++i) {
    host.Step(block.data());
    if (i < lead) continue;
    if (i < r->Blocks()) digest.Add(block.data(), block.size());
    if (o.keepOutput) out->output.insert(out->output.end(), block.begin(), block.end());
  }
  out->blocks = total;
  out->finished = r->Finished();
  out->info = r->Info();
  out->digest = digest.value;
  out->samples = digest.samples;
  out->bitExactExpected = r->BitExactExpected();
  out->reproducesRecordedRun = r->ReproducesRecordedRun();
  out->decisions.resize(out->info.decisions);
  const std::uint32_t n = r->CopyDecisions(out->decisions.data(), static_cast<std::uint32_t>(out->decisions.size()));
  out->decisions.resize(n);
  out->channels = ch;
  out->sampleRate = host.SampleRate();
  out->ok = true;
  return true;
}

ReplayComparison ReplayTwice(const Recording& rec, const std::vector<ReportBank>& banks, const ReplayOptions& o,
                             ReplayRun* first, ReplayRun* second) {
  ReplayRun a, b;
  Replay(rec, banks, o, &a);
  Replay(rec, banks, o, &b);
  ReplayComparison c = CompareRuns(a, b);
  if (first != nullptr) *first = std::move(a);
  if (second != nullptr) *second = std::move(b);
  return c;
}

ExpectationResult CheckRecording(const Recording& rec, const std::vector<ReportBank>& banks,
                                 const ReplayExpectation& expected, const ReplayOptions& o, ReplayRun* run) {
  ReplayRun r;
  Replay(rec, banks, o, &r);
  ExpectationResult e = CheckExpectation(r, expected);
  if (run != nullptr) *run = std::move(r);
  return e;
}

ExpectationResult CheckRecordingFiles(const std::string& recordingPath, const std::string& bankDir,
                                      const std::string& expectationPath, const ReplayOptions& o) {
  ExpectationResult res;
  Recording rec;
  std::string err;
  if (!LoadRecording(recordingPath, &rec, &err)) {
    res.messages.push_back("recording: " + err);
    return res;
  }
  std::vector<ReportBank> banks;
  if (!ReadBankDirectory(bankDir, &banks, &err)) {
    res.messages.push_back("banks: " + err);
    return res;
  }
  ReplayExpectation e;
  if (!LoadExpectation(expectationPath, &e, &err)) {
    res.messages.push_back("expectation: " + err);
    return res;
  }
  return CheckRecording(rec, banks, e, o);
}

BugReport MakeBugReport(const Recording& rec, const std::vector<ReportBank>& banks, const snd_config* config,
                        const std::string& title, const std::string& notes, bool replay, const RecordedRunLog* run) {
  BugReport r;
  r.recording = rec;
  r.banks = banks;
  r.configKnown = config != nullptr;
  r.config = config != nullptr ? *config : ReplayConfigFor(rec);
  r.protocolMajor = livelink::kProtocolMajor;
  r.protocolMinor = livelink::kProtocolMinor;
  r.title = title;
  r.notes = notes;
  const RecordingDescription d = DescribeRecording(rec);
  r.summary = d.Text();
  if (replay) {
    ReplayOptions o;
    o.haveConfig = true;
    o.config = r.config;
    ReplayRun a, b;
    const ReplayComparison c = ReplayTwice(rec, banks, o, &a, &b);
    r.replayed = a.ok && b.ok;
    r.decisionsMatch = c.decisionsMatch;
    r.bitExactExpected = c.bitExactExpected;
    r.bitsMatch = c.bitsMatch;
    r.comparison = c.Text();
    if (run != nullptr) r.comparison += CompareWithRecordedRun(a, d, *run).Text();
    r.expectation = ExpectationOf(a, a.bitExactExpected);
  }
  return r;
}

ReplayComparison ReplayBugReport(const BugReport& rep, ExpectationResult* against, ReplayRun* first) {
  ReplayOptions o;
  o.haveConfig = true;
  o.config = rep.config;
  ReplayRun a, b;
  ReplayComparison c = ReplayTwice(rep.recording, rep.banks, o, &a, &b);
  if (against != nullptr) {
    if (rep.replayed) {
      *against = CheckExpectation(a, rep.expectation);
    } else {
      *against = ExpectationResult{};
      against->messages.push_back("the report has no expectation (it was not replayed when written)");
    }
  }
  if (first != nullptr) *first = std::move(a);
  return c;
}

bool WriteWav(const std::string& path, const std::vector<float>& interleaved, std::uint32_t channels,
              std::uint32_t sampleRate, std::string* error) {
  if (channels == 0u || sampleRate == 0u) {
    if (error != nullptr) *error = "no channels / rate";
    return false;
  }
  buildtool::PcmData pcm;
  pcm.channels = channels;
  pcm.sampleRate = sampleRate;
  pcm.frames = interleaved.size() / channels;
  pcm.samples = interleaved;
  pcm.samples.resize(static_cast<std::size_t>(pcm.frames) * channels);
  const std::vector<std::uint8_t> bytes = buildtool::WriteWav(pcm, buildtool::WavSampleFormat::Float32);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!f) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return true;
}

// --- OutputPlayer --------------------------------------------------------------------------------
struct OutputPlayer::Impl {
  std::vector<float> samples;
  std::uint32_t channels = 2;
  std::atomic<std::uint64_t> at{0};  // next sample (audio thread)
  platform::PlatformOutputDevice* device = nullptr;
  platform::AudioDriver driver;
  bool started = false;
  static void Render(void* user, float* out, std::uint32_t frames, std::uint32_t ch,
                     const platform::RenderInfo*) noexcept {
    Impl& p = *static_cast<Impl*>(user);
    std::uint64_t at = p.at.load(std::memory_order_relaxed);
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        float v = 0.0f;
        const std::uint64_t i = at + c;
        if (c < p.channels && i < p.samples.size()) v = p.samples[static_cast<std::size_t>(i)];
        out[std::size_t{f} * ch + c] = v;
      }
      if (at < p.samples.size()) at += p.channels;
    }
    p.at.store(at, std::memory_order_release);
  }
};

OutputPlayer::OutputPlayer() : impl_(std::make_unique<Impl>()) {}
OutputPlayer::~OutputPlayer() { Stop(); }

bool OutputPlayer::Start(std::vector<float> interleaved, std::uint32_t channels, std::uint32_t sampleRate, bool silent,
                         std::string* error) {
  Stop();
  Impl& p = *impl_;
  p.samples = std::move(interleaved);
  p.channels = channels == 0u ? 2u : channels;
  p.at.store(0, std::memory_order_release);
  IAllocator& a = base::DefaultAllocator();
  p.device = platform::CreateOutputDevice(silent ? platform::OutputBackend::SilentTimer : platform::OutputBackend::Default,
                                          a, nullptr);
  if (p.device == nullptr) {
    if (error != nullptr) *error = "no output device";
    return false;
  }
  platform::AudioDriverDesc ad;
  ad.core.blockFrames = 256;
  ad.core.render = &Impl::Render;
  ad.core.user = &p;
  ad.realtimeThread = false;
  const DeviceDesc dd{sampleRate, p.channels, silent ? 256u : 0u, 0u};
  if (!p.driver.Start(*p.device, dd, ad, a)) {
    platform::DestroyOutputDevice(p.device, a);
    p.device = nullptr;
    if (error != nullptr) *error = "the output device does not start";
    return false;
  }
  p.started = true;
  return true;
}

void OutputPlayer::Stop() {
  Impl& p = *impl_;
  if (p.started) {
    p.driver.Stop();
    p.started = false;
  }
  if (p.device != nullptr) {
    platform::DestroyOutputDevice(p.device, base::DefaultAllocator());
    p.device = nullptr;
  }
}

bool OutputPlayer::Done() const { return impl_->at.load(std::memory_order_acquire) >= impl_->samples.size(); }

std::uint64_t OutputPlayer::FramesPlayed() const {
  return impl_->at.load(std::memory_order_acquire) / (impl_->channels == 0u ? 1u : impl_->channels);
}

}  // namespace snd::tool::preview
