// Independent tests of group P (tool/test/indep_s7_live_properties.md): fixture.
//
// Project "Indep" (DC material: every level is analytic):
//   bank Main   buses Master (master), Sfx (parent Master, volumeDb -1), Amb (parent Master, volumeDb -2)
//               category Cat; parameter Knob (0..1, default 0.5)
//               asset dc (mono 48 kHz PCMF, 4800 frames of kDc, looped)
//               events Tone (bus Sfx, loops), Hum (bus Amb, loops), Spot (3D, master,
//               maxDistance 20, loops)
//   bank Aux    parameter Knob2 (0..10, default 1)
// Rig: the preview engine (Sonus::tool_preview, Manual output), a Connection over the in-process
// transport and LiveSync on the document; GameCell reads a cell of the game (ReadCell, 1.1).
// MiniGame: a hand-driven game end (protocol minor, Hello status, BankList, Ack results and
// holding), for answers a real runtime does not give on demand.
#ifndef SND_TOOL_TEST_INDEP_S7_LIVE_FIXTURE_HPP
#define SND_TOOL_TEST_INDEP_S7_LIVE_FIXTURE_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doc_fixture.hpp"
#include "snd/base/allocator.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/client.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/memory_transport.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/connection.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/live_sync.hpp"
#include "snd/tool/preview/preview_engine.hpp"
#include "snd/version.h"

