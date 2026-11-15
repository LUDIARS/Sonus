// io module (design.md 12.1): default IFileSystem, IO scheduler (priority and bandwidth).
// Dependencies (frozen, root CMakeLists.txt): base, jobs.
//
// Stage-0 stub. The owner of this module adds its headers under
// io/include/snd/io/ and its sources under io/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_IO_IO_MODULE_HPP
#define SND_IO_IO_MODULE_HPP

namespace snd::io {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::io

#endif  // SND_IO_IO_MODULE_HPP
