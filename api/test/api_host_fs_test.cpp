// The root of the built-in file system (CONTRACTS.md D-103): Config.file_system_root
// with snd_create's file_system NULL. Bank paths are relative to the root; what would
// leave it is refused even when the file exists (absolute paths, "..", links whose
// target lies outside, drive / stream colons); links that stay inside are followed; a
// relative root is resolved once at snd_create; the configuration rules.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "api_rig.hpp"
#include "mini_project.hpp"

using namespace apitest;
namespace fsys = std::filesystem;
namespace bt = snd::buildtool;

namespace {

struct Disk {
  Disk() : proj("api-root") {
    proj.Minimal();
    bt::Diagnostics diag;
    built = proj.Build(&diag);
    const std::vector<std::uint8_t> bank = mini::ReadBytes(proj.out() / "Main.sndb");
    root = proj.root() / "game";
    mini::WriteBytes(root / "banks" / "Main.sndb", bank);
    mini::WriteBytes(root / "C:Main.sndb", bank);                  // a legal name on POSIX
    mini::WriteBytes(proj.root() / "outside" / "Main.sndb", bank);  // next to the root
    std::error_code ec;
    fsys::create_symlink("../outside/Main.sndb", root / "link_out.sndb", ec);
    linksOk = !ec;
    fsys::create_symlink("banks/Main.sndb", root / "link_in.sndb", ec);
    linksOk = linksOk && !ec;
    fsys::create_directory_symlink("../outside", root / "dir_out", ec);
    linksOk = linksOk && !ec;
  }
  mini::Project proj;
  fsys::path root;
  bool built = false;
  bool linksOk = false;
};

snd_result CreateOn(const char* root, const snd_file_system* fs, snd_system** sys) {
  snd_config c = SmallConfig();
  c.output_kind = SND_OUTPUT_MANUAL;
  c.file_system_root = root;
  return snd_create(&c, nullptr, fs, sys);
}

// Loads `path` and runs the system until the load finished. Returns the final state.
snd_bank_state Load(snd_system* sys, const char* path, snd_bank_handle* out = nullptr) {
  const snd_bank_handle h = snd_load_bank_async(sys, path);
  if (out != nullptr) *out = h;
  if (h.value == 0u) return SND_BANK_STATE_FAILED;
  for (int i = 0; i < 4000; ++i) {
    snd_update(sys);
    const snd_bank_state s = snd_get_bank_state(sys, h);
    if (s == SND_BANK_STATE_LOADED || s == SND_BANK_STATE_FAILED) return s;
    snd_advance_output(sys, 1, nullptr, 0);
  }
  return snd_get_bank_state(sys, h);
}

bool Unload(snd_system* sys, snd_bank_handle h) {
  if (snd_unload_bank(sys, h, SND_UNLOAD_STOP_AND_UNLOAD) != SND_OK) return false;
  for (int i = 0; i < 4000; ++i) {
    snd_update(sys);
    if (snd_get_bank_state(sys, h) == SND_BANK_STATE_UNLOADED) return true;
    snd_advance_output(sys, 1, nullptr, 0);
  }
  return false;
}

}  // namespace

SND_TEST(ApiHostFs, PathsStayInsideTheRoot) {
  Disk d;
  SND_REQUIRE(d.built);
  SND_REQUIRE(d.linksOk);
  const std::string root = d.root.string();
  snd_system* sys = nullptr;
  SND_REQUIRE_EQ(CreateOn(root.c_str(), nullptr, &sys), SND_OK);
  // Every one of these names an existing bank file; each is refused by the rule.
  const std::string abs = (d.root / "banks" / "Main.sndb").string();
  const std::string outsideAbs = (d.proj.root() / "outside" / "Main.sndb").string();
  const char* refused[] = {
      "../outside/Main.sndb",          // ".." above the root
      "banks/../../outside/Main.sndb",  // the same, later in the path
      abs.c_str(),                     // absolute, even inside the root
      outsideAbs.c_str(),              // absolute outside
      "link_out.sndb",                 // a link whose target lies outside
      "dir_out/Main.sndb",             // through a directory link that leaves the root
      "C:Main.sndb",                   // colons (drive letters / streams) on every OS
      "banks\\Main.sndb",              // backslash
  };
  for (const char* p : refused) {
    snd_bank_handle fh{};
    SND_CHECK_MSG(Load(sys, p, &fh) == SND_BANK_STATE_FAILED, "%s", p);
    SND_CHECK(fh.value != 0u);
    if (fh.value != 0u) SND_CHECK(Unload(sys, fh));  // a failed bank keeps its slot until unloaded
  }
  // A link that stays inside is followed; the plain path loads.
  snd_bank_handle h{};
  SND_CHECK_EQ(Load(sys, "link_in.sndb", &h), SND_BANK_STATE_LOADED);
  SND_REQUIRE(Unload(sys, h));
  SND_CHECK_EQ(Load(sys, "banks/./Main.sndb", &h), SND_BANK_STATE_LOADED);
  snd_destroy(sys);
}

