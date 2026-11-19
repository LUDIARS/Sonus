// D-104: a name whose id is one of the runtime's reserved ids (SND_ID_RESERVED_FIRST ..
// LAST, snd/id.h) is a build error, whatever kind the object is. The names below were
// found by search: FNV-1a of "p0055faf188" is 0xFFFFFFFD (the playback volume), of
// "p0010848619" 0xFFFFFFFE (the distance input).
#include <string>

#include "mini_project.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;

SND_TEST(BuildReservedId, NamesHashingToAReservedIdAreRefused) {
  SND_REQUIRE_EQ(snd::HashId("p0055faf188"), 0xFFFFFFFDu);
  SND_REQUIRE_EQ(snd::HashId("p0010848619"), 0xFFFFFFFEu);
  for (const char* name : {"p0055faf188", "p0010848619"}) {
    for (const char* kind : {"parameter", "event"}) {
      mini::Project p(std::string("reserved-") + kind);
      p.Minimal();
      if (std::string(kind) == "parameter") {
        p.Add(std::string("parameters/") + name + ".json", "parameter", name);
      } else {
        p.Add(std::string("events/") + name + ".json", "event", name,
              "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
      }
      bt::Diagnostics d;
      const bool ok = p.Build(&d);
      SND_CHECK_MSG(!ok && mini::HasDiag(d, bt::Severity::Error, "reserved id"), "%s %s: not refused\n%s", kind, name,
                    mini::Dump(d).c_str());
    }
  }
  // An alias too.
  mini::Project p("reserved-alias");
  p.Minimal();
  p.Add("parameters/K.json", "parameter", "K", "\"aliases\": [\"p0055faf188\"]");
  bt::Diagnostics d;
  SND_CHECK(!p.Build(&d) && mini::HasDiag(d, bt::Severity::Error, "reserved id"));
  // The minimal project itself builds.
  mini::Project q("reserved-none");
  q.Minimal();
  bt::Diagnostics dq;
  SND_CHECK_MSG(q.Build(&dq), "%s", mini::Dump(dq).c_str());
}
