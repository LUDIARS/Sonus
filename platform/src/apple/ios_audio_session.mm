// AVAudioSession (iOS). Objective-C++ compiled with -fobjc-arc (ARC is
// assumed: no retain/release in this file; the C++ side holds the observer
// through CFBridgingRetain / CFBridgingRelease).
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <mutex>

#include "apple/ios_audio_session.hpp"
#include "backends.hpp"
#include "common/owned_string.hpp"

#if !__has_feature(objc_arc)
#error "ios_audio_session.mm must be compiled with -fobjc-arc"
#endif

using snd::platform::detail::coreaudio::IosAudioSession;

// Holds the NSNotificationCenter tokens. `_sink` is cleared under `_lock`
// before the tokens are removed, so a notification that races with
// StopObserving either finished forwarding or sees nil.
@interface SndSessionObserver : NSObject {
 @public
  std::mutex _lock;
  IosAudioSession* _sink;
  NSMutableArray* _tokens;
}
@end

@implementation SndSessionObserver
@end

namespace snd::platform::detail::coreaudio {

namespace {
NSString* CategoryFor(SessionCategory c) {
  switch (c) {
    case SessionCategory::Ambient: return AVAudioSessionCategoryAmbient;
    case SessionCategory::SoloAmbient: return AVAudioSessionCategorySoloAmbient;
    case SessionCategory::Playback: break;
  }
  return AVAudioSessionCategoryPlayback;
}

void ForwardFrom(SndSessionObserver* obs, std::uint32_t bits, std::uint32_t reason, bool resume) {
  std::lock_guard<std::mutex> g(obs->_lock);
  if (obs->_sink != nullptr) obs->_sink->Forward(bits, reason, resume);
}
}  // namespace

IosAudioSession::~IosAudioSession() { StopObserving(); }

bool IosAudioSession::Activate(SessionCategory category, std::uint32_t preferredRate,
                               std::uint32_t preferredFrames) noexcept {
  @autoreleasepool {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    NSError* err = nil;
    if (![s setCategory:CategoryFor(category) error:&err]) return false;
    if (preferredRate != 0u) [s setPreferredSampleRate:static_cast<double>(preferredRate) error:nil];
    if (preferredFrames != 0u) {
      const double rate = preferredRate != 0u ? static_cast<double>(preferredRate) : s.sampleRate;
      if (rate > 0.0) [s setPreferredIOBufferDuration:static_cast<double>(preferredFrames) / rate error:nil];
    }
    return [s setActive:YES error:&err] ? true : false;
  }
}

bool IosAudioSession::Reactivate() noexcept {
  @autoreleasepool {
    return [[AVAudioSession sharedInstance] setActive:YES error:nil] ? true : false;
  }
}

void IosAudioSession::Deactivate() noexcept {
  @autoreleasepool {
    [[AVAudioSession sharedInstance] setActive:NO
                                   withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                         error:nil];
  }
}

IosSessionInfo IosAudioSession::Query() const noexcept {
  IosSessionInfo info;
  @autoreleasepool {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    const double rate = s.sampleRate;
    info.sampleRate = rate > 0.0 ? static_cast<std::uint32_t>(rate + 0.5) : 0u;
    info.outputChannels = static_cast<std::uint32_t>(s.outputNumberOfChannels);
    info.ioBufferFrames = static_cast<std::uint32_t>(s.IOBufferDuration * rate + 0.5);
    info.outputLatency = s.outputLatency;
  }
  return info;
}

void IosAudioSession::Forward(std::uint32_t bits, std::uint32_t routeReason, bool shouldResume) noexcept {
  if ((bits & kIosRouteChanged) != 0u) routeReason_.store(routeReason, std::memory_order_release);
  if ((bits & kIosInterruptEnded) != 0u) shouldResume_.store(shouldResume, std::memory_order_release);
  if ((bits & (kIosInterruptBegan | kIosInterruptEnded)) != 0u) {
    lastBegan_.store((bits & kIosInterruptBegan) != 0u, std::memory_order_release);
  }
  sinkFn_(sinkUser_, bits);
}

bool IosAudioSession::StartObserving(IosSessionFn fn, void* user) noexcept {
  StopObserving();
  sinkFn_ = fn;
  sinkUser_ = user;
  @autoreleasepool {
    SndSessionObserver* obs = [[SndSessionObserver alloc] init];
    obs->_sink = this;
    obs->_tokens = [NSMutableArray array];
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    AVAudioSession* s = [AVAudioSession sharedInstance];
    __weak SndSessionObserver* weakObs = obs;
    [obs->_tokens addObject:[nc addObserverForName:AVAudioSessionInterruptionNotification
                                            object:s
                                             queue:nil
                                        usingBlock:^(NSNotification* note) {
                                          SndSessionObserver* o = weakObs;
                                          if (o == nil) return;
                                          NSNumber* type = note.userInfo[AVAudioSessionInterruptionTypeKey];
                                          NSNumber* opts = note.userInfo[AVAudioSessionInterruptionOptionKey];
                                          const bool began = type != nil && type.unsignedIntegerValue ==
                                                                                AVAudioSessionInterruptionTypeBegan;
                                          const bool resume = opts != nil &&
                                                              (opts.unsignedIntegerValue &
                                                               AVAudioSessionInterruptionOptionShouldResume) != 0;
                                          ForwardFrom(o, began ? kIosInterruptBegan : kIosInterruptEnded, 0, resume);
                                        }]];
    [obs->_tokens addObject:[nc addObserverForName:AVAudioSessionRouteChangeNotification
                                            object:s
                                             queue:nil
                                        usingBlock:^(NSNotification* note) {
                                          SndSessionObserver* o = weakObs;
                                          if (o == nil) return;
                                          NSNumber* reason = note.userInfo[AVAudioSessionRouteChangeReasonKey];
                                          ForwardFrom(o, kIosRouteChanged,
                                                      reason != nil ? static_cast<std::uint32_t>(
                                                                          reason.unsignedIntegerValue)
                                                                    : 0u,
                                                      false);
                                        }]];
    [obs->_tokens addObject:[nc addObserverForName:AVAudioSessionMediaServicesWereLostNotification
                                            object:s
                                             queue:nil
                                        usingBlock:^(NSNotification*) {
                                          SndSessionObserver* o = weakObs;
                                          if (o != nil) ForwardFrom(o, kIosMediaLost, 0, false);
                                        }]];
    [obs->_tokens addObject:[nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                                            object:s
                                             queue:nil
                                        usingBlock:^(NSNotification*) {
                                          SndSessionObserver* o = weakObs;
                                          if (o != nil) ForwardFrom(o, kIosMediaReset, 0, false);
                                        }]];
    observer_ = (void*)CFBridgingRetain(obs);
  }
  return true;
}

void IosAudioSession::StopObserving() noexcept {
  if (observer_ == nullptr) return;
  @autoreleasepool {
    SndSessionObserver* obs = (SndSessionObserver*)CFBridgingRelease(observer_);
    observer_ = nullptr;
    {
      std::lock_guard<std::mutex> g(obs->_lock);
      obs->_sink = nullptr;  // no forward starts after this
    }
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    for (id token in obs->_tokens) [nc removeObserver:token];
    [obs->_tokens removeAllObjects];
  }
}

}  // namespace snd::platform::detail::coreaudio

namespace snd::platform::detail {

// iOS has one output: the session's current route.
std::uint32_t EnumerateCoreAudioDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept {
  if (capacity > 0u && out != nullptr) {
    out[0] = OutputDeviceInfo{};
    CopyTruncatedUtf8(out[0].id, sizeof(out[0].id), "default");
    @autoreleasepool {
      AVAudioSessionRouteDescription* route = [AVAudioSession sharedInstance].currentRoute;
      AVAudioSessionPortDescription* port = route.outputs.firstObject;
      const char* name = port != nil ? port.portName.UTF8String : "Current route";
      CopyTruncatedUtf8(out[0].name, sizeof(out[0].name), name != nullptr ? name : "Current route");
    }
    out[0].isDefault = true;
  }
  return 1u;
}

}  // namespace snd::platform::detail