SND_TEST(ApiHostFs, RelativeRootIsResolvedAtCreate) {
  Disk d;
  SND_REQUIRE(d.built);
  std::error_code ec;
  const fsys::path before = fsys::current_path(ec);
  SND_REQUIRE(!ec);
  fsys::current_path(d.proj.root(), ec);
  SND_REQUIRE(!ec);
  snd_system* sys = nullptr;
  const snd_result r = CreateOn("game", nullptr, &sys);
  fsys::current_path("/", ec);  // the working directory moves away after snd_create
  if (r == SND_OK) {
    SND_CHECK_EQ(Load(sys, "banks/Main.sndb"), SND_BANK_STATE_LOADED);
    snd_destroy(sys);
  }
  fsys::current_path(before, ec);
  SND_CHECK_EQ(r, SND_OK);
  // NULL and "" are the working directory, as before 1.2.
  fsys::current_path(d.root, ec);
  SND_REQUIRE(!ec);
  for (const char* root : {static_cast<const char*>(nullptr), ""}) {
    snd_system* s2 = nullptr;
    SND_CHECK_EQ(CreateOn(root, nullptr, &s2), SND_OK);
    if (s2 != nullptr) {
      SND_CHECK_EQ(Load(s2, "banks/Main.sndb"), SND_BANK_STATE_LOADED);
      snd_destroy(s2);
    }
  }
  fsys::current_path(before, ec);
}

SND_TEST(ApiHostFs, ConfigRules) {
  Disk d;
  snd_system* sys = reinterpret_cast<snd_system*>(1);
  // Not an existing directory: NOT_FOUND (a missing one, a file).
  const std::string missing = (d.proj.root() / "no-such-dir").string();
  SND_CHECK_EQ(CreateOn(missing.c_str(), nullptr, &sys), SND_ERR_NOT_FOUND);
  SND_CHECK(sys == nullptr);
  const std::string file = (d.root / "banks" / "Main.sndb").string();
  SND_CHECK_EQ(CreateOn(file.c_str(), nullptr, &sys), SND_ERR_NOT_FOUND);
  // A root with the game's file system means nothing: refused.
  snd::mock::MemoryFileSystem mfs;
  const snd_file_system ft = snd::ToC(mfs);
  const std::string root = d.root.string();
  SND_CHECK_EQ(CreateOn(root.c_str(), &ft, &sys), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(CreateOn("", &ft, &sys), SND_OK);  // "" is "no root"
  snd_destroy(sys);
  sys = nullptr;
  // Longer than the built-in file system takes (2046 bytes): INVALID_ARGUMENT.
  const std::string tooLong(2047, 'a');
  SND_CHECK_EQ(CreateOn(tooLong.c_str(), nullptr, &sys), SND_ERR_INVALID_ARGUMENT);
  const std::string longest(2046, 'a');  // accepted by the check, then not a directory
  SND_CHECK_EQ(CreateOn(longest.c_str(), nullptr, &sys), SND_ERR_NOT_FOUND);
  // The string may go away after snd_create (the root is copied).
  {
    std::string copy = root;
    SND_REQUIRE_EQ(CreateOn(copy.c_str(), nullptr, &sys), SND_OK);
    copy.assign(copy.size(), 'x');
  }
  SND_CHECK_EQ(Load(sys, "banks/Main.sndb"), SND_BANK_STATE_LOADED);
  snd_destroy(sys);
}
