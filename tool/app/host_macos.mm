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
// Keys: App::Key (Cmd as Ctrl): Cmd+Z / Cmd+Shift+Z, Cmd+S, F2..F10 screens, the text entry.
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

// Every key goes to App::Key (shortcuts, screen keys and the tool's text entry): the key code and
// the character it typed.
void Key(ui::App& app, NSEvent* event) {
  ui::KeyEvent k;
  if ((event.modifierFlags & NSEventModifierFlagShift) != 0) k.modifiers |= ui::kModShift;
  if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) k.modifiers |= ui::kModControl;
  switch (event.keyCode) {
    case 36: case 76: k.key = ui::kKeyEnter; break;
    case 53: k.key = ui::kKeyEscape; break;
    case 51: k.key = ui::kKeyBackspace; break;
    case 117: k.key = ui::kKeyDelete; break;
    case 48: k.key = ui::kKeyTab; break;
    case 123: k.key = ui::kKeyLeft; break;
    case 124: k.key = ui::kKeyRight; break;
    case 125: k.key = ui::kKeyDown; break;
    case 126: k.key = ui::kKeyUp; break;
    case 115: k.key = ui::kKeyHome; break;
    case 119: k.key = ui::kKeyEnd; break;
    case 116: k.key = ui::kKeyPageUp; break;
    case 121: k.key = ui::kKeyPageDown; break;
    case 122: k.key = ui::kKeyF1; break;
    case 120: k.key = ui::kKeyF1 + 1u; break;
    case 99: k.key = ui::kKeyF1 + 2u; break;
    case 118: k.key = ui::kKeyF1 + 3u; break;
    case 96: k.key = ui::kKeyF1 + 4u; break;
    case 97: k.key = ui::kKeyF1 + 5u; break;
    case 98: k.key = ui::kKeyF1 + 6u; break;
    case 100: k.key = ui::kKeyF1 + 7u; break;
    case 101: k.key = ui::kKeyF1 + 8u; break;
    case 109: k.key = ui::kKeyF1 + 9u; break;
    default: break;
  }
  NSString* plain = event.charactersIgnoringModifiers;
  if (k.key == 0u && plain.length == 1) {
    const unichar c = [plain.uppercaseString characterAtIndex:0];
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) k.key = c;
  }
  NSString* typed = event.characters;
  if (typed.length >= 1 && k.key != ui::kKeyEnter && k.key != ui::kKeyBackspace && k.key != ui::kKeyEscape &&
      k.key != ui::kKeyTab && k.key != ui::kKeyDelete) {
    unichar c = [typed characterAtIndex:0];
    std::uint32_t cp = c;
    if (c >= 0xD800 && c <= 0xDBFF && typed.length >= 2) {
      const unichar lo = [typed characterAtIndex:1];
      cp = 0x10000u + ((static_cast<std::uint32_t>(c) - 0xD800u) << 10) + (static_cast<std::uint32_t>(lo) - 0xDC00u);
    }
    if (cp >= 0x20u && !(cp >= 0xF700u && cp <= 0xF8FFu)) k.text = cp;  // not the function-key range
  }
  app.Key(k);
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