namespace ilive {

using doctest::G;
namespace ll = snd::livelink;
namespace bf = snd::bank_format;
namespace json = snd::buildtool::json;

inline constexpr float kDc = 0.25f;
inline double DbRatio(double db) { return std::pow(10.0, db / 20.0); }
inline std::uint64_t Now() { return snd::platform::MonotonicNanoseconds(); }

inline std::string MasterG() { return G(202); }
inline std::string SfxG() { return G(203); }
inline std::string AmbG() { return G(204); }
inline std::string ToneG() { return G(210); }
inline std::string HumG() { return G(211); }
inline std::string SpotG() { return G(212); }
inline std::string Knob2G() { return G(220); }

inline json::Value Json(const std::string& text) {
  json::Value v;
  json::ParseError e{};
  json::Parse(text, &v, &e);
  return v;
}

inline std::map<std::string, std::string> Files(bool withAux = true) {
  std::map<std::string, std::string> f;
  f["project.json"] = "{\"type\": \"project\", \"guid\": \"" + G(200) + "\", \"name\": \"Indep\", \"formatVersion\": 1}";
  f["banks/Main.json"] = "{\"type\": \"bank\", \"guid\": \"" + G(201) + "\", \"name\": \"Main\"}";
  f["buses/Master.json"] = "{\"type\": \"bus\", \"guid\": \"" + MasterG() + "\", \"name\": \"Master\", \"bank\": \"Main\", \"master\": true}";
  f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + SfxG() + "\", \"name\": \"Sfx\", \"bank\": \"Main\", \"parent\": \"Master\", "
                        "\"volumeDb\": -1}";
  f["buses/Amb.json"] = "{\"type\": \"bus\", \"guid\": \"" + AmbG() + "\", \"name\": \"Amb\", \"bank\": \"Main\", \"parent\": \"Master\", "
                        "\"volumeDb\": -2}";
  f["categories/Cat.json"] = "{\"type\": \"category\", \"guid\": \"" + G(205) + "\", \"name\": \"Cat\", \"bank\": \"Main\"}";
  f["parameters/Knob.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(206) +
                              "\", \"name\": \"Knob\", \"bank\": \"Main\", \"min\": 0, \"max\": 1, \"default\": 0.5}";
  f["assets/dc.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(207) + "\", \"name\": \"dc\", \"bank\": \"Main\", \"file\": \"audio/dc.wav\", "
                        "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}, \"loop\": {\"start\": 0, \"end\": 4800}}";
  f["events/Tone.json"] = "{\"type\": \"event\", \"guid\": \"" + ToneG() + "\", \"name\": \"Tone\", \"bank\": \"Main\", \"bus\": \"Sfx\", "
                          "\"category\": \"Cat\", \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["events/Hum.json"] = "{\"type\": \"event\", \"guid\": \"" + HumG() + "\", \"name\": \"Hum\", \"bank\": \"Main\", \"bus\": \"Amb\", "
                         "\"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["events/Spot.json"] = "{\"type\": \"event\", \"guid\": \"" + SpotG() + "\", \"name\": \"Spot\", \"bank\": \"Main\", \"is3D\": true, "
                          "\"maxDistance\": 20, \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  if (withAux) {
    f["banks/Aux.json"] = "{\"type\": \"bank\", \"guid\": \"" + G(219) + "\", \"name\": \"Aux\"}";
    f["parameters/Knob2.json"] = "{\"type\": \"parameter\", \"guid\": \"" + Knob2G() +
                                 "\", \"name\": \"Knob2\", \"bank\": \"Aux\", \"min\": 0, \"max\": 10, \"default\": 1}";
  }
  return f;
}

inline void Load(snd::tool::ProjectDocument& doc, bool withAux = true) {
  doc.LoadFiles(Files(withAux));
  doc.SetBinaryFile("audio/dc.wav", doctest::ToneWav(4800, 48000, 1, kDc));
  doc.SetRandom(doctest::Counter{});
}

inline snd::tool::LiveSyncOptions Options(snd::tool::LiveDecision d = snd::tool::LiveDecision::Ask) {
  snd::tool::LiveSyncOptions o;
  o.decision = d;
  o.throttleMs = 0;
  return o;
}

// Says yes to every structural edit and changes nothing (core has no editor yet, C-01).
class AcceptAll final : public ll::IStructEditor {
 public:
  snd_result Apply(snd::core::System&, const ll::StructEditRequest&) noexcept override {
    ++applied;
    return SND_OK;
  }
  snd_result Undo(snd::core::System&, const ll::StructEditRequest&) noexcept override {
    ++undone;
    return SND_OK;
  }
  std::uint32_t applied = 0, undone = 0;
};

inline float F32Of(std::uint64_t bits) {
  const std::uint32_t u = static_cast<std::uint32_t>(bits);
  float f = 0.0f;
  std::memcpy(&f, &u, 4);
  return f;
}

// The preview engine started from `gameDoc`'s build; the tool edits `doc` (the same project
// unless the test changes one of them first).
struct Rig {
  snd::tool::ProjectDocument doc;
  snd::tool::DocBuild gameBuild;
  std::unique_ptr<snd::tool::preview::PreviewEngine> engine;
  snd::tool::Connection conn;
  std::unique_ptr<snd::tool::LiveSync> sync;
  std::vector<float> out;
  bool ok = false;

  // `gameEdit` changes the document before the game's build is made (then it is undone).
  explicit Rig(snd::tool::preview::PreviewEngineDesc ed = {}, snd::tool::LiveSyncOptions so = Options(),
               const std::function<void(snd::tool::ProjectDocument&)>& gameEdit = nullptr, bool withAux = true) {
    Load(doc, withAux);
    if (gameEdit) {
      snd::tool::ProjectDocument g;
      Load(g, withAux);
      gameEdit(g);
      gameBuild = snd::tool::BuildDocument(g);
    } else {
      gameBuild = snd::tool::BuildDocument(doc);
    }
    if (!gameBuild.ok) {
      for (const auto& i : gameBuild.issues) std::printf("build: %s\n", i.text.c_str());
      return;
    }
    std::string err;
    engine = snd::tool::preview::PreviewEngine::Create(ed, &err);
    if (engine == nullptr) {
      std::printf("engine: %s\n", err.c_str());
      return;
    }
    if (!engine->LoadBuild(gameBuild, &err)) {
      std::printf("load: %s\n", err.c_str());
      return;
    }
    snd::tool::preview::PreviewEngine* e = engine.get();
    if (!so.writer) {
      so.writer = [e](const snd::tool::BankImage& b, std::string* path, std::string*) {
        *path = e->PutRebuiltBank(b.file, b.bytes);
        return true;
      };
    }
    sync = std::make_unique<snd::tool::LiveSync>(doc, so);
    if (!gameEdit) sync->SetBaseline(gameBuild);
    ok = true;
  }
  ~Rig() { conn.Close(); }
  snd::tool::ConnectionDesc Desc() {
    snd::tool::ConnectionDesc d;
    d.connector = engine->connector();
    d.reconnectDelayMs = 20;
    d.name = "indep-live";
    d.profilerMask = 0;
    return sync->Prepare(d);
  }
  bool Connect() { return conn.Open(Desc()) == SND_OK; }
  void Frame() {
    out.clear();
    engine->Render(1, &out);
    const snd::tool::ConnectionEvents ev = conn.Poll();
    sync->Tick(conn, ev, Now());
  }
  template <class Pred>
  bool Run(Pred&& pred, std::uint32_t timeoutMs = 30000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      Frame();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  }
  void Frames(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Frame();
  }
  // Nothing left to send and the game answered everything; then a few blocks for the output.
  bool Settle() {
    Frame();
    const bool r = Run([&] { return sync->phase() == snd::tool::LivePhase::Live; });
    Frames(6);
    return r;
  }
  float Left() const { return out.empty() ? 0.0f : out[out.size() - 2]; }
  float Right() const { return out.empty() ? 0.0f : out[out.size() - 1]; }

  // A cell of the game, read back (ReadCell): false if no answer.
  bool GameCell(snd::Id id, std::uint32_t chunk, std::uint32_t column, std::uint32_t index, ll::CellValue* v) {
    ll::SetValue cell{};
    cell.runtimeId = id;
    cell.chunk = chunk;
    cell.column = column;
    cell.index = index;
    ll::Client* c = conn.client();
    if (c == nullptr) return false;
    const std::uint32_t seq = c->ReadCell(cell);
    return seq != 0u && Run([&] { return c->TakeCell(seq, v); }, 10000);
  }
  // The f32 value of a cell in the game (NaN if it cannot be read).
  float GameF32(snd::Id id, std::uint32_t chunk, std::uint32_t column, std::uint32_t index = 0) {
    ll::CellValue v{};
    if (!GameCell(id, chunk, column, index, &v) || v.result != SND_OK) return std::nanf("");
    return F32Of(v.current);
  }
  // The BankList of the game (QueryBanks).
  std::vector<ll::BankEntry> GameBanks() {
    std::vector<ll::BankEntry> b(64);
    ll::Client* c = conn.client();
    if (c == nullptr) return {};
    const std::uint32_t seq = c->QueryBanks(false);
    if (seq == 0u) return {};
    std::uint32_t reply = 0, n = 0;
    if (!Run([&] {
          n = c->CopyBanks(b.data(), 64u, &reply);
          return reply == seq;
        }, 10000)) {
      return {};
    }
    b.resize(n);
    return b;
  }
};

// Every f32 live cell (SetValue target) of the tool's current build that the runtime writes:
// (id, chunk, column, index, value). Used to compare the game's cells with the document's build.
struct LiveCell {
  snd::Id id;
  std::uint32_t chunk, column, index;
  float value;
};
inline std::vector<LiveCell> F32CellsOf(const snd::tool::DocBuild& b, const snd::tool::ProjectDocument& doc,
                                        const std::vector<std::pair<std::string, std::string>>& members) {
  std::vector<LiveCell> out;
  for (const auto& [guid, path] : members) {
    for (const snd::tool::CellRef& c : snd::tool::LocateField(b, doc, guid, path)) {
      if (!c.addressable || !c.liveWritable || c.type != snd::tool::CellType::F32) continue;
      out.push_back(LiveCell{c.runtimeId, c.chunk, c.column, c.index, F32Of(c.bits)});
    }
  }
  return out;
}

// A music project: Tune (bus Amb) plays Calm (playlist LA: segment SA, DC kDc) or Fight (LB: SB,
// DC 3 kDc); 3/4 at 90 BPM (a bar is 2 s); one rule "* -> *" at the next bar, no fades.
inline std::map<std::string, std::string> MusicFiles() {
  std::map<std::string, std::string> f = Files(false);
  f["assets/ma.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(230) + "\", \"name\": \"ma\", \"bank\": \"Main\", \"file\": \"audio/ma.wav\", "
                        "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}}";
  f["assets/mb.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(231) + "\", \"name\": \"mb\", \"bank\": \"Main\", \"file\": \"audio/mb.wav\", "
                        "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}}";
  // 3/4 at 90 BPM: a bar is 2 s.
  const char* tempo = "\"tempo\": [{\"tick\": 0, \"bpm\": 90, \"beatsPerBar\": 3, \"beatUnit\": 4}]";
  f["segments/SA.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(232) + "\", \"name\": \"SA\", \"bank\": \"Main\", \"asset\": \"ma\", "
                          "\"bars\": 4, " + tempo + "}";
  f["segments/SB.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(233) + "\", \"name\": \"SB\", \"bank\": \"Main\", \"asset\": \"mb\", "
                          "\"bars\": 4, " + tempo + "}";
  f["playlists/LA.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(234) + "\", \"name\": \"LA\", \"bank\": \"Main\", "
                           "\"entries\": [{\"segment\": \"SA\"}]}";
  f["playlists/LB.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(235) + "\", \"name\": \"LB\", \"bank\": \"Main\", "
                           "\"entries\": [{\"segment\": \"SB\"}]}";
  f["events/Tune.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(236) + "\", \"name\": \"Tune\", \"bank\": \"Main\", \"bus\": \"Amb\", \"music\": {"
      "\"initialState\": \"Calm\", \"states\": [{\"name\": \"Calm\", \"playlist\": \"LA\"}, {\"name\": \"Fight\", \"playlist\": \"LB\"}],"
      "\"transitions\": [{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\"}]}}";
  return f;
}

// The preview engine alone (Manual output) on the project (or the music project).
struct Engine {
  snd::tool::ProjectDocument doc;
  snd::tool::DocBuild build;
  std::unique_ptr<snd::tool::preview::PreviewEngine> e;
  std::vector<float> out;
  explicit Engine(bool music = false, const std::function<void(snd::tool::ProjectDocument&)>& edit = nullptr,
                  snd::tool::preview::PreviewEngineDesc d = {}) {
    if (music) {
      doc.LoadFiles(MusicFiles());
      doc.SetBinaryFile("audio/dc.wav", doctest::ToneWav(4800, 48000, 1, kDc));
      doc.SetBinaryFile("audio/ma.wav", doctest::ToneWav(10 * 48000, 48000, 1, kDc));
      doc.SetBinaryFile("audio/mb.wav", doctest::ToneWav(10 * 48000, 48000, 1, 3.0f * kDc));
      doc.SetRandom(doctest::Counter{});
    } else {
      Load(doc);
    }
    if (edit) edit(doc);
    build = snd::tool::BuildDocument(doc);
    if (!build.ok) {
      for (const auto& i : build.issues) std::printf("build: %s\n", i.text.c_str());
      return;
    }
    std::string err;
    e = snd::tool::preview::PreviewEngine::Create(d, &err);
    if (e == nullptr || !e->LoadBuild(build, &err)) {
      std::printf("engine: %s\n", err.c_str());
      e.reset();
    }
  }
  void Frames(std::uint32_t n) {
    out.clear();
    e->Render(n, &out);
  }
  float L() const { return out.empty() ? 0.0f : out[out.size() - 2]; }
  float R() const { return out.empty() ? 0.0f : out[out.size() - 1]; }
};
// --- a hand-driven game ------------------------------------------------------------------------
struct MiniFrame {
  ll::FrameHeader h{};
  std::vector<std::uint8_t> p;
  template <class T>
  T As() const {
    T t{};
    std::memcpy(&t, p.data(), p.size() < sizeof(T) ? p.size() : sizeof(T));
    return t;
  }
  bool Is(ll::MessageType t) const { return h.type == static_cast<std::uint16_t>(t); }
};

class MiniGame final : public ll::FrameReader::Handler {
 public:
  MiniGame() { ok = link.Init(snd::base::DefaultAllocator(), 1u << 20) && reader_.Init(snd::base::DefaultAllocator(), 1u << 20); }
  ~MiniGame() override { Drop(); }
  MiniGame(const MiniGame&) = delete;
  MiniGame& operator=(const MiniGame&) = delete;

  std::uint16_t minor = ll::kProtocolMinor;
  ll::HelloStatus status = ll::HelloStatus::Accepted;
  bool echoHash = true;  // runtimeBuildHash = the Hello's hash (else runtimeHash)
  std::uint64_t runtimeHash = 0x77;
  std::vector<ll::BankEntry> banks;  // the BankList answer
  bool holdAcks = false;
  std::function<std::int32_t(const MiniFrame&)> ackResult;  // null: SND_OK
  bool holdHello = false;  // keep the HelloReply until ReplyHello()
  bool holdBanks = false;  // keep the BankList until ReplyBanks()

  bool ok = false;
  std::vector<MiniFrame> frames;
  struct Held {
    std::uint32_t seq;
    std::int32_t result;
  };
  std::vector<Held> held;
  std::uint32_t accepts = 0;

  ll::MemoryLink link;
  bool connected() const { return t_ != nullptr; }
  void Drop() {
    if (t_ != nullptr) link.acceptor().Release(t_);
    t_ = nullptr;
    pendingDrop_ = false;
  }
  void Pump() {
    if (t_ == nullptr) {
      t_ = link.acceptor().Accept(0);
      if (t_ == nullptr) return;
      reader_.Reset();
      ++accepts;
    }
    for (int guard = 0; guard < 64 && t_ != nullptr; ++guard) {
      std::uint8_t buf[8192];
      std::size_t got = 0;
      const ll::IoStatus st = t_->Recv(buf, sizeof buf, &got);
      if (st == ll::IoStatus::Ok) {
        reader_.Feed(buf, got, *this);
        if (pendingDrop_) Drop();
        continue;
      }
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) Drop();
      break;
    }
    if (t_ != nullptr && Now() - lastPing_ > 200000000ull) {
      lastPing_ = Now();
      const ll::Ping ping{Now()};
      Send(ll::MessageType::Ping, &ping, sizeof ping);
    }
  }
  bool Send(ll::MessageType type, const void* payload, std::uint32_t n) {
    if (t_ == nullptr) return false;
    std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(type), 0, n, ++seqOut_);
    if (n != 0u) std::memcpy(b.data() + ll::kFrameHeaderBytes, payload, n);
    const std::uint8_t* p = b.data();
    std::size_t left = b.size();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (left != 0u) {
      std::size_t sent = 0;
      const ll::IoStatus st = t_->Send(p, left, &sent);
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) return false;
      p += sent;
      left -= sent;
      if (left != 0u) {
        if (std::chrono::steady_clock::now() > end) return false;
        t_->Wait(false, true, 2);
      }
    }
    return true;
  }
  void SendAck(std::uint32_t seq, std::int32_t result) {
    const ll::Ack a{seq, result};
    Send(ll::MessageType::Ack, &a, sizeof a);
  }
  void Release() {
    for (const Held& h : held) SendAck(h.seq, h.result);
    held.clear();
  }
  std::vector<MiniFrame> Of(ll::MessageType t) const {
    std::vector<MiniFrame> out;
    for (const MiniFrame& f : frames) {
      if (f.Is(t)) out.push_back(f);
    }
    return out;
  }

  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* payload) noexcept override {
    MiniFrame f;
    f.h = h;
    f.p.assign(payload, payload + h.payloadBytes);
    if (f.Is(ll::MessageType::Ping)) {
      const ll::Pong pong{f.As<ll::Ping>().senderTimeNs, 0u};
      Send(ll::MessageType::Pong, &pong, sizeof pong);
      return;
    }
    if (f.Is(ll::MessageType::Pong)) return;
    frames.push_back(f);
    if (f.Is(ll::MessageType::Hello)) {
      if (holdHello) {
        heldHelloHash_ = f.As<ll::Hello>().projectBuildHash;
        helloHeld_ = true;
        return;
      }
      ll::HelloReply r{};
      r.protocolMajor = ll::kProtocolMajor;
      r.protocolMinor = minor;
      r.status = static_cast<std::uint32_t>(status);
      r.runtimeBuildHash = echoHash ? f.As<ll::Hello>().projectBuildHash : runtimeHash;
      r.contractVersion = SND_CONTRACT_VERSION;
      r.sampleRate = 48000;
      r.blockSize = 256;
      Send(ll::MessageType::HelloReply, &r, sizeof r);
      return;
    }
    if (f.Is(ll::MessageType::SyncDecision)) {
      if (f.As<ll::SyncDecision>().mode == static_cast<std::uint32_t>(ll::SyncMode::Disconnect)) pendingDrop_ = true;
      return;
    }
    if (f.Is(ll::MessageType::QueryBanks) && minor >= 1u && holdBanks) {
      heldBankSeqs_.push_back(h.seq);
    } else if (f.Is(ll::MessageType::QueryBanks) && minor >= 1u) {
      SendBanks(h.seq);
    }
    if ((h.flags & ll::kFrameWantsAck) != 0u) {
      const std::int32_t result = ackResult ? ackResult(f) : SND_OK;
      if (holdAcks) {
        held.push_back(Held{h.seq, result});
      } else {
        SendAck(h.seq, result);
      }
    }
  }
  void OnOversized(const ll::FrameHeader&) noexcept override {}

  // Answers a held Hello (status / hash as set now; echoHash echoes the Hello's hash).
  bool helloHeld() const { return helloHeld_; }
  void ReplyHello() {
    if (!helloHeld_) return;
    helloHeld_ = false;
    ll::HelloReply r{};
    r.protocolMajor = ll::kProtocolMajor;
    r.protocolMinor = minor;
    r.status = static_cast<std::uint32_t>(status);
    r.runtimeBuildHash = echoHash ? heldHelloHash_ : runtimeHash;
    r.contractVersion = SND_CONTRACT_VERSION;
    r.sampleRate = 48000;
    r.blockSize = 256;
    Send(ll::MessageType::HelloReply, &r, sizeof r);
  }
  std::uint64_t heldHelloHash() const { return heldHelloHash_; }
  std::size_t banksAsked() const { return heldBankSeqs_.size(); }
  void ReplyBanks() {
    for (std::uint32_t q : heldBankSeqs_) SendBanks(q);
    heldBankSeqs_.clear();
  }
  // The BankList answering QueryBanks `seq` (the reply carries the request's seq, protocol §8.1).
  void SendBanks(std::uint32_t seq) {
    if (t_ == nullptr) return;
    std::vector<std::uint8_t> b(sizeof(ll::ListHeader) + banks.size() * sizeof(ll::BankEntry));
    const ll::ListHeader lh{static_cast<std::uint32_t>(banks.size()), static_cast<std::uint32_t>(sizeof(ll::BankEntry))};
    std::memcpy(b.data(), &lh, sizeof lh);
    if (!banks.empty()) std::memcpy(b.data() + sizeof lh, banks.data(), banks.size() * sizeof(ll::BankEntry));
    std::vector<std::uint8_t> fr(ll::kFrameHeaderBytes + b.size());
    ll::EncodeHeader(fr.data(), static_cast<std::uint16_t>(ll::MessageType::BankList), 0, static_cast<std::uint32_t>(b.size()), seq);
    std::memcpy(fr.data() + ll::kFrameHeaderBytes, b.data(), b.size());
    std::size_t sent = 0;
    t_->Send(fr.data(), fr.size(), &sent);
  }

 private:
  bool helloHeld_ = false;
  std::uint64_t heldHelloHash_ = 0;
  std::vector<std::uint32_t> heldBankSeqs_;
  ll::ITransport* t_ = nullptr;
  ll::FrameReader reader_;
  std::uint32_t seqOut_ = 0;
  std::uint64_t lastPing_ = 0;
  bool pendingDrop_ = false;
};

}  // namespace ilive

#endif  // SND_TOOL_TEST_INDEP_S7_LIVE_FIXTURE_HPP
