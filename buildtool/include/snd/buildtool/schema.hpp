// The members of the project format the build reads (the schema behind its "unknown member"
// warnings, project-format.md §1), for tools that must cover every member (the authoring
// tool's member -> bank table, D-178b).
#ifndef SND_BUILDTOOL_SCHEMA_HPP
#define SND_BUILDTOOL_SCHEMA_HPP

#include <string>
#include <vector>

namespace snd::buildtool {

// Kind names ("project", "bank", ..., "patch").
std::vector<std::string> SchemaKinds();
// Every member path of a kind, leaves and containers: "tracks[].clips[].random.volumeDb",
// "single.*.impact" ("[]" = an array element, "*" = a member of a free-key object). Common
// members (type, guid, name, aliases, bank, notes) included.
std::vector<std::string> SchemaMembers(const std::string& kind);

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_SCHEMA_HPP
