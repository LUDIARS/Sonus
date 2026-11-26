// Live reflection of the document (design.md 11.2), group P. UI independent (Sonus::tool_core).
//
// LiveSync keeps a running game (or the preview engine) in step with the editable document
// (ProjectDocument): the tool's data is the truth (11.2 "正しいデータは常にツール側にある").
//
// 1. Connecting and matching (protocol §4, §8): Prepare(desc) sets the Hello build hash to the
//    document's current build and, with LiveDecision::Ask, defers the answer to a BuildMismatch
//    (Connection::awaitingSync). On a mismatch LiveSync asks for the game's banks (QueryBanks;
//    a 1.0 game has none: the check says so) and fills check(): versions, both build hashes and
//    every bank with its verdict (same / differs / only in the tool / only in the game). Until
//    the answer, nothing is sent. Answers: SendDiffs (every difference goes out, step 2) or Refuse
//    (Connection::DecideSync(Refuse): the game drops the link, nothing is sent). LiveDecision::
//    SendDiffs / Refuse answer at once. The game waits for the answer for its handshake timeout
//    (5 s by default); after that it drops the link, the client reconnects and the question comes
//    again (pending() stays true until answered).
//    D-182: the Hello hash is the document's current build at every connection (a new client while
//    connecting: Connection::SetProjectBuildHash). The answer is remembered for a reconnect to the
//    same game only (its bank list: bank id, language, content hash; a 1.0 game: its build hash);
//    another game is asked again. A 1.1 game is never asked (nor answered from memory) before its
//    bank list is in: the phase is Checking until then and the question carries the verdict per
//    bank (G10). The connection's state, its HelloReply and its generation are read from one
//    Connection::Poll (D-182): an answer goes only to the connection it was decided for.
//    SetDecision(Ask) forgets it.
//    Reconnect: no assumption on whether the game kept the overrides. Every cell and table the tool
//    ever sent (Undone ones too) goes again with the current build's value, on top of the bank-list
//    diff (the game's content hash against the builds the tool knows): after a reconnect the game's
//    cells are the document's build. Live structural edits are not sent again (whether the game kept
//    them is not known): a bank that had them is hot reloaded, and a successful reload empties
//    structEdits() and reserve() for it.
// 2. Edits: every change of the document (an edit, Undo, Redo: its revision) is built (throttled:
//    at most one build per throttleMs while edits keep coming; the last state is always built and
//    sent: FlushNow() forces it, e.g. at the end of a drag) and compared with what the game holds
//    (DiffBuilds). Routes, per bank:
//      value cells the runtime writes live              -> SetValue (coalesced per cell)
//      a table whose shape is unchanged                  -> ReplaceChunk (one in flight)
//      buses / effects / sends added or removed          -> StructEdit (within Config.dev_reserve)
//        (a bus added in today's bank format moves the master's row, so events' bus columns change
//        too: in practice an added bus goes by hot reload, G3). Value changes of the same build in
//        those tables (bus volumes, effect parameters, send levels of the objects that stay) go as
//        SetValue right after the structural edits; anything else in them sends the bank by reload.
//      anything else, or a refused ReplaceChunk / StructEdit (UNSUPPORTED, OUT_OF_BUDGET, LIMIT)
//                                                        -> hot reload of the bank (step 3)
//    What cannot go anywhere (no reload writer, a 1.0 game, a bank the game does not have, the
//    runtime does not support it: LiveSupportOf) is listed in unsupported() with the reason.
//    Back-pressure (D-86): at most maxInFlight requests wait for their Ack; COMMAND_BUFFER_FULL,
//    NOT_READY and a busy chunk slot (LIMIT_REACHED on ReplaceChunk) are re-sent without showing,
//    never over a newer value of the same cell (D-183), until giveUpMs after the first send.
//    A value of an object a live StructEdit added (NOT_FOUND: core keeps no cell for it, C-01) goes
//    by hot reload instead of being a problem.
//    Other failures become problems() naming the edit (document revision), the object and the
//    member.
//    items(): per object (and member for value cells) whether it is Live, Queued, InFlight,
//    Reloading, Failed, Unsupported, HeldByMixer or Offline.
//    Saving the document changes nothing live. Mixer monitoring values (MixerModel) win over the
//    document for the same cell while the mixer holds them (heldByMixer): such cells are not sent
//    and show HeldByMixer; when the mixer lets go, the document's value is sent by itself (D-183).
// 3. Hot reload (8.5): the bank is built (the current document), written where the game can read
//    it (options.writer: a directory relative to the game's file system root, or the preview
//    engine's PutRebuiltBank) and ReloadBank is sent; reloads() shows the phases (Unloading,
//    Loading, Done / Failed). Edits during a reload wait and go out after it (NOT_READY is never
//    shown). A failed reload says why and that the bank is detached in the game; a rebuilt bank that
//    cannot be written is a Failed reload with the writer's reason. Either keeps phase() Failed
//    (failedBanks()) until a reload of that bank succeeds (D-184). Reloads load a bank the game does
//    not hold (kReloadLoadIfAbsent). A successful reload ends the bank's live edits (its cells,
//    tables and structural edits: reserve() counts none of them).
//    reserve(): what the live structural edits take from Config.dev_reserve (counted by the tool
//    from what the game accepted; limits from options.reserveLimits); structEdits() lists them;
//    UndoStructEdit(i) sends the inverse (RemoveBus / RemoveEffect; a send has none: reload). A Remove
//    the document sends (its Undo of a live Add) gives the Add's cost back (D-183).
// Protocol 1.0 games: no QueryBanks / ReloadBank / StructEdit / RevertValue / ReadCell
// (features() says so); SetValue and ReplaceChunk still work, the rest is listed.
//
// Threads: the UI thread (Tick and every reader). The client is thread-safe.
#ifndef SND_TOOL_LIVE_SYNC_HPP
#define SND_TOOL_LIVE_SYNC_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "snd/format/livelink_protocol.hpp"
#include "snd/tool/connection.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"

