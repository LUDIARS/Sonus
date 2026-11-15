// Contract 1.1 D-55: the public C / C++ API and every internal header compile
// together in one translation unit under the project's warnings (-Wshadow,
// -Werror). Stage 2's api reads both layers. Before 1.1, WhenKind::ClockSample
// shadowed snd::ClockSample here.
#include "snd/ext/allocator.hpp"
#include "snd/ext/codec.hpp"
#include "snd/ext/dsp_effect.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/ext/output_device.hpp"
#include "snd/format/bank_check.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/music_if.hpp"
#include "snd/internal/notify.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/internal/schema.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/physics.hpp"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

SND_TEST(Contract11, PublicAndInternalLayersCompileTogether) {
  static_assert(static_cast<unsigned>(snd::WhenKind::AtClockSample) == SND_WHEN_CLOCK_SAMPLE);
  const snd::ClockSample clock = 123;  // the internal type of the same former name
  SND_CHECK_EQ(snd::When::AtClockSample(clock).ToC().kind, SND_WHEN_CLOCK_SAMPLE);
  SND_CHECK(snd::When::AtClockSample(clock).Kind() == snd::WhenKind::AtClockSample);
}
