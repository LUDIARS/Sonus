// macOS host: an ordinary NSWindow of this application with Tela's AppKit
// overlay (tela::MacOSOverlay) following it, drawn with Tela's Pictor surface.
// Tela has no normal-window view on macOS (only overlays), so the window is the
// overlay's target. Tela feeds button presses to the runtime through its input
// panels; a local NSEvent monitor observes pointer positions, the scroll wheel
// and shortcuts for the drawn controls (pointer_router.hpp). The monitor runs
// before Tela's panels see an event, so a drag starts on the first move after
// Tela granted the press.
//
// The overlay covers the target's whole frame (title bar included); the
// declaration leaves the title bar free with AppDesc::insetTop.
//
// Compiled with exceptions (-fexceptions, ARC): Tela's AppKit adapter throws.
// Shortcuts: Cmd+Z undo, Cmd+Shift+Z redo, F2 mixer, F3 profiler.
#import <AppKit/AppKit.h>

#include <cmath>
#include <cstdio>
#include <exception>

#include <tela/macos_overlay.hpp>
#include <tela/pictor_surface.hpp>

#include "host.hpp"

namespace snd::tool::app {
namespace {

struct MacPointer {
  ui::App* app = nullptr;
  std::uint64_t sequence = 0;
  std::uint64_t gesture = 0;
};

tela::HostPointerEvent MakeEvent(MacPointer& mp, NSEvent* event, tela::PointerPhase phase) {
  tela::HostPointerEvent e;
  e.sequence = ++mp.sequence;
  if (phase == tela::PointerPhase::down) ++mp.gesture;
  e.gesture_id = mp.gesture;
  e.viewport_revision = mp.app->runtime().viewport().revision;
  e.phase = phase;
  e.button = phase == tela::PointerPhase::move || phase == tela::PointerPhase::wheel ? tela::PointerButton::none
                                                                                     : tela::PointerButton::primary;
  // Same basis as Tela's PointerSession: global points from the primary display's top,
  // times the viewport's backing scale.
  const NSPoint point =
      event.window != nil ? [event.window convertPointToScreen:event.locationInWindow] : NSEvent.mouseLocation;
  const CGFloat top = NSScreen.screens.count != 0 ? NSMaxY(NSScreen.screens.firstObject.frame) : 0.0;
  const float scale = mp.app->runtime().viewport().dpi_scale > 0.0f ? mp.app->runtime().viewport().dpi_scale : 1.0f;
  e.desktop_x = static_cast<int>(std::lround(point.x * scale));
  e.desktop_y = static_cast<int>(std::lround((top - point.y) * scale));
  const NSEventModifierFlags flags = event.modifierFlags;
  if ((flags & NSEventModifierFlagShift) != 0) e.modifiers |= ui::kModShift;
  if ((flags & NSEventModifierFlagCommand) != 0) e.modifiers |= ui::kModControl;
  if (phase == tela::PointerPhase::wheel) {
    const CGFloat dy = event.scrollingDeltaY;
    e.wheel_y = static_cast<float>(event.hasPreciseScrollingDeltas ? dy / 10.0 : dy);
  }
  return e;
}

void Key(ui::App& app, NSEvent* event) {
  const bool command = (event.modifierFlags & NSEventModifierFlagCommand) != 0;
  const bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
  NSString* chars = event.charactersIgnoringModifiers;
  if (command && chars.length == 1 && [chars.lowercaseString isEqualToString:@"z"]) {
    if (shift) {
      app.OnRedo();
    } else {
      app.OnUndo();
    }
  } else if (event.keyCode == 120) {  // F2
    app.OnScreen(ui::Screen::Mixer);
  } else if (event.keyCode == 99) {  // F3
    app.OnScreen(ui::Screen::Profiler);
  }
}

}  // namespace

int RunMacOS(ui::App& app, const Options& o) {
  if (o.font.empty()) {
    std::fprintf(stderr, "sonus-tool: --font <file.ttf> is required (Tela does not bundle a font)\n");
    return 2;
  }
  @autoreleasepool {
    try {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
      [NSApp finishLaunching];
      // --size is in physical pixels elsewhere; on macOS the window is sized in points.
      const NSRect frame = NSMakeRect(80, 80, o.width, o.height);
      NSWindow* window = [[NSWindow alloc]
          initWithContentRect:frame
                    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable)
                      backing:NSBackingStoreBuffered
                        defer:NO];
      window.title = @"Sonus Tool";
      window.releasedWhenClosed = NO;
      window.backgroundColor = [NSColor colorWithCalibratedRed:0.08 green:0.10 blue:0.14 alpha:1.0];
      [window makeKeyAndOrderFront:nil];
      [NSApp activateIgnoringOtherApps:YES];
      app.SetInsetTop(static_cast<float>(window.frame.size.height - window.contentLayoutRect.size.height));

      tela::PictorSurface surface(o.font);
      tela::MacOSOverlay overlay(app.runtime(), surface, static_cast<std::uint32_t>(window.windowNumber));
      MacPointer pointer;
      pointer.app = &app;
      MacPointer* mp = &pointer;
      id monitor = [NSEvent
          addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                                                NSEventMaskLeftMouseDragged | NSEventMaskScrollWheel |
                                                NSEventMaskKeyDown)
                                       handler:^NSEvent*(NSEvent* event) {
                                         const std::uint64_t ms = NowNs() / 1000000u;
                                         switch (event.type) {
                                           case NSEventTypeLeftMouseDown:
                                             mp->app->Pointer(MakeEvent(*mp, event, tela::PointerPhase::down), ms, false);
                                             break;
                                           case NSEventTypeLeftMouseDragged:
                                             mp->app->Pointer(MakeEvent(*mp, event, tela::PointerPhase::move), ms, false);
                                             break;
                                           case NSEventTypeLeftMouseUp:
                                             mp->app->Pointer(MakeEvent(*mp, event, tela::PointerPhase::up), ms, false);
                                             break;
                                           case NSEventTypeScrollWheel:
                                             mp->app->Pointer(MakeEvent(*mp, event, tela::PointerPhase::wheel), ms, false);
                                             break;
                                           case NSEventTypeKeyDown:
                                             Key(*mp->app, event);
                                             break;
                                           default: break;
                                         }
                                         return event;
                                       }];
      while (window.visible) {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.016];
        NSEvent* event = nil;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:until
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]) != nil) {
          [NSApp sendEvent:event];
          until = [NSDate distantPast];
        }
        app.Frame(NowNs());
        overlay.synchronize();
      }
      [NSEvent removeMonitor:monitor];
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "sonus-tool: %s\n", e.what());
      return 1;
    } catch (...) {
      std::fprintf(stderr, "sonus-tool: unknown error from the Tela host\n");
      return 1;
    }
  }
}

}  // namespace snd::tool::app
