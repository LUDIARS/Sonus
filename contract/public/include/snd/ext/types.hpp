// C++ names for the extension payload types (design.md 12.3). FROZEN CONTRACT.
//
// POD payloads are aliases of the C structs in snd/ext.h and snd/types.h, so
// there is exactly one definition and no conversion between the C and C++
// APIs. Enumerations are `enum class` with the C constant values.
#ifndef SND_EXT_TYPES_HPP
#define SND_EXT_TYPES_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/ext.h"
#include "snd/types.h"

namespace snd {

inline constexpr std::uint32_t kMaxChannels = SND_MAX_CHANNELS;

// 3.3 memory classes.
enum class MemTag : std::uint32_t {
  Tables = SND_MEM_TABLES,
  FrameArena = SND_MEM_FRAME_ARENA,
  ThreadArena = SND_MEM_THREAD_ARENA,
  StreamPool = SND_MEM_STREAM_POOL,
  DecoderPool = SND_MEM_DECODER_POOL,
  DspPool = SND_MEM_DSP_POOL,
  PcmCache = SND_MEM_PCM_CACHE,
  Bank = SND_MEM_BANK,
  CommandArena = SND_MEM_COMMAND_ARENA,
  Notify = SND_MEM_NOTIFY,
  LiveLink = SND_MEM_LIVELINK,
  Misc = SND_MEM_MISC,
  Count = SND_MEM_TAG_COUNT,
};

// Jobs (2.3).
enum class JobLane : std::uint32_t {
  Prefetch = SND_JOB_LANE_PREFETCH,
  Background = SND_JOB_LANE_BACKGROUND,
  Count = SND_JOB_LANE_COUNT,
};
using JobFn = snd_job_fn;
using JobDesc = snd_job_desc;

// Job completion helpers implementing the JobDesc contract. Implementations of
// IJobSystem must use exactly these (or equivalent atomics).
inline bool IsJobCancelled(const JobDesc& job) noexcept {
  if (job.cancel_flag == nullptr) return false;
  // cancel_flag is const for the submitter's intent; the load itself does not write.
  std::atomic_ref<std::uint32_t> flag(*const_cast<std::uint32_t*>(job.cancel_flag));
  return flag.load(std::memory_order_acquire) != 0u;
}
inline void MarkJobDone(const JobDesc& job) noexcept {
  if (job.done_counter == nullptr) return;
  std::atomic_ref<std::uint32_t> counter(*job.done_counter);
  counter.fetch_sub(1u, std::memory_order_acq_rel);
}
// Runs one job per the contract: skip if cancelled, then signal completion.
inline void RunJob(const JobDesc& job) noexcept {
  if (!IsJobCancelled(job) && job.fn != nullptr) job.fn(job.data);
  MarkJobDone(job);
}

// File system (6.5).
using FileId = snd_file_id;
inline constexpr FileId kInvalidFile = SND_FILE_INVALID;
using IoToken = snd_io_token;
enum class IoState : std::uint32_t {
  Pending = SND_IO_PENDING,
  Done = SND_IO_DONE,
  Error = SND_IO_ERROR,
  UnknownToken = SND_IO_UNKNOWN_TOKEN,
};
using IoStatus = snd_io_status;
inline IoState StateOf(const IoStatus& s) noexcept { return static_cast<IoState>(s.state); }

// Codec (6.4).
using CodecHeader = snd_codec_header;
inline constexpr std::uint32_t kDecodeError = SND_DECODE_ERROR;

// DSP (4.6, 7).
using EffectDesc = snd_effect_desc;
using ParamBlock = snd_param_block;
using RenderContext = snd_render_context;

// Output device (2.2, 2.6).
using DeviceDesc = snd_device_desc;
using DeviceCallback = snd_device_callback;
using DeviceEvent = snd_device_event;
using DeviceEventFn = snd_device_event_fn;
enum class DeviceEventKind : std::uint32_t {
  Changed = SND_DEVICE_EVENT_CHANGED,
  Suspended = SND_DEVICE_EVENT_SUSPENDED,
  Resumed = SND_DEVICE_EVENT_RESUMED,
  Lost = SND_DEVICE_EVENT_LOST,
  FocusLost = SND_DEVICE_EVENT_FOCUS_LOST,
  FocusGained = SND_DEVICE_EVENT_FOCUS_GAINED,
};

constexpr std::uint32_t Fourcc(char a, char b, char c, char d) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

}  // namespace snd

#endif  // SND_EXT_TYPES_HPP