namespace snd::tool {

// --- what the runtime cannot take live yet (item 8: one place) --------------------------------
// Blocking: the change is not sent (listed). Not blocking: it is sent but has no effect in the
// current runtime (listed as a warning). Integration points: see the reasons (requests C-10, C-11,
// D-179). `bits` is the cell's new value (CURV.target, SNPV.targetKind) when known.
struct LiveSupport {
  bool supported = true;
  bool blocking = false;
  std::string reason;
};
LiveSupport LiveSupportOf(std::uint32_t fourcc, std::uint32_t column = ~0u, std::uint64_t bits = 0, bool haveBits = false);

enum class LiveDecision : std::uint8_t { Ask = 0, SendDiffs = 1, Refuse = 2 };

enum class LivePhase : std::uint8_t {
  Offline = 0,           // no connection
  Connecting = 1,        // connecting / handshake
  AwaitingDecision = 2,  // a mismatch waits for SendDiffs / Refuse
  Checking = 3,          // asking the game for its banks
  Syncing = 4,           // requests queued or waiting for answers
  Live = 5,              // the game holds the document's build
  Refused = 6,           // the user refused the mismatch (nothing is sent)
  // D-184: something the game should hold did not reach it: a hot reload failed (the bank may be
  // detached in the game) or its rebuilt bank could not be written. failedBanks() says which and
  // why; the phase stays Failed until a reload of that bank succeeds (or a new session finds it).
  Failed = 7,
};
const char* LivePhaseName(LivePhase p);

enum class BankVerdict : std::uint8_t { Same = 0, Differs = 1, ToolOnly = 2, GameOnly = 3 };
struct BankMatch {
  std::string file;  // the tool's bank file ("" for GameOnly)
  Id bankId = 0;
  Id languageId = 0;
  BankVerdict verdict = BankVerdict::Same;
  std::uint64_t toolContentHash = 0, gameContentHash = 0;
  std::uint64_t toolBuildHash = 0, gameBuildHash = 0;
  bool writable = true;    // the game's bank has the dev override layer
  bool knownBase = false;  // the game's bank is a build the tool knows: value-level diffs
};
struct SyncCheck {
  bool valid = false;          // filled for the current session
  std::uint16_t peerMajor = 0, peerMinor = 0;
  std::uint32_t helloStatus = 0;  // livelink::HelloStatus
  std::uint64_t toolBuildHash = 0, gameBuildHash = 0;
  bool banksKnown = false;     // the game sent its BankList (1.1)
  std::vector<BankMatch> banks;
  std::string note;            // e.g. "the game speaks protocol 1.0: no bank list"
};

enum class LiveRoute : std::uint8_t { None = 0, SetValue = 1, ReplaceChunk = 2, StructEdit = 3, Reload = 4 };
const char* LiveRouteName(LiveRoute r);
enum class LiveState : std::uint8_t {
  Live = 0,         // the game holds the document's value
  Queued = 1,       // waiting to be sent (also: during a reload)
  InFlight = 2,     // sent, waiting for the Ack
  Reloading = 3,    // its bank is being hot reloaded
  Failed = 4,       // refused (problems())
  Unsupported = 5,  // cannot be sent (unsupported())
  HeldByMixer = 6,  // the mixer's monitoring value holds the cell
  Offline = 7,      // not connected / refused: not sent
};
const char* LiveStateName(LiveState s);

struct LiveItem {
  std::string guid;      // object ("" = a whole bank)
  std::string kind;      // object kind
  std::string name;
  std::string member;    // member pattern for a cell ("volumeDb", "tracks[#].clips[#].startMs"), "" = object
  std::string bank;      // bank file
  LiveRoute route = LiveRoute::None;
  LiveState state = LiveState::Live;
  std::int32_t result = 0;
  std::string reason;
  std::uint64_t revision = 0;  // document revision of the last change
};

struct LiveProblem {
  std::uint64_t revision = 0;  // the edit (document revision)
  std::string guid, name, member, bank;
  std::uint32_t chunk = 0;
  LiveRoute route = LiveRoute::None;
  std::int32_t result = 0;
  std::string message;
};

struct UnsupportedItem {
  std::string bank;
  std::uint32_t chunk = 0;
  std::uint32_t column = ~0u;
  std::string guid;
  bool blocking = true;
  std::string reason;
};

struct ReloadProgress {
  std::string bank;
  Id bankId = 0;
  std::string path;              // what the game reads
  std::uint32_t seq = 0;
  livelink::ReloadPhase phase = livelink::ReloadPhase::Unloading;
  bool finished = false;
  std::int32_t result = 0;       // final (Ack)
  bool detached = false;         // failed: the bank is no longer loaded in the game
  std::string reason;
  std::uint64_t revision = 0;
};

struct StructEditInfo {
  std::string bank;
  livelink::StructOp op = livelink::StructOp::AddBus;
  std::uint32_t target = 0;
  std::vector<std::uint64_t> args;
  bool applied = false;  // the game answered OK
  bool undone = false;
  std::uint64_t revision = 0;
};
struct ReserveLimits {
  std::uint32_t buses = 16, effects = 32;  // snd_config_init's Config.dev_reserve
  std::uint64_t bytes = 1ull << 20;
};
struct ReserveUsage {
  std::uint32_t buses = 0, effects = 0;
  std::uint64_t bytes = 0;
  ReserveLimits limits;
};

struct LiveFeature {
  const char* name;
  bool available;
  std::string reason;
};

struct LiveSyncStats {
  std::uint64_t builds = 0, buildErrors = 0;
  std::uint64_t sent = 0, acked = 0, resent = 0, failed = 0, coalesced = 0;
  std::uint64_t values = 0, chunks = 0, structEdits = 0, reloads = 0;
  std::uint64_t superseded = 0;  // answers of requests a reload replaced
};

// Writes a rebuilt bank where the game reads it: true and the path (relative to the game's file
// system root) to give ReloadBank.
using ReloadWriter = std::function<bool(const BankImage& bank, std::string* gamePath, std::string* error)>;
// A directory writer: `dir` on disk, `gamePrefix` the same directory as the game sees it ("" =
// the root). Each reload gets a new name ("<prefix><name>.<n>.sndb").
ReloadWriter DirectoryReloadWriter(const std::string& dir, const std::string& gamePrefix);

struct LiveSyncOptions {
  LiveDecision decision = LiveDecision::Ask;
  std::uint32_t throttleMs = 33;
  std::uint32_t maxInFlight = 128;
  // D-183: a request the game keeps answering as busy (COMMAND_BUFFER_FULL, NOT_READY, a busy chunk
  // slot) is sent again until this much time passed since it was first sent; then it is a problem.
  std::uint32_t giveUpMs = 5000;
  ReloadWriter writer;            // null: no hot reload (listed as unsupported)
  bool structEdits = true;        // try StructEdit for bus / effect / send changes
  ReserveLimits reserveLimits;
  DocBuildOptions build;          // memory only by default
  // Cells the mixer's monitoring values hold (11.2 上書き層): not sent while true.
  std::function<bool(Id runtimeId, std::uint32_t chunk, std::uint32_t column, std::uint32_t index)> heldByMixer;
};

class LiveSync {
 public:
  explicit LiveSync(const ProjectDocument& doc, LiveSyncOptions options = {});

