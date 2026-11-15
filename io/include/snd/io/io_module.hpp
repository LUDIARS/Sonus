// io module (design.md 12.1): default IFileSystem, IO scheduler (priority and bandwidth).
// Dependencies (frozen, root CMakeLists.txt): base, jobs.
//
// Public headers (only contract and base types appear in them, D-02; platform
// and jobs stay inside io/src because the modules using io do not see them):
//   stream_pool.hpp          StreamPool: fixed chunks of Config.memory.streamPoolBytes
//   io_scheduler.hpp         IoScheduler (IO thread, priority + bandwidth), StreamChannel
//                            (compressed ring IO -> decode), FileRequest (bank loading)
//   stream_read_planner.hpp  StreamReadPlanner: chunked read sequence with loop wrap
//   default_file_system.hpp  DefaultFileSystem: IFileSystem over OS files (D-24)
//   io.hpp                   all of the above
#ifndef SND_IO_IO_MODULE_HPP
#define SND_IO_IO_MODULE_HPP

namespace snd::io {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::io

#endif  // SND_IO_IO_MODULE_HPP