  // The build the game was started from (else the first build of the document): the base of
  // value-level diffs for banks the game reports with that content.
  void SetBaseline(const DocBuild& build);
  // Sets desc's Hello hash (the document's current build) and the deferred decision (Ask).
  ConnectionDesc Prepare(ConnectionDesc desc);

  // UI thread, once per frame, after Connection::Poll (pass its events).
  void Tick(Connection& connection, const ConnectionEvents& events, std::uint64_t nowNs);
  // Builds and sends the current document at the next Tick, whatever the throttle.
  void FlushNow() { flush_ = true; }

  // --- decision ---------------------------------------------------------------------------
  LivePhase phase() const { return phase_; }
  bool pending() const { return phase_ == LivePhase::AwaitingDecision; }
  const SyncCheck& check() const { return check_; }
  bool ChooseSendDiffs(Connection& connection);
  bool ChooseRefuse(Connection& connection);
  // Ask also forgets the remembered answer (D-182).
  void SetDecision(LiveDecision d) {
    options_.decision = d;
    if (d == LiveDecision::Ask) rememberedValid_ = false;
  }

  // --- state ------------------------------------------------------------------------------
  const DocBuild& current() const { return current_; }  // the latest build
  std::uint64_t builtRevision() const { return builtRevision_; }
  bool buildOk() const { return current_.ok; }
  std::vector<LiveItem> items() const;
  const LiveItem* Find(const std::string& guid, const std::string& member = std::string()) const;
  const std::vector<LiveProblem>& problems() const { return problems_; }
  void ClearProblems() { problems_.clear(); }
  const std::vector<UnsupportedItem>& unsupported() const { return unsupported_; }
  const std::vector<ReloadProgress>& reloads() const { return reloads_; }
  bool reloading() const;
  const std::vector<StructEditInfo>& structEdits() const { return structs_; }
  ReserveUsage reserve() const;
  bool UndoStructEdit(std::size_t index);
  std::vector<LiveFeature> features() const;
  // D-184: banks whose last hot reload failed (file -> reason); not empty: phase Failed.
  const std::map<std::string, std::string>& failedBanks() const { return failedBanks_; }
  std::uint32_t queued() const { return static_cast<std::uint32_t>(queue_.size()); }
  std::uint32_t inFlight() const { return static_cast<std::uint32_t>(inflight_.size()); }
  const LiveSyncStats& stats() const { return stats_; }
  // Re-sends everything the game may not hold (after problems were fixed in the game).
  void Resync() { resync_ = true; }

 private:
  struct Op {
    LiveRoute route = LiveRoute::None;
    std::uint64_t revision = 0;
    std::string bank;
    Id bankId = 0, languageId = 0;
    std::uint32_t epoch = 0;  // bank epoch (a reload replaces older requests)
    livelink::SetValue value{};
    std::string cellKey;
    std::uint32_t fourcc = 0, column = 0;
    std::vector<std::uint8_t> bytes;
    livelink::StructOp structOp = livelink::StructOp::AddBus;
    std::uint32_t target = 0;
    std::vector<std::uint64_t> args;
    std::int32_t structIndex = -1;  // structs_ entry
    std::string path;
    bool loadIfAbsent = false;
    std::int32_t reloadIndex = -1;  // reloads_ entry
    std::vector<std::string> guids;
    std::string member;
    std::uint32_t seq = 0;
    std::uint32_t resends = 0;
    std::uint64_t serial = 0;       // order of the edits (a newer value of the same cell wins, D-183)
    std::uint64_t firstSentNs = 0;  // when it was first sent (busy answers give up after giveUpMs)
    std::uint32_t row = 0;          // SetValue: the cell's row in its table (re-sent on a reconnect)
    std::uint64_t contentHash = 0;  // Reload: the content hash of the bank written
  };
  struct SentCell {
    std::string bank;
    std::uint32_t row = 0;
    livelink::SetValue value{};
  };
  bool Build(std::uint64_t nowNs);
  void OnSessionStarted(Connection& c);
  void Reconcile(Connection& c);
  void PlanDelta(const LiveDelta& d, const std::vector<BankImage>& before);
  void PlanBank(const BankImage* before, const BankImage& after, bool unknownBase);
  void Enqueue(Op op);
  void Escalate(const std::string& bank, const std::string& reason, std::uint64_t revision,
                const std::vector<std::string>* guids = nullptr);
  void Send(livelink::Client* c, std::uint64_t nowNs);
  void TakeAnswers(livelink::Client* c);
  void Settle(const Op& op, std::int32_t result);
  void Mark(const Op& op, LiveState s, std::int32_t result, const std::string& reason);
  void Problem(const Op& op, std::int32_t result, const std::string& message);
  void Unsupported(const std::string& bank, std::uint32_t chunk, std::uint32_t column, const std::string& guid,
                   bool blocking, const std::string& reason);
  const BankImage* CurrentBank(const std::string& file) const;
  std::vector<std::string> ObjectsOf(const std::string& bank) const;
  void UpdatePhase(Connection& c);
  std::string GuidText(const Guid& g) const;

  const ProjectDocument& doc_;
  LiveSyncOptions options_;
  LivePhase phase_ = LivePhase::Offline;
  SyncCheck check_;
  DocBuild current_;
  std::uint64_t builtRevision_ = ~0ull;
  std::uint64_t lastBuildNs_ = 0;
  bool flush_ = false, resync_ = false;
  bool haveBaseline_ = false;
  std::vector<BankImage> baseline_;
  std::uint64_t baselineHash_ = 0;
  std::map<std::uint64_t, BankImage> known_;   // content hash -> image (builds the game may hold)
  std::vector<BankImage> sent_;                // what the game holds once the queued requests are applied
  bool sentValid_ = false;                     // sent_ describes the game (after the session's reconcile)
  std::uint64_t sentRevision_ = 0;
  std::map<std::string, std::uint32_t> epoch_;  // bank file -> epoch
  std::deque<Op> queue_;
  std::vector<Op> inflight_;
  std::map<std::string, LiveItem> items_;      // guid + "\n" + member
  std::vector<LiveProblem> problems_;
  std::vector<UnsupportedItem> unsupported_;
  std::vector<ReloadProgress> reloads_;
  std::vector<StructEditInfo> structs_;
  std::vector<std::string> changed_;           // objects changed since the last plan
  std::uint64_t seenDocRevision_ = 0;
  bool sessionLive_ = false;   // a session is up and the reconcile ran
  bool banksAsked_ = false;
  std::uint32_t banksSeq_ = 0;
  std::uint32_t connects_ = 0;
  bool refused_ = false;
  bool decided_ = false;       // the user answered this mismatch
  std::uint16_t peerMinor_ = 0;
  LiveSyncStats stats_;
  // D-182 / D-183
  std::uint64_t serial_ = 0, nowNs_ = 0;
  std::map<std::string, std::uint64_t> latestSerial_;            // cell key -> newest edit
  std::map<std::string, SentCell> sentCells_;                    // every cell ever sent (re-sent on a new session)
  std::map<std::string, std::vector<std::uint32_t>> sentChunks_;  // bank -> tables replaced live
  std::map<std::string, Op> held_;                               // cell key -> value the mixer holds back
  std::map<std::string, std::string> failedBanks_;               // bank -> why its last reload failed
  bool rememberedValid_ = false;                                 // the answer to a mismatch, for the same game
  LiveDecision remembered_ = LiveDecision::Ask;
  std::string rememberedGame_;
  bool rememberPending_ = false;
  bool waitingForList_ = false;
  std::uint64_t seenLink_ = ~0ull;
  std::string GameSignature() const;
  void ResendSentState();
  void ReleaseHeld();
};

}  // namespace snd::tool

#endif  // SND_TOOL_LIVE_SYNC_HPP
